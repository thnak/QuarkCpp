# Open Questions

All twenty-six subsystem specs are now drafted (see [README.md](README.md)). What
remains are **cross-cutting design questions** that touch more than one spec and
should be resolved before any spec is promoted from *Draft* to *Accepted*.

## Resolved

- **Outbound streaming replies — an `ask` that returns a stream** (006 + 024 + 017 + 018 +
  010 + ADR-007 reply seam) — the **one remaining pure-design cross-cutting item** — resolved
  in [ADR-018](decisions/ADR-018-outbound-streaming-replies.md) by a 3-design debate-prove
  (28 CORRECT / 11 disproven, real C++23 + GCC/Clang + ASan/UBSan/TSan). The winner is
  **Reply-Credit-Ring (PUSH)**: an outbound streaming reply is the **024 inbound credit-ring
  run backward** — callee = `head` producer, caller = `disp`/`tail` consumer, credit derived
  `capacity-(head-tail)` with **no shared counter**. Three seams around the (verbatim) ring: a
  **single-resolve `StreamReplyCell`** OPEN handshake (rides the ADR-007 `reply_` field; the
  ordinary single-shot `ReplyCell` is **untouched**), the **N-item ring**, and an **in-band
  EoS**; per-item identity is a **callee-assigned `producer_seq`** deduped by the caller's
  `disp` watermark (017), cross-node credit-return is **monotone max-merge** (010), and
  cancel/deadline do a **two-part terminal wake** (002/018). The **N-discrete-reply baseline was
  measured and disqualified** (fails GATE-3/4/5 + cross-node GATE-2/6: unbounded/shed, ReplyCell
  reuse UAF, post-teardown delivery). The pull alternative (`DemandChannel`) **hit 31–33 M/s but
  conceded the 023 0-RMW hard gate** (0.00391 RMW/item vs PUSH's measured 0), so it is adopted
  only as a secondary **`ReplyMode::Pull`** for high-RTT links. The **item-transport leg is proven**
  (it is the shipped 024 `StreamChannel`/`StreamActivation` flipped: 8.3× amortization, 0 per-item
  heap, 0 caller-drain RMW, 5.3–6.4 M/s ≥ the 4M floor). **006 outbound stays Draft** — promotion
  is gated on the **015 OPEN-cell re-admit** (`co_await` on-lane resume, still stubbed in
  `detail/reply_cell.hpp`) clearing an ADR-014-grade real-scheduler run; the item leg does not
  wait on it. Folded into 006/024/017/018/010/004/003/002/001/022/023. **New residual:** the
  fan-in deriving-reply hazard (017); AArch64 weak-memory re-gate of the flipped ring (inherits
  024).

- **Actor execution vehicle — passive+stackless vs green threads/fibers** (001/002/015/024/023)
  — resolved in [ADR-015](decisions/ADR-015-actor-execution-vehicle-passive-stackless-vs-fibers.md)
  by a 4-vehicle debate-prove (32 CORRECT / 1 DISPROVEN, real C++ + sanitizers). The
  **core vehicle stays passive + stackless run-to-completion** — the only vehicle passing
  the safety gate *and* winning the idle-economy + throughput axis (192 B/idle-actor,
  5.59 M/GB identical at 10⁶↔10⁷; suspended frame beats a fiber at every depth ≤ 8; sync
  p99 96.4 ns, 41.8 M/s drain, 0 ctx-switch, 0 hot-path alloc). The user's *"pool green
  threads across idle actors to cut cost"* hypothesis was **measured and disproven** — a
  fiber pins a stack + VMA the moment it runs, and a fiber blocked in an opaque syscall pins
  its carrier OS thread exactly like a pooled thread. But a fiber facility **earned a scoped
  place**: an opt-in, off-hot-path, per-blocking-invocation (never per-actor) `BlockingHandler`
  — thread-backed `quark::blocking<>` (asm-free default) + gated stackful `quark::fiber<>`
  (BorrowedFiber) — for the one axis stackless physically cannot serve (suspending an
  un-colorable foreign-C chain in place; + C4 cooperative multiplexing). Folded into
  001/002/015/024/023. **New residuals** (below): the fiber park/resume handshake needs the
  ARM64 weak-memory re-gate; the fiber path needs a mandatory `__tsan_*_fiber` annotation
  gate-0 before any fiber proof is admissible; the thread-backed adapter latency is a loaded
  µs-scale distribution (p50 ~62 µs), not the ~3 µs originally claimed.

- **Reentrancy / quiescence** (was the top cross-cutting hazard, touching
  001/002/003/007/012) — resolved in
  [015-Reentrancy-and-Quiescence.md](015-Reentrancy-and-Quiescence.md): reentrant
  handlers interleave only at `co_await` (no state data races); a shared
  **admission-control** gate unifies drain budget (`Paused`) with **quiescence**
  (`Draining`/`Cancelling`); `Restart` uses `quiesce(Cancel)`, snapshots use
  `quiesce(Drain)`; sequential actors pay nothing.

- **Serialization, one story for three consumers** (wire 010, snapshot + event-log
  012) — resolved in [016-Serialization.md](016-Serialization.md): one
  reflection-free `describe`/`QUARK_SERIALIZE` per type drives a **canonical
  tagged encoding** (durable, evolvable via stable field tags + `schema_version` +
  migration chain); wire gets a **transparent tagless fast path** negotiated by
  schema fingerprint. "One story" = one description, not one byte layout.
  **Accepted (x86-64)** — the encode-budget + negotiation/evolution gate is met
  ([ADR-016](decisions/ADR-016-serialization-wire-fast-path-encode-gate.md)): tagless
  encode p99 **25–28 ns** (~20× under the 500 ns Hard ceiling, near-memcpy, 0 alloc,
  reflection-free) across 4 build cells; round-trip + additive evolution + migration pass;
  all three controls fired (fingerprint- and endian-mismatch each **corrupt** under
  forced-tagless, proving the negotiation fallback is load-bearing; tagged 1.67–1.83× slower).
  Deferred: the ≤200 ns *goal*-stamp to 023 reference silicon and the ARM64/big-endian
  cross-peer re-gate.

- **Effectively-once effects** (wire 010 + fenced persistence 012 + identity 016)
  — resolved in [017-Delivery-Guarantees.md](017-Delivery-Guarantees.md): three
  `Delivery` levels; effectively-once = deterministic message identity + per-sender
  dedup watermark + **transactional outbox** committed atomically with state under
  a fencing token, output delivered post-commit. Includes the partition worked
  example — a double-activated actor's zombie commit is fenced out, so it produces
  no state and no output. Price: effectively-once actors are **CP** (need a
  linearizable store).

- **Cross-node time** (004 context + 006 ask + 010 transport + 011 deadlines) —
  resolved in [018-Clocks-and-Deadlines.md](018-Clocks-and-Deadlines.md):
  deadlines are **local monotonic instants**; across a node boundary the transport
  ships **remaining duration** and the receiver reconstructs against its own
  monotonic clock, subtracting a conservative transit estimate **reused from SWIM's
  RTT**. Deadlines inherit down the causal chain (monotonically non-increasing) and
  bias lenient, never falsely strict; the PAL pins a suspend-counting canonical
  monotonic clock.

- **Platform Abstraction Layer** (the OS seam under 002/010/012/018) — resolved in
  [019-Platform-Abstraction-Layer.md](019-Platform-Abstraction-Layer.md): one
  compile-time-selected surface (event loop, sockets, affinity/NUMA, durable flush,
  clocks) with the rule that *only* the PAL touches OS APIs; a **completion
  (proactor)** I/O model unifies io_uring/IOCP with epoll/kqueue emulation; and a
  **simulation backend** is what makes 014's deterministic testing real.

- **Security** (distribution 010 + context 004/006 + secrets 013 + persistence 012
  + PAL 019 + observability 009) — resolved in [020-Security.md](020-Security.md):
  security is a **boundary** concern (single-process pays nothing) across five
  seam-based layers — node identity gating SWIM admission + HRW placement, a
  mutually-authenticated encrypted transport, boundary authorization with
  **principal propagation** that attenuates down the causal chain (the security
  analogue of 018's deadline inheritance), a zeroizing `SecretSource`, and optional
  at-rest envelope encryption. The one honest exception to the std-only-default
  principle: **crypto is never self-implemented** — the default transport is
  plaintext (dev-only, loud in `Strict`) and production is a thin adapter over a
  vetted library (mbedTLS/BoringSSL). CSPRNG is a PAL primitive.

- **Cluster formation & lifecycle** (distribution 010 + security 020 + persistence
  012 + serialization 016 + config 013) — resolved in
  [021-Cluster-Formation-and-Lifecycle.md](021-Cluster-Formation-and-Lifecycle.md):
  the **operational lifecycle** 010/020 both assume but neither specced. The **source
  of trust** is explicit config feeding 020's `NodeAuthority` (dev default: shared
  cluster secret, loud in `Strict`; production: cluster CA or SPIFFE), plus a
  **cluster id + membership epoch** to block accidental cross-cluster merges.
  **Connections** realize 010's one-per-peer via a `Discovery` seam (default static
  seeds, needed only at join), **lazy** data-plane establishment, deterministic
  **dial dedup** (lower `NodeId` wins), and SWIM-ping-reused keepalive. **Scaling**
  is a staged join FSM (placement recomputes only once a node can host work),
  **fenced hand-off** (012/017) on planned moves, a **stabilization window** to damp
  churn/flap, graceful drain (015) vs. crash, and rolling upgrade riding 016's wire
  fallback. Non-goal: it makes join/leave safe & cheap; *when* to scale is an
  external control loop over 009.

