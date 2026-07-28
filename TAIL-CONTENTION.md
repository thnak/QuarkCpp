# Why `tail_` stalls the mailbox

*Companion explainer to [002 — Scheduler](002-Scheduler.md) §Mailbox hot-path baseline and
[ADR-031](decisions/ADR-031-mailbox-mpsc-hot-path-r8-judgment.md)/[ADR-032](decisions/ADR-032-mailbox-mpsc-hot-path-r9-judgment.md).*

One atomic variable decides who gets to send a message next. When only one
thread ever sends, that costs nothing. When several do, it becomes the whole
system — and adding more senders stops helping almost immediately.

| | |
|---|---|
| **+2.7%** | throughput gained, 2→4 producers |
| **−40%** | per-producer rate, 1→2 producers |
| **1** | cache line, shared by every sender |

## Mechanism: one variable, one owner at a time

Every actor's mailbox is a queue with a single tail pointer, `Mailbox::tail_`.
To send a message, a thread does one atomic swap against it — on x86, a
`lock xchg` instruction:

```cpp
Descriptor* prev = tail_.exchange(d, memory_order_acq_rel);
prev->link.next.store(d, memory_order_release);
```

That's the entire publish path. No retry loop, no lock, no CAS. It's
deliberately the cheapest possible way to hand a message to a queue — *if
you're the only one doing it.*

The instruction needs one thing first: exclusive ownership of the 64-byte
cache line `tail_` lives on. A single core can get that instantly — nothing
else wants the line. That's why one producer alone gets close to the
mailbox's raw ceiling.

## What changes with a second sender

```
Every producer swaps the same line

  [producer A]  [producer B]  [producer C]  [producer D]
        \             \        /             /
         \             \      /             /
          \             \    /             /
           '------->  tail_  <-----------'
                (one 64-byte line, one owner at a time)
```

Every producer's swap has to wait for exclusive ownership of the same line.
More producers means more waiting for the line to arrive, not more messages
landing per second — the physical cost of passing that one line between
cores becomes the ceiling.

This is a cache-coherency cost, not a software inefficiency: the CPU's own
protocol for keeping memory consistent across cores requires that only one
core hold a line writable at a time. Every contended swap includes the time
to invalidate the previous owner's copy and transfer the line — tens of
cycles, more again across sockets. That transfer cost is what shows up as
"no scaling," not the instruction itself.

## Evidence

Isolated test: *N* producer threads, each with its own pre-allocated
messages (no shared allocator, nothing else contended), all publishing into
one mailbox drained by one consumer. Pinned to a single CPU socket.

| Producers | Total M msg/s | Per-producer M msg/s | Gain vs. previous row |
|---:|---:|---:|---:|
| 1  | 9.51  | 9.51 | — |
| 2  | 11.31 | 5.66 | +18.9% |
| 4  | 11.62 | 2.90 | +2.7% |
| 8  | 15.80 | 1.98 | +36.0% |
| 15 | 18.34 | 1.22 | +16.1% |

Doubling producers from 2 to 4 adds **2.7% total throughput** — effectively
nothing — while each producer's own share keeps falling. That is what "stuck
at zero marginal scale" looks like in practice: the line, not the core
count, is the ceiling.

A same-socket vs. cross-socket check at P=4 ruled out NUMA distance as the
cause: confined to one socket, throughput was **12.51 M/s**; deliberately
split across two sockets, **10.37 M/s** — worse, as expected, but the
flattening is already there *within one socket*. The bottleneck is the line
itself, not where the cores sit.

ADR-032 r9 reconfirmed the same shape with a direct measurement on the
shipped mailbox: P=1→4 aggregate enqueue throughput degrading ~24-29%
(28.97→21.93 Mops/s gcc, 24.81→17.62 Mops/s clang) — same mechanism, a
different host, a different round.

## Why it matters: this only bites one specific pattern

The scaling ceiling above applies to *many senders → one actor*: a shared
logger, a counter, an aggregator, anything that many threads all `tell()` at
once. It does *not* apply to spreading work across many independent actors —
each actor has its own mailbox and its own `tail_`, so 100, 1,000, or
100,000 actors under load show *no* throughput difference by population
size. The ceiling is per shared line, not per system.

It's also not a bug in the traditional sense. The design is a deliberate
trade: pay nothing when uncontended, pay the coherency cost only when
several senders actually collide on one actor. That trade was made
explicitly (ADR-004) and has held for nine design rounds since.

## Status

- **ADR-029** — flagged the P=1 vs P=4 gap as real, but *"still undiagnosed."*
- **ADR-031** — mechanism isolated and proven: shared cache-line contention
  on `tail_`, confirmed independently of pool effects and of NUMA placement.
  Diagnosing it is not the same as fixing it.
- **ADR-032** — reconfirmed the mechanism with a direct measurement on the
  shipped mailbox, then tried two fresh producer-side redesigns (SBR-v5,
  SEG-REX) purpose-built to remove the shared line. Both were disqualified
  on separate grounds (broken ordering guarantee; a sanitizer-invisible
  reclamation deadlock plus a 3-4x throughput loss) — neither closed the gap.
  SEG-REX's bounded-segment-reversal mechanism did achieve flat p999
  oldest-message-discovery latency independent of backlog depth, a real
  reusable result recommended for pairing with proper hazard-pointer/RCU
  reclamation in a future round, rather than re-attempting SEG-REX as
  specified.
- **Open** — closing the gap still needs a producer-side design that gives
  each sender its own cache line, merged fairly by the one consumer, without
  giving up the mailbox's FIFO/ordering guarantee. Not yet attempted as a
  fresh round.

---
*Field notes on the mailbox MPSC lineage, ADR-029 through ADR-032 — informational, not a spec or an ADR itself.*
