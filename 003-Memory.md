# 003 — Memory

## Goals

- **Zero allocations on the hot path.**
- **Cache locality** — data an actor touches lives in its shard's domain.
- **Shard ownership** — each shard owns its allocator; no cross-shard contention.

## Layout

```
Mailbox → MessageHandle → Descriptor → Payload
```

- **MessageHandle** — a small, trivially-copyable reference stored *by value*
  wherever a queued message is named (cancellation tokens, reply routing). It is
  `{Descriptor*, u48 generation}` — the pointer locates the descriptor, the
  **48-bit** generation fences it against reuse (see *Cancellation*, below). The
  mailbox itself stores no separate handle word: the queue link lives **inside** the
  descriptor (see *Mailbox structure*), so enqueue/dequeue moves one pointer.
- **Descriptor** — fixed-size metadata block (pooled). Its **first member** is the
  intrusive mailbox link; the rest is per-message metadata:
  - `MailNode { std::atomic<Descriptor*> next; }` — **must be the first member**
    (see *Mailbox structure*)
  - `std::atomic<uint64_t> gen_state` — **one packed word** `{generation:48,
    state:4, flags:12}`; the generation is bumped on every `release()` and gates
    stale cancellation. It is a *single* atomic so the generation check and the
    state flip are one CAS (see *Cancellation* — closes a TOCTOU)
  - `MessageId`
  - payload reference (offset/pointer into a payload arena)
  - *(state `Queued`/`Running`/`Completed`/`Cancelled` and flags live in the packed
    `gen_state` word above)*
  - deadline
  - trace id
  - priority metadata
- **Payload** — the message data itself, stored **independently** of the
  descriptor, in an arena / slab / object pool. Payload storage and descriptor
  storage have separate lifetimes.

## Mailbox structure — intrusive Vyukov MPSC

> Pinned by **[ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md)** — the
> only design of three that swept its safety, correctness, and speed claims under
> executed C++ (GCC 14.2 + Clang 20.1, TSan/ASan/UBSan, percentile benchmarks).

The mailbox is an **intrusive Vyukov MPSC queue**: the queue node *is* the pooled
`Descriptor`. Because the link is the descriptor's first member, a
`MailNode*` and a `Descriptor*` are **pointer-interconvertible**, so the queue
threads descriptors directly with no side allocation and no separate handle array.
The invariant is guarded at compile time:

```cpp
static_assert(std::is_standard_layout_v<Descriptor>);
static_assert(offsetof(Descriptor, link) == 0);
static_assert(std::is_standard_layout_v<std::atomic<Descriptor*>>);  // desc_of() cast safety
#ifdef __cpp_lib_is_pointer_interconvertible
static_assert(std::is_pointer_interconvertible_with_class(&Descriptor::link));
#endif
```

> The `#ifdef` is load-bearing, not cosmetic: `std::is_pointer_interconvertible_with_class`
> is **not provided by libstdc++ as shipped with Clang 20.1** (feature-test macro
> `__cpp_lib_is_pointer_interconvertible` undefined) — an unconditional use fails to
> compile there (verified). Deleting the assert to satisfy Clang would turn
> `desc_of()` into an unchecked UB cast, so the `is_standard_layout_v<atomic<Descriptor*>>`
> assert is added as the always-on guard and **CI must build this on both GCC and
> Clang** (ADR-003).

- **Enqueue** (many producers): one unconditional `tail_.exchange(desc, acq_rel)`
  then one link store. Wait-free per producer, **allocation-free**, and **ABA-free
  by construction** — it never compares an address, so no address can be recycled
  underneath a compare. Proven: the enqueue compiles to `1 lock xchg + 2 stores`,
  no retry loop, on both compilers. The exchange is **`acq_rel`, not `release`**:
  the acquire half orders the predecessor's node-initialization *before* the
  successor's link store (publication ordering). On x86-TSO the `xchg` is already a
  full barrier so this is free; the order is spelled `acq_rel` so the code is correct
  on weakly-ordered ISAs too (ADR-003 — release-only permits a lost-newest-node
  execution that x86 sanitizers cannot see; the weak-memory proof itself is deferred
  with the rest of ARM, see OpenQuestions).