- **Resource governance & overload control** (backpressure 006 + deadlines 018 +
  concurrency 015 + scheduler 002 + security boundary 020 + distribution 010/021) —
  resolved in
  [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md):
  the **availability** sibling of 020. Founded on one invariant — **bound every
  exhaustible resource** (006's mailbox generalized to memory, workers, connections,
  in-flight sets) — so overload becomes "a bound is hit," not collapse. Mechanisms:
  a `RateLimiter` seam (default **token bucket**) at the ingress boundary next to
  authz; **shed-don't-buffer** load shedding that drops **doomed (deadline-expired,
  018) work first and cheapest**; **circuit breaking** that composes with SWIM to
  cap downstream `ask` pileup; per-shard fair sharing. The honest boundary (parallel
  to 020's crypto honesty): the engine governs **application-layer** resources on
  traffic that arrived; **volumetric/L3-L4 DoS is the kernel/LB's job**, not the
  engine's. Static limits default; adaptive is opt-in.

- **Performance targets & the benchmark harness** (hot paths 001/002/003 + send 006
  + observability 009 + testing counterpart 014 + PAL 019 + governance 022) —
  resolved in
  [023-Performance-Targets-and-Budgets.md](023-Performance-Targets-and-Budgets.md):
  the RFC's qualitative claims (*zero-cost*, *O(1)*, *contention-free*) are pinned to
  **numbers on a fixed reference machine** (x86-64 primary — Zen4/SPR class; ARM64 as
  an allowed ratio) with a locked priority rule (**balanced, throughput breaks
  ties**, bounded by hard latency/footprint ceilings so batching can't buy
  throughput). Hot-path budgets (local `tell` ≤ 100 ns, `ask` ≤ 1 µs p50, descriptor
  ≤ 64 B, **0 hot-path allocations**, **0 cross-core RMW on the drain path**,
  ≥ 10 M msg/s/core) become **Hard vetoes or Goal regressions** graded by the
  `quark-prover` loop; a spec can't reach *Accepted* with an unproven Hard budget.
  The **benchmark harness** (micro + macro, percentile-not-mean, PAL-clock timed,
  CI-gated with a noise band) runs on the *native* PAL backend — the perf counterpart
  to 014's *sim*-backend correctness. Numbers are provisional until real silicon
  re-baselines them.

- **Inbound streaming & large-message cost** (messaging 006 + resources 004 +
  memory 003 + scheduler 002 + governance 022 + perf 023) — resolved in
  [024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md) and
  proven by [ADR-005](decisions/ADR-005-inbound-stream-ingestion-hot-path.md): a
  high-rate stream is a **Resource, not N messages**. `StreamChannel` is a
  pre-allocated per-stream **SPSC credit-ring** off the mailbox, reached by **one
  reusable descriptor per arm-edge** (not one per frame), scheduled through the
  settled exec-state wakeup + `seq_cst` close-out **verbatim**. **Credit is derived**
  from single-writer cursors (no shared RMW → race-free by construction); a **split
  `disp`/`tail` cursor** gives exactly-once dispatch across async suspension;
  backpressure is the **credit window (022) — a producer stall, never mid-stream
  shedding** (the per-item-`tell` baseline was proven to shed 3.9–4.8M frames).
  8/8 claims proven (30–57 M frames/s, 0 alloc, 0 drain-RMW, both compilers).
  **Accepted (x86-64)** — the promotion gate is met:
  [ADR-014](decisions/ADR-014-streaming-async-suspend-real-scheduler-gate.md) wired the
  async-suspend/resume seam to the **real 002 multi-threaded scheduler + 015 admission
  gate** and proved exactly-once at 10⁷ frames on both compilers (`lost = dup = torn =
  fifo_violations = 0`, no descriptor double-enqueue, credit only for completed frames,
  0-alloc + 0-RMW, TSan clean, transfer path genuinely taken), with all three mandatory
  controls firing. Deferred: the Hard absolute-latency budget (023 silicon) and the ARM64
  weak-memory re-gate of the `seq_cst` Dekker close-out. *Outbound* streaming replies (an
  `ask` returning a stream) remain open in 006.

- **Placement optimization & stateless workers** (placement 010 + policies 005 +
  execution invariant 001 + membership 010/021 + config 013 + governance 022) —
  resolved in
  [025-Placement-Policies-and-Stateless-Workers.md](025-Placement-Policies-and-Stateless-Workers.md):
  two gaps on one axis — **determinism**. Nodes advertise **static capabilities**
  (labels/flags/`weight`) gossiped with SWIM membership; placement generalizes to a
  **strategy + modifiers** (`Require`/`Prefer`/`Weighted`/`LocalFirst`/`Affinity`),
  all pure functions of the gossiped set so they stay deterministic and
  coordinator-free (the developer's optimization levers). **Load-aware** selection is
  confined to **`Stateless<N>`** — a declared pool of N local activations, load-routed,
  **no identity / no per-key FIFO / non-durable**, the one explicit relaxation of the
  single-activation invariant (each pool activation is still single-executor).
  Stateful actors never move for load, only on membership change. **RESOLVED — Accepted
  (x86-64)**, both blocking gates now `CORRECT`. The **`Stateless<N>` pool relaxation is
  PROVEN** ([ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md) Gate C
  — exactly-once under concurrency, TSan/ASan clean, shared-state control races; fan-out
  1.5–2.8× > N hand-rolled). The **Weighted-HRW distribution gate** — after ADR-011 caught
  ADR-006's non-proportional `weight·H` formula (fixed to log-WRH `w/(−ln H)`) and a uniform-only
  `CoV ≤ 0.2` threshold (restated on load-per-weight ρ), and ADR-012 exposed a mis-specified
  band — was re-gated **CORRECT** under a like-for-like preregistered p99-vs-p99 band against an
  independent multinomial ([ADR-013](decisions/ADR-013-weighted-hrw-distribution-regate-2.md):
  `R(N) ∈ [0.8765, 1.1051]`, at the multinomial floor, churn exact, both controls fired). The
  scheme sits at the information-theoretic floor; 025 is Accepted (x86-64).

- **Large-scale cluster topology** (distribution 010 + cluster 021 + placement 025 +
  security 020 + observability 009 + config 013 + perf 023) — resolved in
  [026-Large-Scale-Cluster-Topology.md](026-Large-Scale-Cluster-Topology.md) and proven
  by [ADR-006](decisions/ADR-006-large-scale-cluster-topology.md): scaling to 10³–10⁴
  nodes without a coordinator. Three **configurable** axes (topology / connections /
  placement-cache); the small default (`Flat/FullMesh/Direct`) instantiates none of it
  (zero-cost-when-unused). The N ≥ 512 default is **`VirtualBins`** — O(1)
  N-independent placement (5–6 ns, content-addressed determinism), **bounded
  partial-view** (O(log N) sockets), and **DHT-relay** (≤⌈log₂N⌉ hops). Beyond ~10⁴
  nodes the roster O(N) wall is crossed by the opt-in `Partitioned` tier (per-group
  determinism). **Accepted (x86-64)** — the cross-node **FIFO-under-variable-hop-relay** Hard
  gate is **proven** ([ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md):
  0 inversions / 100 × 10⁶ arrivals, unpinned control inverts 88–96%). The Weighted-HRW
  companion issue (non-proportional formula + uniform-only balance threshold) is **resolved**
  by 025's re-gate ([ADR-013](decisions/ADR-013-weighted-hrw-distribution-regate-2.md) CORRECT —
  proportional log-WRH at the multinomial floor); it never gated 026's FIFO/topology promotion.

- **Developer-facing authoring & handler-dispatch API** (developer model 005 +
  messaging 006 + execution 001 + resources 004 + metadata 008 + perf 023) — resolved
  as **one coherent API** in [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)
  (D1 JumpTable-Dispatch, proven: 27 claims CORRECT, the one competing "zero-indirect"
  headline DISPROVEN by objdump on both toolchains). Every choice is a compile-time
  policy: **dispatch** = dense per-actor `.rodata` jump-table keyed by a `consteval`
  `slot_of<A,M>` over `using protocol = Protocol<…>` (one indexed indirect call,
  ≈260 B/actor, beats the 008 scan, ties a hand-switch on uniform+skew, no RTTI/vtable);
  **resources** = member-field declarations (`Cached<Logger> log_;`); **`ask`** =
  async-only (`task<expected<R,error>>`, no `ask_sync`; off-lane uses `block_on`);
  **refs** = always-typed `ActorRef<A>` (routers = typed groups, dead-letter = a
  non-dispatching sink); **reply ordering** = Sequential→request-order,
  Reentrant→completion-order via a shard-pooled monotonic-gen `ReplyCell`. Measured:
  sync tell p99 62 ns, ask round-trip p99 130 ns, 0 hot-path alloc, 0 drain RMW — all
  now 023 regression gates. Folded into 005/006/001/004/008/023. **Residual (not
  blocking x86-64):** protocol-list drift needs the `QUARK_PROTOCOL` macro + lint; the
  reply-lifetime UAF class is a permanent ASan+TSan CI gate; AArch64 inherits the
  mailbox's open weak-memory litmus.

- **Configuration + activation-lifecycle policy** (config 013 + metadata/startup 008 +
  developer model 005 + timers 011 + perf 023) — resolved as **one configurable policy
  surface** in [ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md)
  (D3 Frozen-Core + Hot-Leaf, proven: 8 CORRECT / 0 disproven, one INCONCLUSIVE that is a
  self-imposed cold-path target, not a gate). Every knob declares an **override scope**
  (defaults < engine < node < actor-type < instance, last-highest-setter-wins, resolved
  **once** at build/reconfig) and a **reconfig class**: **BuildOnly** (worker/shard count,
  NUMA, topology, execution mode, type set) fails fast on a live change; **Live** (drain
  budget, mailbox bound + overflow, idle timeout) packs into **one 8-byte atomic word per
  `(shard × type_index)`** — the hot read is a single `mov + mask` (0 RMW, no tear, proven
  0 torn / 4.6B reads), the live publish a single relaxed store (67–73 ns, 0 alloc, can't
  stall the drain). **Guarded `add_actor_type<T>()`** after `build()` (incremental
  Validation + release table swap, pre-sized to a `max_types` cap, QSBR-reclaimed) resolves
  the 008 hot-reload question. Idle deactivation (`KeepAlive`/`IdleTimeout`) rides the 011
  per-shard wheel on the actor's own lane (single-executor preserved; `[Deactivate,M]` race
  aborts eviction — no message loss). Folded into 013/008/005/011/023. **Residual (not
  blocking x86-64):** publish→visible re-measure on the reference core; ARM64 no-tear arm
  unverified; a control-plane publish governor against reconfig-storm cache-line ping-pong.

