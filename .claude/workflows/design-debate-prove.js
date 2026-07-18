export const meta = {
  name: 'design-debate-prove',
  description: 'Competing designs for a Quark subsystem debate, defend against each other, then get proven or disproven with real compiled+executed C++ (sanitizers + benchmarks) before a judge picks a winner and writes an ADR.',
  whenToUse: 'A Quark actor-engine design decision needs competing proposals, adversarial defense, and empirical proof before a spec is promoted from Draft to Accepted. Pass args to target a specific subsystem/claim; defaults to the Mailbox MPSC hot path.',
  phases: [
    { title: 'Design', detail: 'N architects propose competing designs, each with falsifiable fast/safe/correct claims' },
    { title: 'Cross-examine', detail: 'Each design is red-teamed; its author rebuts; surviving claims are extracted' },
    { title: 'Prove', detail: 'A prover implements each design in C++23 and runs sanitizers + benchmarks to mark every claim CORRECT/WRONG/INCONCLUSIVE' },
    { title: 'Judge', detail: 'Synthesize defended designs + executed evidence into a winner, an ADR, and spec recommendations' },
  ],
}

// ---------------------------------------------------------------------------
// Target: what the debate is about. Override by passing `args`, e.g.
//   { title, question, specs: [...], invariants: [...], angles: [...], designs: 3 }
// Defaults to the Mailbox MPSC hot path (specs 002 + 003).
// ---------------------------------------------------------------------------
// `args` may arrive as an object or as a JSON string depending on how the run
// was launched — normalize both to an object so overrides always apply.
let T = args
if (typeof T === 'string') { try { T = JSON.parse(T) } catch (e) { T = {} } }
if (typeof T !== 'object' || !T) T = {}
const TARGET = {
  title: T.title || 'Mailbox MPSC hot path',
  question: T.question || [
    'Design the actor Mailbox: the MPSC queue that owns FIFO message ordering.',
    'It stores fixed-size MessageHandles (never payloads). Many producer threads',
    '(tell/ask from any worker) enqueue; exactly one worker drains it at a time,',
    'guaranteed by the actor exec-state CAS (the single-executor invariant).',
    'Prove it is (fast) allocation-free on the steady-state hot path and scales',
    'under high producer contention, (safe) free of data races / UB / ABA under',
    'TSan+ASan+UBSan, and (correct) strictly FIFO with no lost or duplicated',
    'handles and tombstones skipped exactly once.',
  ].join(' '),
  specs: T.specs || ['003-Memory.md', '002-Scheduler.md', '001-Actor-Execution-Model.md', '015-Reentrancy-and-Quiescence.md'],
  invariants: T.invariants || [
    'At most one executor (consumer) per actor at any instant.',
    'Mailbox ordering is FIFO by default.',
    'No heap allocation on the steady-state hot path.',
    'No data race / UB under ThreadSanitizer + Address/UB sanitizer.',
  ],
}
const ANGLES = T.angles || [
  'Intrusive lock-free MPSC (Vyukov-style single-consumer queue): producers CAS a shared tail, the single consumer walks a private head; nodes are pooled descriptors, no per-enqueue allocation.',
  'Segmented bounded ring buffer with atomic head/tail and a fixed-capacity backing array per segment; enqueue is a fetch-add into a slot, drain is a batch sweep; overflow allocates a new segment (measure that it never happens in steady state).',
  'Hybrid: lock-free multi-producer enqueue onto a stack (LIFO push via CAS) reversed once by the single consumer into a private FIFO drain list — trades a reverse for a cheaper, ABA-resistant producer path.',
]
const N = Math.min(ANGLES.length, T.designs || 3)

const specList = TARGET.specs.map(s => '`' + s + '`').join(', ')
const invList = TARGET.invariants.map(i => '- ' + i).join('\n')

// ---------------------------------------------------------------------------
// Role preambles. Embedded here so the workflow is self-contained and does not
// depend on the `.claude/agents/quark-*.md` types being registered (they aren't
// hot-loaded mid-session). The .md files remain the canonical, editable role
// docs; keep these in sync with them.
// ---------------------------------------------------------------------------
const GROUND = [
  `You are working on Quark, a high-performance C++23 actor engine. Ground rules that NO design may violate:`,
  `- C++23 std-only core (std::expected, coroutines quark::task<>, std::stop_token, std::pmr shard allocators, concepts + deducing-this). No RTTI/reflection on the hot path. OS specifics only behind the PAL.`,
  `- Core invariants: at most one executor per actor at any instant; mailbox FIFO by default; scheduler schedules activations not messages; stable placement (ActorId -> shard); workers are lanes not owners.`,
  `- Zero-cost hot path: no heap allocation, no reflection, no virtual dispatch for policy, no dynamic resource resolution while a message is processed.`,
  `- No .NET / managed-runtime vocabulary; express everything in idiomatic C++.`,
].join('\n')