- **Dequeue** (single consumer): the draining worker walks a **consumer-private**
  `head_` with plain loads — **zero cross-core atomic RMW on the drain path**
  (023 hard budget). Its cross-worker handoff carries no atomic of its own;
  `head_`'s visibility rides the actor exec-state CAS (001, 002). The transient
  window between a producer's `exchange` and its link store is surfaced as a third
  drain result **`Busy`** (distinct from `Empty`), handled by the scheduler (002).

### Stub sentinel on its own cache line

A Vyukov queue is anchored by a **stub** sentinel node. It must be
`alignas(64)` and kept off the consumer-private `head_`'s cache line — a producer
writes the stub's link on every idle→active re-arm, so a co-located stub
**false-shares** `head_` and taxes the dequeue hot path (ADR-004,
`perf c2c`-confirmed). Enforce the separation:

```cpp
static_assert(offsetof(Mailbox, stub_) - offsetof(Mailbox, head_) >= 64);
```

### Single-membership invariant

A descriptor is in the mailbox **at most once**. A double-`tell` of one descriptor
would corrupt the intrusive chain. This is a global lifecycle invariant guaranteed
by the descriptor allocation lifecycle (a descriptor is pooled-out on enqueue,
returned only after drain) plus a debug-build assertion; it cannot be checked
cheaply on the hot path.

### Backpressure

The queue is **unbounded** — it provides no backpressure of its own. A runaway
producer that outruns the drain exhausts the shard's descriptor pool. Bounding
that is **out of mailbox scope**: admission and overload control are a companion
policy in [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md).

## Allocators

Each shard owns a `std::pmr::memory_resource`:

- **Descriptors** — fixed-size pool (`std::pmr::pool_resource` or a hand-rolled
  free-list). Allocation is a pop; free is a push.
- **Payloads** — arena/slab for short-lived message data; object pools for
  frequently reused fixed-size payloads.

Because the allocator is shard-owned and a shard is drained by one worker at a
time in the common case, allocation is contention-free without locks on the hot
path.

### Free-list synchronization (ADR-037)

`quark::detail::MessagePool` (`include/quark/detail/message_pool.hpp`) implements the
descriptor pool above: each partition is a plain `std::mutex` + intrusive
`free_head`, one partition per producer-thread lane (`num_partitions`, default 1) to
remove producer-vs-producer contention. A `design-debate-prove` debate (ADR-037)
settled the residual cost that partitioning alone does not remove — `acquire()` and
`reclaim()` each still took the partition mutex once, a full lock/unlock round trip
per message. The winning design fronts the existing mutex+`free_head` with a small,
non-atomic, thread-local **magazine** (a bounded `Descriptor*[64]` array) per
`(thread, partition)` pair: `acquire()` pops from the calling thread's magazine and
only touches the mutex when it is empty, refilling up to 32 cells in one critical
section; `reclaim()` pushes into the drain lane's magazine for the cell's home
partition and only touches the mutex once the magazine hits capacity, returning 32
cells in one critical section. Net effect: the mutex is touched roughly 1/32nd as
often, for a measured 1.27x–3.0x round-trip throughput improvement across producer
counts {1, 2, 4} (see ADR-037 for the full evidence table).

**Pool lifetime clause.** A `MessagePool` is safe to destroy at any time — no
resident magazine cell is ever dereferenced after its owning pool dies, guarded by a
generation-safe `std::weak_ptr<PartitionToken>` liveness check on every magazine
touch, never a raw pointer compare. Ordinary `Engine` shutdown (which joins every
worker thread before any pool it fed a `ReclaimSink` to is destroyed) needs no
extra step: a thread's magazines flush automatically, through the normal
mutex-guarded splice, when that thread exits. A caller that keeps a thread running
**past** the lifetime of a specific pool — and wants that pool's steady-state cell
count to conserve exactly rather than forfeit a bounded resident batch — must call
`quark::detail::flush_current_thread_message_caches()` on every thread that touched
the pool before destroying it. Skipping this call is always safe (never a
use-after-free); at worst it leaves up to 64 cells per thread absent from that
pool's free list at the moment of destruction, which is moot once the pool's own
storage is freed with it.

**Capacity-sizing note.** Because up to 64 cells can sit resident in each thread
that touches a partition rather than on its `free_head` at any instant, pre-sizing
a partition to only its exact steady-state working-set count is no longer
sufficient to guarantee zero cold growth under multi-thread traffic — operators
should budget headroom (measured: a 40-cell pre-sized/2-thread config needed
73–103 cells in practice). This does not change the 0-heap-allocation-once-pre-
sized contract; it only raises how much pre-sizing "enough" requires under
concurrent access.