- **Failure, supervision & recovery policy** (failure 007 + resources 004 + quiescence 015
  + persistence 012 + perf 023) — resolved as **one configurable policy surface** in
  [ADR-009](decisions/ADR-009-failure-supervision-and-recovery-policy-model.md) (D1
  Minimal/Assert-Intact wins 13/10/5 proven, 0 disproven, extended with D3's proven-safe
  knobs). The handler-boundary guard is **zero-cost on the success path** (p99 54.4 ns
  guarded vs 53.5 ns no-guard, 0 alloc, objdump no added work). Resolutions: `Resume` =
  **assert-intact** default + opt-in Sequential-only `Transactional<Off|Snapshot|Journal>`;
  escalation = configurable `Supervision<Node|PerType|Tree>` bounded by static depth +
  `escalation_ttl` + per-supervisor `MaxRestarts` + 022 per-shard token bucket; ask-on-restart
  = `OnRestartAsk<Fail|Retry<N,IdempotencyKey>>` (default Fail); `Restart` reloads persisted
  state iff `Persistent<…>` (reload returns `std::expected`, failure escalates); `PerMessage`
  factory failure **fails the message** (checked before the handler body; `Degrade` is an
  explicit opt-in). Plus proven containment: **deadline/cancellation carved out** of the
  restart decision (no transient-overload storm), **EventSourced staging fence** (throwing
  handler commits nothing), single exactly-once descriptor-reclamation join point. Folded into
  007/004/015/012/023. **Residual (not blocking x86-64):** cold-path unwinder cost on
  high-throw workloads; large-state `Transactional<>` has no COW yet (Reentrant can't use it —
  gated on the 015 COW open q); retry idempotency is asserted not verified; ARM64 litmus; the
  three adopted D3 knobs re-run through the gates against the D1 guard before those paragraphs
  promote Draft→Accepted.

