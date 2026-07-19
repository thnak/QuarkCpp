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