**Thread-local footprint.** Every thread that ever calls `acquire()`/`reclaim()` on
any `MessagePool` in the process pays a fixed ~36 KB `thread_local` cost (a
64-slot, direct-mapped magazine table, shared across every pool that thread
touches, not allocated per-pool). Fine at this repo's stated thread-count scale;
worth revisiting only if the engine grows into a very-high-thread-count deployment
shape.

## Cancellation and memory

Cancellation is a **state transition only** — no descriptor is freed early and no
queue entry is removed. A cancelled descriptor lives as a **tombstone** until the
draining worker reaches it, at which point it is skipped and its descriptor +
payload are returned to their pools. This keeps the mailbox queue append/pop-only.

**Cancellation is generation-gated.** A cancel acts through a `MessageHandle`
(`{Descriptor*, u48 generation}`) and writes the cancelled state **only if**
`handle.generation == descriptor.generation`. `release()` bumps the descriptor's
generation before returning it to the pool, so a late cancel that races reclamation
finds a generation mismatch and is a safe no-op. This is **mandatory, not
optional**: ADR-002 proved by executed contrast that a bare `Descriptor*` cancel
racing reclaim is a **heap-use-after-free** of pooled memory (ASan-confirmed),
whereas the generation-gated form ran 8M cancellations clean.

**The generation is 48 bits, not 32** (ADR-004). A `u32` generation over a small
descriptor pool **wraps in ~24 h at 50 M msg/s** — after wrap a stale handle can
alias a live message's generation and *wrongly cancel it*, a silent lost message
(the executed `u8`-width control fired exactly this). 48 bits pushes the wrap
horizon past any process lifetime. The `{generation:48, state:4, flags:12}` packing
into one `uint64_t` is now fixed: future flag/state growth must **not** steal
generation bits below 48.

The generation check and the state flip must be **one packed atomic CAS**, not two
separate atomics: pack `{generation, state}` into a single word and CAS it, so a
concurrent `release()` (which bumps the generation) cannot slip between the check
and the flip. ADR-003 exposed a **TOCTOU** when they are two independent atomics —
the cancel reads a matching generation, `release()` recycles, then the stale cancel
writes a flag into the reused descriptor.

## Completion — `Descriptor::complete()` (ADR-029, r7 judgment)

The shipped, current spec is the **plain-store** form: `Running -> Completed` is
one relaxed load of `gen_state` followed by one `release`-order store of the
updated word — **not** a CAS, and `complete()` returns `void`. This is the form
proven clean across all seven rounds of the mailbox debate (ADR-001..004,
ADR-020, ADR-027, ADR-029) and is normative until superseded.

**Future work (gated, not scheduled).** A non-void/CAS form of `complete()` —
e.g. one that reports whether the transition raced a concurrent cancel — must
not be introduced on its own. It is gated on `011`'s deadline-driven
force-completer landing first, with the API-shape change (return value) and
every `activation.hpp` call-site update designed and red-teamed **together as
one unit**. ADR-029's round-7 attempt at this (S5, CAS-hardening `complete()`)
was **conceded and withdrawn** during cross-examination because the fix required
threading a return value through 7 call sites in `activation.hpp` that the
proposal never touched — introducing the change piecemeal is the mistake this
note exists to prevent.

**`gen_state` flags subfield — debug-only double-enqueue guard bit.** The 12-bit
`flags` subfield of the packed `gen_state` word (`GenState::flags_bits`, see
*Mailbox structure* above) reserves a **debug-build-only** `kDebugInQueue` guard
bit (r7), set on enqueue and checked/cleared on claim, to catch a double-`tell`
of one descriptor (the *Single-membership invariant* above) with an assertion
instead of a corrupted intrusive chain. **Correct bit-packing is a normative
requirement**: the bit must be **shifted into the flags subfield** (e.g. via
`GenState::pack`/`with_state`-style masked composition), never a raw `OR` into
the whole 64-bit word — a real bit-shift bug of exactly this kind was found and
fixed during ADR-029's proof (S6).

## Close-out ordering — Dekker fence proven necessary; `ExecStateCell`'s own ordering now independently proven load-bearing (ADR-031 r8, closed by ADR-032 r9)

Two distinct ordering claims in the exec-state close-out path were previously
bundled together; ADR-031 separated them and proved only one; ADR-032 closed
the other:

- **The seq_cst Dekker fence in `Mailbox::producer_close_out_fence` /
  `consumer_close_out_fence` is proven load-bearing.** Removing it reproduces
  a real lost-wakeup: 0 lost/50M messages fenced vs. 366k–618k lost/50M
  unfenced (ADR-031 F5, reconfirmed ADR-032: 0/50M fenced vs 441,292–576,097
  lost/50M unfenced). This fence stays in the shipped design and is not a
  candidate for removal or relaxation.
- **`ExecStateCell`'s own release/acquire ordering is now independently
  proven load-bearing (ADR-032 F4-revised), closing the question ADR-031
  left open.** ADR-031's mutant test was inconclusive because it ran the
  full `sched_no_lost_wakeup_test` workload, where `run_queue.hpp`'s own,
  untouched, Vyukov-style MPSC transitively republishes the same data via
  its own `acq_rel` exchange — masking whether `ExecStateCell`'s ordering
  does anything on its own. ADR-032 isolated the claim with a dedicated
  two-thread litmus that has no `run_queue.hpp` hop at all
  (`tests/exec_state_cell_isolated_litmus_test.cpp`): at acquire/release, 0
  TSan reports over 5M trials on both GCC and Clang; downgraded to
  `memory_order_relaxed`, TSan fires a real race every time. The ordering is
  independently necessary, not merely redundant with `run_queue.hpp`'s
  incidental republication — this closes the open question definitively.
- **Methodological caveat (ADR-032, applies to any future ordering-strength
  claim on this mailbox): TSan cannot detect a race between two accesses to
  the *same* atomic object, regardless of memory_order.** ADR-032's own
  standard "downgrade release→relaxed and expect TSan to fire" control did
  not fire on either `link_push`'s next-store or the producer's
  `tail_.exchange` when tested this way. Any future claim that a given
  memory_order is load-bearing on this mailbox must be verified via a
  dedicated *isolated* litmus test (as `exec_state_cell_isolated_litmus_test`
  now is) — never via a downgrade-and-rerun-TSan control on the shipped
  multi-producer test, which cannot distinguish "genuinely redundant" from
  "TSan structurally can't see this class of bug."
- **New permanent regression coverage from ADR-032**: `F3b` (cancel racing
  concurrent release/recycle — 1,000,000 iterations, 0 UAF/double-free, 0
  TSan/ASan reports) and `C2b` (48-bit generation wraps correctly at the
  boundary: seeded at gen_max, `release()` → `generation()==0`, stale
  gen_max handles safely rejected post-wrap).

### Rejected designs (round 9, ADR-032)

- **SBR-v5** (resident sequence-numbered ring + proven-mailbox overflow
  valve) — disqualified: its own stated global-ticket-order guarantee across
  the ring/overflow seam is violated once ≥2 producers concurrently engage
  overflow (up to 858,983 out-of-order deliveries per run). Do not re-attempt
  this specific ring/overflow handoff protocol without first fixing that
  ordering seam.
- **SEG-REX** (segmented Treiber-push / bounded-batch-reversal mailbox) —
  disqualified: its segment-seal/reclamation protocol has a reproducible
  unsigned-underflow-then-deadlock, sanitizer-invisible (a logical/ABA bug,
  not a data race), that persisted after an in-round repair attempt. Do not
  re-attempt this specific announce/revalidate/quiesce reclamation mechanism
  without first replacing it with real hazard-pointer/RCU-style deferred
  reclamation — see 015's residual-risk note on this same design.

### Rejected designs (round 10, ADR-033)

- **SEG-HP** (per-slot-`Descriptor*`/generation-gated segmented Treiber-push,
  hazard-pointer/RCU-paired descendant of SEG-REX) — disqualified: a
  reproducible premature-`Empty` violation of the Busy/Empty/mid-publish
  tri-state contract at an *ordinary* (non-force-sealed) full-segment-
  rotation boundary — the gap between publishing a segment's last slot and
  linking the next segment lets `try_dequeue()` report `Empty` even though
  guaranteed-forthcoming work is already committed (195/200 isolated repro;
  13,353/240,000 in one multi-lane stress run) — plus a ~370-400x rotation-
  burst p999 regression versus the incumbent (91.9-112.8µs vs. 250-286ns).
  Do not re-attempt this specific rotation/reclamation mechanism without
  first fixing both defects (see 015's residual-risk note and ADR-033's
  round-11 scoping experiment). Its array-indexed, no-reversal-walk flat
  p999 oldest-message-discovery result (F3) is real and strictly better than
  SEG-REX's own finding, but is not detachable from the broken mechanics
  around it — bank it, do not reuse the design as shipped.