- **Priority & fairness scheduling** (scheduler 002 + developer model 005 + timers 011 +
  perf 023) — resolved in [ADR-010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md)
  (D1 K-band per-shard run-queue, proven 7/0). `Priority<P>` is an engine-level compile-time
  scheduling policy: the shard's activation run-queue generalizes to
  `std::array<ActivationMpsc, K>` under `PriorityBands<K, Anti>`; **`UniformFIFO` (K=1) is
  the default and objdumps byte-identical** to today's single MPSC (zero-cost when uniform).
  Enqueue = compile-time band subscript on the same `tail_.exchange` (**0 added cross-core
  RMW**); O(K≤8) relaxed top-band probe. **Per-actor mailbox FIFO is inviolable** (priority
  orders activations across actors, never an actor's own messages — proven 0 inversions);
  high-band dispatch p99 ~316× lower than FIFO. Anti-starvation is itself a knob:
  `RotatingReserve<M>` (default, guaranteed bound `(d+1)·K·M` select turns) or `WeightedDRR<w…>`
  (proportional share). A deadline-unified `EdfBanded` policy was **evaluated and deferred**
  (can degrade below FIFO under overload — the D3 EDF-domino, disproven). Folded into
  002/005/011/023. **Residual:** ask-chain priority inversion is unmitigated (static
  per-type priority, no donation — `band_of()` is the future extension point); K is a
  build-time constant; HW RMW counters need re-confirming with `perf c2c` on a CI box.

## Highest-leverage unresolved questions

These affect multiple subsystems; resolving one constrains several specs.

1. **Type-name → `type_key` stability across toolchains** (008). Now **mandatory**,
   not merely desirable: the cross-platform target means a mixed Linux/Windows/
   ARM/x86 cluster (010) and shared durable logs (012, 016) must derive identical
   `type_key`s from GCC/Clang/MSVC. Needs a per-toolchain conformance test, with a
   developer-assigned explicit `type_key` as the fallback if it proves unreliable.
   *(This is a conformance test to write, not a design question.)*

   **Partially answered — the test now exists and the answer is "no, not yet".**
   [`tests/metadata_typekey_conformance_test.cpp`](tests/metadata_typekey_conformance_test.cpp)
   pins golden key constants in three tiers (the pre-existing `metadata_typekey_test.cpp`
   asserts only self-consistency and cannot observe drift at all). Measured on
   Linux/x86-64, g++ 14.3 vs clang 20.1:
   - **Described types are portable** — `fingerprint_v<T>` folds `{(tag, wire_type)}`
     with no name, so it is toolchain-independent *by construction*. Byte-identical
     across both compilers. This is the tier durable headers and wire negotiation ride on.
   - **User-defined names agree GCC↔Clang** — also byte-identical, so those goldens are
     asserted for both.
   - **Builtin spellings DIVERGE today**: `size_t` is `long unsigned int` (GCC) vs
     `unsigned long` (Clang); likewise `long long unsigned int`/`unsigned long long` and
     `long long int`/`long long`. Different string → different FNV-1a → **different
     `type_key` for the same type**, and it propagates into any template over them
     (`Wrap<uint64_t>`). Left as recorded-not-asserted (Tier 3); pinning a golden would
     hard-fail one compiler.
   - **MSVC diverges for every name-derived key** (read from source, not yet measured —
     no Windows host here): the `__FUNCSIG__` slice in `metadata.hpp §canonical_type_name`
     retains the `struct`/`class` elaborator, yielding `struct ns::Point` where GCC/Clang
     yield `ns::Point`. Tier 2 is therefore `#if !defined(_MSC_VER)`-guarded. **Confirming
     this on Windows CI is the next step.**

   Closing this means normalizing the spellings (strip the MSVC elaborator, canonicalize
   builtin names) or mandating an explicit per-type key; then Tier 3 promotes to Tier 1.

2. **`type_key` collisions between distinct Described types** (008/016) — **NEW, design,
   not verification.** Because `fingerprint_v<T>` folds only `{(tag, wire_type)}` and
   deliberately excludes the name, the property that makes it toolchain-portable (item 1)
   also makes it **non-unique**: structurally identical types share a key. Measured in one
   TU, g++ 14.3 — `Point{int x;int y}`, `Other{int p;int q}` and `Account{uint64 id;int64
   balance}` all fold to `0x60eb68b9763e6f4c`. 008 promotes this fingerprint to *the durable,
   cross-run/cross-node type identity*, so two same-shape message types silently share a
   durable record header and wire identity. `Account` colliding with `Point` is the pointed
   case: `uint64_t` and `int` both map to the varint wire type, so semantically unrelated
   types with different field widths and names are one identity downstream.
   - **Scope**: registered *actor* types are safe — their keys seed with the canonical name
     (unique) and startup Validation runs a Strict collision scan (`metadata.hpp:506`).
     Described *message* types do not go through that registry, so nothing catches them.
   - **`describe.hpp:236`'s field-order invariant is NOT violated** — checked separately.
     Swapping *tags* (`(2,x),(1,y)` vs `(1,x),(2,y)`) does yield a distinct fingerprint, and
     differing wire types at the same tag likewise. Reordering *declarations* while tags stay
     attached to their fields correctly yields the same fingerprint — that is a compatible
     schema change, not a defect. The fold behaves exactly as 016 specifies; the open question
     is only whether a *schema-shape* fingerprint is the right thing to promote to *type
     identity*.
   - **The tension is structural**: name-free ⇒ portable but colliding; name-based ⇒ unique
     but toolchain-fragile (item 1). Adding the name to the fingerprint trades this defect
     straight back for item 1. A direction worth red-teaming is **splitting the roles**: keep
     the shape fingerprint for wire/schema negotiation, where shape-equality is precisely the
     desired semantic, and give durable type identity a separate normalized-or-explicit key —
     plausibly the same mechanism as item 1's "developer-assigned explicit `type_key`"
     fallback, closing both. That is an ADR (`design-debate-prove`), not a patch.

3. **Weak-memory (AArch64) proof of the mailbox handoff** (001/002/003, ADR-002).
   The intrusive-Vyukov mailbox was chosen with executed evidence, but the host had
   no herd7/GenMC, so **all sanitizer evidence is x86-TSO**. Two edges are unproven
   on a weakly-ordered target: the exec-state release/acquire handoff that carries
   the consumer-private `head_` across workers, and the lost-wakeup/publication
   edges. **Deferred, not blocking, given the current support phase** (Linux/x86-64
   is the target we have hardware for — see README): on x86-TSO these specs are
   *not* held back by this item. The proof is gated on the **Linux baseline
   stabilizing** — at that point **abstract/formal models** (herd7/GenMC litmus, and
   the deterministic sim of 014) are how we extend the handoff's correctness to
   weakly-ordered targets **without needing the ARM hardware first**. Only ARM64
   promotion, not x86-64, waits on it. The winner is not expected to change
   (exchange should *widen* its lead over CAS-retry on ARM); the *margin* and the
   formal backing are what's open. Like item 1, this is verification to run, not a
   design to invent. *(024's `armed.exchange`-as-Dekker-arm is the same class of
   x86-TSO-only evidence and defers with this item.)*

4. **Stream async-suspend resume against the real scheduler** (024/002/015,
   ADR-005) — **RESOLVED** ([ADR-014](decisions/ADR-014-streaming-async-suspend-real-scheduler-gate.md)).
   StreamChannel's split-cursor exactly-once-across-suspension property, previously proven
   only against an in-thread model resolver, is now **wired to the real 002 multi-threaded
   scheduler + 015 admission gate** and proven: the completion continuation re-enters
   `StreamChannel::drain` **without re-enqueuing the descriptor** (max membership 1, transfer
   path genuinely taken), exactly-once at 10⁷ frames on both compilers, 0-alloc + 0-RMW, TSan
   clean, all three mandatory controls firing. **024 is Accepted (x86-64).** Deferred: the Hard
   absolute-latency budget (023 silicon) and the ARM64 weak-memory re-gate of the `seq_cst`
   Dekker close-out (TSO-proven only).

## Per-spec open questions

Each drafted spec ends with its own *Open questions* section; the notable ones:

- **001** — *(Reply ordering for concurrent `ask`s: resolved, ADR-007 — Sequential→request-order, Reentrant→completion-order via pooled `ReplyCell`. Reentrancy/quiescence: resolved, 015.)*
- **002** — *(`Priority<P>` queue structure: resolved — K-band per-shard run-queue, `UniformFIFO` default, ADR-010. Drain-budget accounting: resolved, 015.)*
- **003** — inline-small-payload optimization. *(Reentrant payload reclamation: resolved, 015.)*
- **004** — *(Cold-shard resolution order: resolved — Node/Shard resources are resolved eagerly, for every configured shard, synchronously inside `Engine`'s `build()` cold phase, strictly before any worker thread exists; a Node-scoped resource shared by many shards is dissolved by ordering, not synchronization (no CAS/lock/`call_once`), ADR-021. Resource-declaration ergonomics: resolved as member fields, ADR-007. `PerMessage` factory failure: resolved — fails the message, checked pre-handler, ADR-009.)*
- **005** — *(Resource-declaration ergonomics **and** `handle` dispatch mechanism: both resolved, ADR-007 — member fields + dense jump-table. Only `tell`/`ask` naming remains.)*
- **006** — **outbound** streaming replies (`ask` returning a stream). *(`ask` from sync code: resolved — async-only, no `ask_sync`, ADR-007. Typed vs. dynamic refs: resolved — always-typed, ADR-007. Backpressure/overflow policy: resolved as the foundational lever of governance, 022. **Inbound** streaming: resolved, 024.)*
- **007** — *(State rollback on `Resume`, escalation granularity, `ask` retry across restart, Restart+persistence reload: all resolved, ADR-009 — assert-intact + opt-in `Transactional<>`, `Supervision<Node|PerType|Tree>`, `OnRestartAsk<Fail|Retry>`. Restart quiescence: resolved, 015.)*
- **008** — *(Hot-reload / dynamic registration after `build()`: resolved — guarded `add_actor_type<T>()`, incremental Validation + release table swap, pre-sized to `max_types`, ADR-008.)*
- **009** — whether user counters are declared as a resource/policy (compile-time slot) or
  registered dynamically at startup. *(Histogram bucket layout: resolved — per-metric
  configurable compile-time `HistogramSpec`, closed-form bucket lookup, Validation-capped
  `bucket_count`, ADR-022. Cardinality control: resolved — per-type always-on grid + opt-in
  per-instance `PerInstanceMetrics<N>` over a build-time-sized critical-arena, ADR-022.)*
- **010** — **cross-node backpressure** (the named residual keeping 010 at *Accepted core* rather than full Accepted). *(Cross-node FIFO-under-relay: resolved — proven, ADR-011. Re-placement granularity: resolved, 026. Split-brain effect safety: resolved via fencing, 017; transport per-OS via the PAL; formation/bootstrap/connection-lifecycle/elastic-scale: resolved, 021.)*
- **011** — shard-fairness/starvation under `shard_count > worker_count` (tracked against
  002/ADR-010 — not a wheel-design gap; the timekeeper's targeted wake cannot rescue a worker
  that's busy rather than parked). *(Wheel granularity/tier sizing vs. deadline precision, and
  timekeeper-tick drift: resolved — 5 fixed tiers × 64 slots on a 64µs base tick, a single
  relaxed `next_due_hint_` per shard, ADR-023. Cross-node time: resolved, 018. Idle-timeout
  deactivation mechanism: resolved, ADR-008. On-demand passivation reusing the same
  deactivation/close-out path: resolved, ADR-034.)*
- **012** — exactly-once effects: combining at-least-once delivery (010) with idempotent,
  fenced persistence to get effectively-once semantics. *(The `ask`-retry-across-restart case
  folds into `OnRestartAsk<Retry<N,IdempotencyKey>>` (007, ADR-009); cross-node
  effectively-once is resolved in 017. Log compaction cadence for EventSourced actors, and
  sync-vs-background checkpoints: resolved — wall-clock `CompactEvery<T>` cadence, checkpoint
  write always async/background, idle-deactivation as a required trigger, ADR-024. Reentrant
  commit ordering: resolved, 015. Schema evolution: resolved, 016. Deactivation-time flush of
  `declare_lazy`-registered actors, reusing `save_snapshot` with a fence acquired at
  construction: resolved, ADR-034.)*
- **013** — *(Live reconfiguration **and** override precedence across layers: both resolved, ADR-008 — reconfig class + packed Live word + five-layer scope. Only a publish-path storm governor remains.)*
- **014** — in-process vs. multi-process distribution (010) test tier. *(Interleaving
  exploration strategy: resolved — a compile-time `Chooser` policy for random search plus
  DPOR-pruned bounded model checking (`explore_bounded(N, branch_cap)`), ADR-025.
  Performance-regression testing: resolved — the native-PAL benchmark harness + quantified
  budgets, 023, is the perf counterpart to this spec's sim-backend correctness. The sim
  backend seam is now pinned by the PAL, 019.)*
- **019** — whether `SimEngine` uses build-time backend selection or the templated
  `Engine<Pal>` form for in-process coexistence with a real engine in one test. *(Vectored/
  scatter-gather I/O and registered-buffer generality: resolved — one mandatory Tier-1
  baseline plus a named Tier-2 extension point (`advanced_io()`/`AdvancedIo`), byte-identical
  caller source across backends, ADR-026. Minimum OS versions and the fallback matrix:
  resolved, ADR-026.)*

## Not yet specced at all

- *(Empty — every subsystem the RFC set out to cover is now drafted, including
  inbound streaming (024). The former entries here (Security, DoS/governance,
  benchmark harness) are resolved by 020, 022, and 023 respectively. **Outbound streaming
  replies — the last open *design* question — is now resolved in mechanism by
  [ADR-018](decisions/ADR-018-outbound-streaming-replies.md)** (above). What remains is
  **mostly verification to run**: the `type_key` conformance test (now written — it reports
  a real GCC↔Clang divergence on builtin spellings, item 1), the AArch64 weak-memory proof of
  the mailbox handoff, and the **015 OPEN-cell re-admit real-scheduler gate** — which now
  blocks *both* an ordinary `ask`'s co_await resume and 006 outbound's OPEN handshake
  promotion. **One design question has since reopened**: `type_key` collisions between
  structurally identical Described types (item 2) — the name-free fingerprint that makes
  durable keys toolchain-portable also makes them non-unique, and it contradicts
  `describe.hpp:236`'s stated field-order invariant. That one needs an ADR, not a test.)*

## Decisions locked (do not reopen without cause)

- C++23, hybrid handler model, CRTP policy types — see [README.md](README.md).
- **Cross-platform**: Linux/Windows/macOS on x86-64 + ARM64; OS specifics behind
  the PAL; no POSIX-only assumptions in subsystem logic.
- Std-only core; heavy dependencies only as optional adapters behind seams.
- No .NET / managed-runtime vocabulary anywhere in the RFC.