const ARCHITECT_ROLE = [
  `ROLE: systems architect. READ the named spec files in the repo root first and use their exact vocabulary.`,
  `Produce ONE concrete design (not a survey) pushed to its strongest form: real memory layout, atomics + memory orders, a real/near-real C++23 hot path, and how each invariant is upheld.`,
  `State your position as FALSIFIABLE claims. Each claim is kind fast (measurable by benchmark), safe (checkable by sanitizer/stress), or correct (checkable by test). For EVERY claim give howToFalsify: the concrete runnable experiment that would prove you WRONG. A claim with no falsifying experiment is not a claim — drop it. Be honest in risks.`,
  GROUND,
].join('\n\n')

const REBUT_ROLE = [
  `ROLE: you are the AUTHOR of the design under attack (defense mode). Concede attacks that are right (conceding a fatal flaw is a correct outcome, not a loss) and rebut those that are wrong with precise counter-arguments and revised design detail. Output only the claims that SURVIVE scrutiny, each with the exact C++ experiment that will prove it. Do not smuggle a conceded claim back in.`,
  GROUND,
].join('\n\n')

const REDTEAM_ROLE = [
  `ROLE: red team. Your loyalty is to the running machine, not any design. Steelman the design in one line, then attack its fast/safe/correct claims hardest where they are proud. Check the known failure modes: data races / wrong memory orders / UB / use-after-free of pooled descriptors; ABA on CAS loops and unsafe reclamation; FIFO or single-consumer violations; reorders across co_await; hidden allocations claimed zero (arena growth, std::function, node alloc, pmr upstream fallback); false sharing; "wait-free"/"scales linearly" claims that actually serialize on one cache line; tombstone double-free / skipped-twice.`,
  `Rate each attack fatal/serious/minor. For each, give executableCheck: the specific test/sanitizer/benchmark that would DEMONSTRATE the flaw. An objection with no such check is an opinion — downgrade it. You may compile small probes yourself to confirm before asserting. Do not invent flaws to seem thorough; a clean design honestly reported is valid.`,
  GROUND,
].join('\n\n')

const PROVER_ROLE = [
  `ROLE: prover — the empirical judge. You do not argue; you run code. Every verdict is backed by a command and its observed output, never reasoning alone.`,
  `Work in a fresh scratch dir: BUILD="$(mktemp -d /tmp/quark-prove.XXXXXX)"; cd "$BUILD". Never write build artifacts into the repo. Toolchain: g++ (GCC 14) and clang++ (Clang 20), both -std=c++23.`,
  `Procedure: (1) implement the design faithfully in real C++23 — real atomics, memory orders, pooling; if under-specified make the most charitable choice and note it. (2) Build under BOTH g++ and clang++ with -O2 -Wall -Wextra; if it doesn't compile clean under both, buildOk:false and report errors. (3) Prove each claim with the matching tool: safe -> rebuild with -fsanitize=thread and -fsanitize=address,undefined and run a multi-producer/single-consumer stress loop (sanitizer report = WRONG); fast -> write a benchmark measuring exactly what the claim asserts (ops/s, ns/op, allocation count, producer-count sweep) and pin the number; correct -> an assertion test (FIFO, count conserved, no dup/skip). (4) Verdict per claim CORRECT/WRONG/INCONCLUSIVE with the counterexample/number. (5) Report benchmark numbers even for passing claims — the judge compares designs on them.`,
  `Rules: no evidence, no CORRECT. Prefer determinism (fixed counts, enough iterations for stable timing, report methodology). Re-run intermittent races many times and report hit rate (still WRONG). If you weakened the design to compile, say so and mark affected claims INCONCLUSIVE. artifactPath = the scratch dir.`,
  GROUND,
].join('\n\n')