- **`DeliveryMode<ThroughputFirst<K>>`** (K independent unmodified-Vyukov
  shards, one per producer-thread-hash-lane, merged by a round-robin
  single consumer) — not disqualified on safety (every `safe`/`correct`
  claim proved CORRECT), but its central throughput claim (>=3x at P=K=8)
  fell to 1.2x-1.5x real / 2.1x-2.7x idealized-ceiling, and a second claim
  (idle-shard latency "must not shift materially") was also disproven
  (1.7x-2.8x p50 degradation). Both trace to the same structural cause: the
  single-executor invariant means one consumer thread still merges all K
  shards. Not adopted as the default this round; flagged as a candidate for
  a future opt-in implementation decision, contingent on restating the
  throughput claim honestly and documenting the idle-shard cost. See
  ADR-033.

**Methodological lesson (ADR-033, r10) — a harness must be shown incapable
of a structurally-forced verdict before its result is trusted.** Two
harness-construction traps were caught by the r10 prover in its own
first-draft test code, before they could produce a false verdict: a
single-threaded strict-alternation lost-wakeup harness that could never
exhibit a lost wakeup *by construction*, regardless of whether the design
under test was correct; and a `Busy`-observability harness with two threads
illegally calling `try_dequeue()` on the same shard concurrently — itself a
violation of `Mailbox`'s own single-consumer precondition, caught only
because TSan flagged the harness's *own* race. This sits alongside the
existing TSan-same-atomic-object blind spot (below) as a standing
requirement: any future mailbox-adjacent proof must verify the harness
itself cannot pass or fail independently of the design's actual behavior,
and cannot itself violate the invariant it is meant to check.

**Hash-diffusion caveat (ADR-033, r10) — a single-fold avalanche hash is
measurably weaker for small sequential inputs.** A thread-nonce-keyed
sharding hash of the form `h ^= h >> 32`, evaluated against a
process-wide monotonic counter, showed ~5.76% worse lane spread for the
*first* values the counter produces — exactly the common case of a fixed
worker pool spawned once at startup. This is real but currently
unquantified in downstream impact (throughput/fairness effect not yet
measured at realistic K/pool sizes); flag it as a caveat for any future
thread-id/nonce-based shard- or lane-assignment hash in this codebase, not
yet a disqualifying defect on its own. See ADR-033 (`DeliveryMode` F2b).

## Shared-payload reclamation (broadcast)

A `Topic<M>` publish (ADR-019) allocates **one** immutable, pool-allocated
`SharedPayload<M>` and fans it to N subscribers as N thin descriptors — so the
payload has N potential last-users and must be reclaimed **exactly once** no matter
which subscriber gets there:

```cpp
struct alignas(64) SharedPayload {
    std::atomic<uint32_t> rc;   // own cache line — padded away from M below
    void (*dtor)(void*);        // type-erased dtor thunk (the tell path's mechanism)
    // ---- 64-byte boundary ----
    M payload;                  // immutable after publish
};
```

- **Refcount protocol.** Init `rc`, then `fetch_add` once per **admitted** enqueue
  under a publisher **BUILD** ref held across the whole fan-out (so `rc` cannot
  reach zero mid-publish even if the first subscriber drains instantly). The
  publisher drops its BUILD ref last; whichever party runs the final
  `fetch_sub(acq_rel)` observing the pre-decrement value `== 1` runs the
  **type-erased `dtor` thunk** (the same mechanism the point-to-point tell path
  already carries) and returns the cell to its pool.
- **Reclaimed exactly once** whether a subscriber **consumes**, **drops** (mailbox
  full / deadline), **unsubscribes**, or **dies** — every terminal path funnels
  through the same `fetch_sub`, so there is no leak and no double-free (ADR-019
  GATE 4: ASan/UBSan/TSan clean, with skip-dec → leak and extra-dec → UAF controls
  both firing).
- **`rc` on its own cache line**, `alignas(64)` and padded away from the immutable
  `M`, so read-side consumers touching `M` do not false-share with reclaim-side
  writers hammering `rc`.

