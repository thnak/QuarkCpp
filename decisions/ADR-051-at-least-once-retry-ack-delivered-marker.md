# ADR-051: AtLeastOnce retry+ack, durable outbox delivered marker, bounded dedup maps

Status: Accepted (std-only core; engine wiring deferred). Issue #19 and the trimming half of #24.
Governs: `include/quark/core/delivery.hpp`; spec 017 (and 010 for the wire).

## Decision

1. **Delivered marker.** New internal Described record `DeliveredRecord{out_sender, out_seq}`
   (`quark_type_salt = 0x5157444C31`, per ADR-050). `EffectivelyOnceLane::mark_delivered` writes it as
   a fenced `append_batch` and removes the entry from the pending outbox. `deliver_outbox(sink)` takes
   `sink -> result<bool>` (true = receiver acked) and marks acked entries. `drain_outbox` now only
   walks pending entries. `recover()` replays `DeliveredRecord`s (always after their outbox record) and
   erases them from the rebuilt outbox. A fenced zombie cannot mark anything delivered.
2. **Retry + ack.** `ReliableSender` / `ReliableReceiver`: transport-agnostic go-back-N over a
   consecutive per-(sender, receiver) seq starting at 1. The sender re-sends unacked frames in order
   each `pump`; the receiver delivers only `seq == watermark+1`, acks cumulatively, and drops
   duplicates/gaps. Each frame carries `base` (oldest unacked seq) so a receiver that never saw or
   evicted the sender re-admits it at `seq == base`. A plain high-water-mark alone is NOT safe over a
   lossy link (seq 2 arriving before a lost seq 1 would permanently skip 1), hence in-order acceptance.
3. **Bounded maps.** `set_limits(watermark_cap, reply_cache_cap)` on the lane and a cap on
   `ReliableReceiver`; oldest-touched-first eviction (`detail::BoundedMap`); 0 = unbounded (default,
   behaviour unchanged). `recover()` re-applies the cap.

## Trade-offs / honest caveats

* Eviction weakens dedup: an evicted sender's duplicate is no longer recognised from RAM, so
  effectively-once degrades to at-least-once for senders idle past the cap, and an evicted cached
  reply is not re-emitted. The durable log is untouched. Size the cap >= active senders. A stronger
  fix (consulting the durable log on a miss, or ack-driven trimming) is not built.
* On the `ReliableReceiver` path eviction can cause a redelivery (duplicate), never a loss.
* The `Delivery<AtLeastOnce>` policy still does not auto-wire the engine's remote `tell` to
  `ReliableSender`: engine.hpp/dispatch were out of scope (concurrent owner). The mechanism is
  provided and tested at the delivery.hpp seam; the caller (transport/engine) supplies the sink and
  ack return path. The issue's "netem" gate is satisfied by a deterministic seeded in-process 30%
  frame-loss simulation (`tests/delivery_at_least_once_test.cpp`), not a real network.
* The `ReliableSender` queue is unbounded and uses `std::deque`/`std::string` (retry path, not the
  mailbox hot path); no virtual/RTTI.
* `mark_delivered` is one fenced commit per ack (no batching); `deliver_outbox` is O(n^2) in pending
  entries. A shard-batched outbox / ack cursor remains deferred (017 open question).
* Fan-in `derive_outbound` monotonicity limitation (existing) is unchanged.