const JUDGE_ROLE = [
  `ROLE: judge closing the debate. Ranking, in order: (1) Safety is a GATE — any safe/correct claim marked WRONG by the prover disqualifies that design from winning unless a stated cheap fix exists. (2) Proven beats claimed — count only claims that BOTH survived red-teaming AND were proven CORRECT with executed evidence; INCONCLUSIVE carries no weight; disproven counts against. (3) Among safe survivors prefer the best MEASURED hot-path numbers. (4) A design that bends a core invariant does not win.`,
  `Output: name the winner with a rationale citing specific proven claims and numbers. Write a decision record to decisions/ADR-<nnn>-<slug>.md in the repo (create decisions/ if absent; pick the next number by listing it) containing the question, one-line design summaries, an evidence table (claim -> survived? -> proven? -> number), the decision, and residual risks. List spec recommendations (which spec file changes and how) and residual risks. Be decisive; if evidence is genuinely insufficient, say so and name the single tie-breaking experiment.`,
  GROUND,
].join('\n\n')

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------
const CLAIM = {
  type: 'object', additionalProperties: false,
  required: ['id', 'kind', 'statement', 'howToFalsify'],
  properties: {
    id: { type: 'string', description: 'short stable id, e.g. F1, S2, C3' },
    kind: { enum: ['fast', 'safe', 'correct'] },
    statement: { type: 'string' },
    howToFalsify: { type: 'string', description: 'concrete runnable experiment that would prove this WRONG' },
  },
}
const DESIGN_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['name', 'summary', 'dataStructure', 'hotPathSketch', 'claims', 'risks'],
  properties: {
    name: { type: 'string' },
    summary: { type: 'string' },
    dataStructure: { type: 'string', description: 'concrete memory layout, atomics, and their memory orders' },
    hotPathSketch: { type: 'string', description: 'real or near-real C++23 for the enqueue + dequeue/drain path' },
    claims: { type: 'array', minItems: 2, items: CLAIM },
    risks: { type: 'array', items: { type: 'string' } },
  },
}
const ATTACK_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['steelman', 'attacks', 'overallThreat'],
  properties: {
    steelman: { type: 'string' },
    attacks: {
      type: 'array', items: {
        type: 'object', additionalProperties: false,
        required: ['targetClaimId', 'severity', 'argument', 'executableCheck'],
        properties: {
          targetClaimId: { type: 'string' },
          severity: { enum: ['fatal', 'serious', 'minor'] },
          argument: { type: 'string' },
          executableCheck: { type: 'string', description: 'test/sanitizer/benchmark that would demonstrate the flaw' },
        },
      },
    },
    overallThreat: { type: 'string' },
  },
}
const REBUT_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['survivingClaims', 'conceded', 'revisions'],
  properties: {
    survivingClaims: {
      type: 'array', items: {
        type: 'object', additionalProperties: false,
        required: ['id', 'kind', 'statement', 'experiment'],
        properties: {
          id: { type: 'string' },
          kind: { enum: ['fast', 'safe', 'correct'] },
          statement: { type: 'string' },
          experiment: { type: 'string', description: 'exact C++ experiment that will prove it in the Prove phase' },
        },
      },
    },
    conceded: { type: 'array', items: { type: 'string' }, description: 'claim ids withdrawn under attack' },
    revisions: { type: 'string', description: 'design changes made to close holes' },
  },
}
const PROOF_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['designName', 'buildOk', 'compilers', 'verdicts', 'benchmark', 'sanitizers', 'artifactPath'],
  properties: {
    designName: { type: 'string' },
    buildOk: { type: 'boolean' },
    compilers: { type: 'array', items: { type: 'string' }, description: 'e.g. ["g++ 14", "clang++ 20"]' },
    verdicts: {
      type: 'array', items: {
        type: 'object', additionalProperties: false,
        required: ['claimId', 'result', 'evidence'],
        properties: {
          claimId: { type: 'string' },
          result: { enum: ['CORRECT', 'WRONG', 'INCONCLUSIVE'] },
          evidence: { type: 'string', description: 'command run + observed output / measured number' },
        },
      },
    },
    benchmark: {
      type: 'object', additionalProperties: false,
      required: ['description', 'numbers'],
      properties: { description: { type: 'string' }, numbers: { type: 'string', description: 'ops/s, ns/op, producer-count sweep, allocation count' } },
    },
    sanitizers: { type: 'string', description: 'TSan / ASan / UBSan results' },
    artifactPath: { type: 'string', description: 'scratch dir where experiments can be re-run' },
  },
}
const DECISION_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['winner', 'rationale', 'decisionRecordPath', 'specRecommendations', 'residualRisks', 'provenClaims', 'disprovenClaims'],
  properties: {
    winner: { type: 'string' },
    rationale: { type: 'string' },
    decisionRecordPath: { type: 'string' },
    specRecommendations: {
      type: 'array', items: {
        type: 'object', additionalProperties: false,
        required: ['spec', 'change'],
        properties: { spec: { type: 'string' }, change: { type: 'string' } },
      },
    },
    residualRisks: { type: 'array', items: { type: 'string' } },
    provenClaims: { type: 'integer' },
    disprovenClaims: { type: 'integer' },
  },
}

// ---------------------------------------------------------------------------
// Prompt builders
// ---------------------------------------------------------------------------
const brief = [
  `# Target: ${TARGET.title}`,
  ``,
  TARGET.question,
  ``,
  `Read these specs in the repo root before you start: ${specList}.`,
  `Invariants this design MUST uphold:`,
  invList,
].join('\n')

const architectPrompt = (angle, i) => [
  ARCHITECT_ROLE, ``, brief, ``,
  `You are architect #${i + 1}. Your assigned design angle — push THIS to its strongest concrete form (do not converge on the others):`,
  ``, angle, ``,
  `Produce a single concrete design with falsifiable fast/safe/correct claims, each with a howToFalsify experiment. Be concrete about atomics and memory orders.`,
].join('\n')