The residual **O(N) coherence traffic on the reclaim line** (N cores bouncing the
`rc` cache line as they decrement) is a **consumer-lane cost** — it lands on the
draining workers, **off the publisher's critical path** (the publish leg does only
one `fetch_add` per admitted enqueue and never waits for a decrement).

## `StreamChannel<F>` consumer-only cursor ownership (ADR-039)

`StreamChannel<F>`'s `disp`/`tail` cursors (024) are deliberately **single-writer**
— `MonotoneCursor` exposes only `load`/`store`, no RMW, so a second writer isn't
merely discouraged, it's structurally impossible to do atomically. The normative
rule this buys, made explicit after `FanOut<M, Policy>` (006, ADR-039) needed it:

> Only the ring's **single designated drainer thread**, or its **owning
> `shared_ptr`'s destructor once uniquely referenced**, may call
> `StreamChannel`'s consumer-only cursor API (`peek`/`slot_at`/
> `advance_dispatch`/`advance_tail`). A second thread reaching into those cursors
> — e.g. a **producer** trying to reclaim a departed subscriber's backlog itself
> — is a confirmed second-writer violation: the two writers race on the same
> non-atomic cursor, corrupting the ring (ADR-039's original design ran reclaim
> on the producer thread and this was caught as a cross-subscriber
> heap-use-after-free before it shipped).

The correct pattern (proven by `FanOut<M, Policy>`'s `LaneEntry`, mirroring the
already-proven `BoundedInbox<M>::~BoundedInbox` idiom for the unrelated broadcast
ring above): wrap the channel in a refcounted handle whose **destructor** drains
any still-queued items and releases their payload refs. The destructor only runs
once every reference to the handle — the membership snapshot's and the
subscriber's own drain handle — has dropped, so by construction no consumer
(the subscriber) and no producer are still touching the cursors when it runs:
single-threaded, exactly-once, refcount-gated reclaim, never a live second writer.

`StreamChannel<F>::push_blocking_while(const F&, const std::atomic<bool>&
keep_going)` (added alongside this rule) is a **departure-aware** blocking push,
distinctly named rather than an overload of `push_blocking` (an ambiguous
overload between the two was a reproduced compile defect) — it re-checks
`keep_going` around the same reverse-Dekker stall/wake protocol so a parked
producer is released when the party it's blocked on departs, not just when
credit returns. The departing side must pair a `keep_going` flip with a call to
the new `notify_departure()` on that same channel, or a producer already asleep
in `credit_gen_.wait()` has no wakeup to observe.

## Ownership summary

| Thing | Owned by | Freed when |
|---|---|---|
| Activation queue entries | Shard | Dequeued |
| Descriptor | Shard pool | Message reaches `Completed`/skipped tombstone |
| Payload | Shard arena/pool | Descriptor released |
| Actor state | Its home shard | Actor deactivated (see lifecycle policies, 005) |
| Stream ring + slots + arena (024) | Shard `pmr`, pre-allocated at stream-open (cold path) | Stream closed — **0 per-frame hot-path allocations** (measured: 0 / 50M frames). Zero-copy where the transport DMAs into the registered arena; a by-reference slot's buffer lifetime is tied to the credit/read-cursor advance (or the in-order byte-prefix credit under `ZeroCopyRetained`). |
| Reply ring + slots + arena (ADR-018) | **Caller** shard `pmr`, pre-allocated cold at `ask_stream` | Stream terminal (close/cancel/deadline) — **0 per-item steady-state heap on both the produce and drain legs** (the 024 ring flipped: callee produces, caller drains). |

An outbound streaming reply carries **0 per-item steady-state heap on both legs** —
the callee's produce leg and the caller's drain leg
([ADR-018](decisions/ADR-018-outbound-streaming-replies.md), the 024 ring run
backward). The one acknowledged non-zero is the callee's **cold `task<>` frame at 1
alloc/ask**; it is eliminable by an **optional pooled `promise_type` operator new**
(a shard frame-slab) that **does not touch the item path**.

## Open questions

- Payload sizing strategy: inline-small-payload optimization (store ≤ N bytes in
  the descriptor to skip the arena) vs. always-separate for uniformity.

Resolved: reclamation under `Reentrant` actors with overlapping payload lifetimes
— payloads are freed **per message** (pool semantics, on the descriptor's
transition to `Completed`/`Cancelled`), never by bulk drain-step reset; a
quiescent point (in-flight = 0) permits an optional arena bulk-reset as an
optimization. See `015-Reentrancy-and-Quiescence.md`.
