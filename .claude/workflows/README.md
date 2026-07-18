# Quark design-debate-prove harness

A group of agents that **design competing solutions, defend them against each
other, then settle the argument by writing and running real C++** — so a spec is
promoted on measured evidence, not on the most confident paragraph. Built for the
goal of a *fast, safe* actor engine.

## The cast (`.claude/agents/`)

| Role | Agent | Does |
|---|---|---|
| **Architect** | `quark-architect` | Proposes one concrete design for a target, stated as **falsifiable** fast/safe/correct claims (each with an experiment that would disprove it). Also plays defense: rebuts red-team attacks on its own design. |
| **Red team** | `quark-redteam` | Steelmans a design, then attacks its claims hardest where they're proud — races, UB, ABA, lost/reordered messages, hidden allocations, false zero-cost. Every attack ships an executable check. |
| **Prover** | `quark-prover` | The empirical judge. Implements each design in C++23, builds under **g++ + clang++**, runs **ASan/UBSan/TSan** + benchmarks, and marks every claim `CORRECT` / `WRONG` / `INCONCLUSIVE` with the command + observed output as proof. |
| **Judge** | `quark-judge` | Weighs designs by claims that **both survived attack and were proven**. Safety-WRONG is disqualifying; then ranks on measured numbers. Picks a winner and writes an ADR under `decisions/`. |

## The flow (`design-debate-prove.js`)

```
Design ─────────► Cross-examine ──────► Prove ──────────► Judge
N architects      red-team attack       implement C++     winner +
propose, each     → author rebut        build g++/clang   ADR + spec
with falsifiable  → surviving claims    TSan/ASan/UBSan    recommendations
claims                                  + benchmark
                                        → CORRECT/WRONG
```

Each design runs the attack→rebut→prove chain **independently** (a pipeline, no
barrier), so design 1 can be compiling while design 3 is still being attacked.
The judge is the one barrier at the end.

## Running it

This is a **saved workflow** — it does not run on its own. Trigger it by asking
Claude in this repo to *"run the design-debate-prove workflow"*, or Claude can
invoke it directly:

```
Workflow({ name: 'design-debate-prove' })                    # default: Mailbox MPSC hot path
```

### Targeting a different subsystem / claim

Pass `args` to point the debate anywhere:

```js
Workflow({
  name: 'design-debate-prove',
  args: {
    title: 'Tagless wire fast path (016)',
    question: 'Prove the negotiated tagless serialization path is zero-cost vs canonical TLV: ...',
    specs: ['016-Serialization.md', '010-Distribution.md'],
    invariants: [
      'Byte-identical decode of every value the TLV path accepts.',
      'No heap allocation on the encode/decode hot path.',
    ],
    angles: [                 // one competing design per angle (optional; defaults provided)
      'Struct-of-arrays direct memcpy with a compile-time field plan ...',
      'Codegen a flat writer/reader from QUARK_SERIALIZE with bounds folded out ...',
    ],
    designs: 2,               // how many competing designs (<= angles.length)
  },
})
```

| arg | meaning | default |
|---|---|---|
| `title` | short name of the debate | `Mailbox MPSC hot path` |
| `question` | the full design problem + what "fast/safe/correct" means here | mailbox brief |
| `specs` | spec files (repo root) the architects must read | `003, 002, 001, 015` |
| `invariants` | hard constraints every design must uphold | single-executor, FIFO, alloc-free, sanitizer-clean |
| `angles` | distinct design directions, one architect each | 3 mailbox angles |
| `designs` | number of competing designs (`<= angles.length`) | 3 |

## Output

The workflow returns the winner, the count of proven vs disproven claims, and the
path to the **ADR** the judge writes under `decisions/ADR-<nnn>-<slug>.md`. The
prover's scratch build dirs (`/tmp/quark-prove.*`) are left in place so you can
re-run any experiment yourself.

## Iterating on the harness

- Tune a role's behaviour → edit the matching `.claude/agents/quark-*.md`.
- Change the flow (more designs, extra verify round, add a completeness critic)
  → edit `design-debate-prove.js` and re-run.
- Watch a run live with `/workflows`.