const redteamPrompt = (d) => [
  REDTEAM_ROLE, ``, brief, ``,
  `Red-team this design. Steelman it, then attack its claims hardest where it is proud. Rate each attack fatal/serious/minor and give an executableCheck for each.`,
  ``, '## Design under review', ``,
  JSON.stringify(d, null, 2),
].join('\n')

const rebutPrompt = (d, attack) => [
  REBUT_ROLE, ``, brief, ``,
  `You are the author of the design below. The red team attacked it. Concede what is right, rebut what is wrong (with revised design detail where needed), and output the claims that SURVIVE — each with the exact C++ experiment that will prove it.`,
  ``, '## Your design', ``, JSON.stringify(d, null, 2),
  ``, '## Red-team attack', ``, JSON.stringify(attack, null, 2),
].join('\n')

const proverPrompt = (d, rebut) => [
  PROVER_ROLE, ``, brief, ``,
  `Implement this design in real C++23, build it under g++ AND clang++, run it under ASan/UBSan/TSan, and benchmark it. Mark each SURVIVING claim CORRECT / WRONG / INCONCLUSIVE with the command + observed output as evidence. Report benchmark numbers even for passing claims (the judge compares designs on them). Work in a mktemp scratch dir; do not write into the repo.`,
  ``, '## Design to implement', ``, JSON.stringify(d, null, 2),
  ``, '## Surviving claims to prove (with the author-proposed experiments)', ``, JSON.stringify(rebut, null, 2),
].join('\n')

const judgePrompt = (designs, examined, proofs) => [
  JUDGE_ROLE, ``, brief, ``,
  `Close the debate. Weigh each design by claims that BOTH survived red-teaming AND were proven CORRECT by executed C++. Safety WRONG = disqualified from winning (unless a stated cheap fix exists). Then rank surviving designs by measured hot-path numbers. Pick a winner, write an ADR to decisions/ADR-<nnn>-<slug>.md (create decisions/ and pick the next number), and give concrete spec-update recommendations for ${specList}.`,
  ``, '## Designs', ``, JSON.stringify(designs, null, 2),
  ``, '## Cross-examination (attacks + rebuttals with surviving claims)', ``, JSON.stringify(examined, null, 2),
  ``, '## Executed evidence (per design)', ``, JSON.stringify(proofs, null, 2),
].join('\n')

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
phase('Design')
log(`Target: ${TARGET.title} — ${N} competing designs`)
const designs = (await parallel(
  Array.from({ length: N }, (_, i) => () =>
    agent(architectPrompt(ANGLES[i], i), {
      label: `design:${i + 1}`, phase: 'Design', agentType: 'general-purpose', schema: DESIGN_SCHEMA,
    })
  )
)).filter(Boolean)

if (!designs.length) return { error: 'No designs produced.' }
log(`${designs.length} designs proposed; cross-examining and proving each`)

// Pipeline per design: attack -> rebut -> prove. No barrier — design 1 can be in
// the Prove phase while design 3 is still being attacked.
phase('Cross-examine')
const proofs = await pipeline(
  designs,
  (d, _orig, i) => agent(redteamPrompt(d), {
    label: `attack:${i + 1}`, phase: 'Cross-examine', agentType: 'general-purpose', schema: ATTACK_SCHEMA,
  }).then(attack => ({ attack })),
  (r, d, i) => agent(rebutPrompt(d, r.attack), {
    label: `rebut:${i + 1}`, phase: 'Cross-examine', agentType: 'general-purpose', schema: REBUT_SCHEMA,
  }).then(rebut => ({ attack: r.attack, rebut })),
  (r, d, i) => agent(proverPrompt(d, r.rebut), {
    label: `prove:${i + 1}`, phase: 'Prove', agentType: 'general-purpose', schema: PROOF_SCHEMA, effort: 'high',
  }).then(proof => ({ index: i, attack: r.attack, rebut: r.rebut, proof })),
)

const done = proofs.filter(Boolean)
const examined = done.map(x => ({ design: designs[x.index].name, attack: x.attack, rebut: x.rebut }))
const evidence = done.map(x => x.proof)
log(`${evidence.length} designs proven; judging`)

phase('Judge')
const verdict = await agent(judgePrompt(designs, examined, evidence), {
  label: 'judge', phase: 'Judge', agentType: 'general-purpose', schema: DECISION_SCHEMA, effort: 'high',
})

return {
  target: TARGET.title,
  designsProposed: designs.length,
  designsProven: evidence.length,
  winner: verdict && verdict.winner,
  decisionRecord: verdict && verdict.decisionRecordPath,
  verdict,
}
