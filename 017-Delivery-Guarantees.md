# 017 — Delivery Guarantees (Effectively-Once)

**Cross-cutting resolution.** Composes messaging (006), distribution delivery
(010), persistence fencing (012), and message identity (016) into a defined set of
end-to-end guarantees — and delivers the *worked example* the effectively-once
open question asked for. The headline claim the RFC has made since 010 —
"exactly-once is not a transport property; effectively-once is built from
at-least-once + idempotent handlers + fenced persistence" — is made concrete and
stress-tested here.

## The three levels (a policy)

```cpp
class Payment : public quark::Actor<Payment,
                    Sequential,
                    Persistent<EventSourced, PersistMode::Sync>,
                    Delivery<DeliveryLevel::EffectivelyOnce>> {};
```

| Level | Guarantee | Cost | Requires |
|---|---|---|---|
| **`AtMostOnce`** (default) | Deliver 0–1 times; no retry | free | nothing |
| `AtLeastOnce` | Deliver ≥1 time; handler must be idempotent | retry + ack | — |
| `EffectivelyOnce` | Effect (state + outbound) happens exactly once despite duplicate delivery, crash-replay, and re-placement | dedup state + CP store | `Persistent` + a linearizable `StateStore` |

Exactly-*once delivery* remains impossible (two-generals); `EffectivelyOnce` is
exactly-once **effects**. Validation (008) rejects `EffectivelyOnce` on a
non-persistent actor or an eventually-consistent store.

## Message identity, made deterministic

Effectively-once needs to *recognize* a duplicate, which requires stable identity:

- Every message has `MessageId = (sender_id, seq)` where `seq` is a per-`(sender,
  receiver)` monotonic counter. Combined with FIFO delivery (006), a receiver can
  dedup with a **per-sender high-water-mark** rather than storing every id —
  bounded to O(active senders), not O(messages).
- **Outbound** messages a handler sends get **deterministically derived** ids:
  `child.id = derive(inbound.id, outbound_index)`. So if the handler is replayed
  (crash before commit), it produces the *same* outbound ids, and their receivers
  recognize the duplicates. Only id-derivation must be deterministic — not the
  whole handler.

## The mechanism: commit the effect atomically, deliver output after

Processing message `M` in an effectively-once actor:

1. **Dedup check.** If `M.seq ≤ watermark[M.sender]`, `M` is a duplicate → skip the
   effect and re-emit the recorded reply/ack (idempotent). Else continue.
2. **Handle.** Run the handler, producing a state change (or event), zero or more
   **outbound** messages, and an optional reply.
3. **Commit atomically** — one durable transaction (natural on the append-only WAL,
   012) containing: the state change/event, the advanced `watermark[M.sender]`,
   the outbound messages (a **transactional outbox**), and the reply. The write
   carries the activation's **fencing token** (012); a stale token → **rejected**
   (this activation is a partitioned zombie — abort, escalate 007).
4. **Deliver output after commit.** A post-commit dispatcher drains the outbox,
   sending each outbound message with `AtLeastOnce` retry; each is dedup'd at *its*
   receiver by the same mechanism. The reply is likewise sent post-commit.

State, dedup marker, and outbox commit **together or not at all**. That single
atomicity is what makes the whole chain effectively-once by induction.

## Why output goes *after* commit (self-debate)

- **Send before commit:** crash after send, before commit → on replay the handler
  re-runs and re-sends. Duplicate output — tolerable *only because* the receiver
  dedups, but the sender's own state may not reflect the send. Fragile.
- **Send after commit:** crash after commit, before send → on recovery the outbox
  is durable and the dispatcher re-drains it. Output is **at-least-once from a
  durable record**, dedup'd downstream. No lost output, no state/output skew.
- **Decision:** transactional outbox — record intent to send inside the commit,
  deliver after. This also means a **fenced-out zombie sends nothing**: its commit
  is rejected, so its outbox never becomes durable, so the dispatcher never runs.
  Fencing protects state *and* output with one gate.

## Worked example — Client → Order → Inventory

`Order` and `Inventory` are both `EffectivelyOnce`. Trace each failure mode:

| Scenario | What happens | Effectively-once holds? |
|---|---|---|
| **Happy path** | `Order` commits {reserve event, watermark++, outbox: `Reserve` to `Inventory`, reply}; dispatcher sends `Reserve`; `Inventory` commits once. | ✔ once each |
| **Duplicate delivery** of client msg | Second copy has `seq ≤ watermark` → skipped, cached reply re-sent. | ✔ effect once |
| **`Order` crashes before commit** | No durable effect; on recovery the (retried) client msg is reprocessed fresh. | ✔ once |
| **`Order` crashes after commit, before sending `Reserve`** | Recovery reloads state, dispatcher re-drains outbox, `Reserve` sent (maybe again). `Inventory` dedups by `Reserve`'s deterministic id. | ✔ once |
| **`Reserve` delivered twice** (retry) | `Inventory` sees `seq ≤ watermark` → skips, re-acks. | ✔ once |
| **Partition: two `Order` activations** | Both process; both attempt commit; the store accepts only the higher fencing token; the zombie's commit is rejected → no state, no outbox, no `Reserve`. | ✔ once (zombie is inert) |

The partition row is the acceptance test the open question demanded: a
double-activated actor under partition produces exactly one durable effect and one
downstream `Reserve`, because commit is fenced and output is post-commit.

## The consistency price (CAP)

`EffectivelyOnce` makes the `StateStore` the **linearizable consistency anchor**:
fencing tokens are leases acquired from it, and durable commits are ordered by it.
If the store is unreachable, an effectively-once actor **cannot make durable
progress** — it fails fast / escalates (007) rather than risk a split effect.
These actors are therefore **CP**: they sacrifice availability under a store
partition for correctness. Actors needing availability choose `AtLeastOnce` +
idempotent logic (AP, best-effort) instead. This tradeoff is explicit and
per-actor.

## Interaction notes

- **Reentrancy (015):** dedup check and commit both happen in the synchronous
  region on the actor lane, so concurrent in-flight handlers don't race the
  watermark; commit sequence numbers order them.
- **Reply cache:** the recorded reply lets a duplicate `ask` re-emit the identical
  reply without reprocessing (needed because retried `ask`s are the common
  duplicate source).
- **Dedup GC:** watermarks are O(senders); a sender that goes permanently silent
  leaves a stale watermark entry — reclaimed by an idle-sender TTL (open question).

## Open questions

- Watermark reclamation policy for long-dead senders (TTL vs. explicit teardown).
- Whether the transactional outbox is per-actor or shard-batched for throughput.
- Cross-`type_key` deterministic id derivation stability across binaries/platforms
  — depends on the same `type_key` stability caveat as 008/016.
- **Outbound id derivation under fan-in.** The pure `child.id = derive(inbound.id,
  outbound_index)` gives a strictly-monotonic outbound `seq` along a **linear** chain
  (Client → Order → Inventory — the worked example), so the receiver's per-sender
  high-water-mark dedups correctly. At a **fan-in** deriving actor (one that emits to a
  single downstream while consuming from *multiple* upstreams with independent
  `inbound.seq` ranges), two upstreams' derived outbound seqs can overlap — the
  downstream watermark would then wrongly skip a genuine message. Robust fan-in needs a
  **durable per-`(sender, receiver)` outbound counter** (advanced inside the same atomic
  commit as state+watermark+outbox), not a pure function of the inbound id alone. The
  linear-chain derivation is correct and is what the acceptance test exercises; the
  durable-counter form is the fan-in generalization, deferred with the id-derivation
  open question above.
