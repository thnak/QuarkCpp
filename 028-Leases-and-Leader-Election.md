# 028 — Leases and Leader Election (Fencing-Token Leases)

Status: **Draft** (simulation-verified; ADR-058). Reference: [`lease.hpp`](include/quark/core/lease.hpp),
[`tests/lease_simulation_test.cpp`](tests/lease_simulation_test.cpp).

010 places an actor on one node by HRW over SWIM membership; 012 fences the *Store* so a superseded
activation cannot corrupt state (ADR-055 added a store-wide `FencingEpoch` for restore). What is missing
is the same guarantee for **singleton roles** ("the one node running the scheduler for tenant X", "the
one compactor") and a way to choose an owner *now* rather than after SWIM's detection time. This spec
adds a **lease**: a time-bounded, single-holder claim on a key, whose **fencing token** is the 012 Store
token. Leader election for a key *is* "who holds its lease".

> **The locked decision.** Safety is carried **only** by the Store's token order. A lease is an
> *optimisation* that lets a holder act without a Store round-trip per operation; it is never relied on
> for exclusion. Clocks affect only when a holder *believes* it may act, never whose write lands.

## 1. Model

* **Key**: an `ActorId` naming the role. **Arbiter**: one linearizable authority per key (in practice
  the node hosting the Store for that key, or a consensus-backed Store). **Holder/candidate**: any node.
* **Term = fencing token = `FenceToken`** returned by `Store::acquire_fence(key)`. A *new* grant calls it
  once; a renewal by the current holder does **not** (same term). `FencingEpoch` (ADR-055) is the same
  type: after a restore the store-wide floor is `epoch`, and the next `acquire_fence` returns `> epoch`,
  so leases issued before a restore are fenced out with no extra mechanism.
* **Messages**: `Request{from,id}` -> `Reply{to,id,granted,term}`. Lossy, reordered, delayed,
  duplicated, partitionable. No message is assumed to arrive; no ordering is assumed.

## 2. Protocol

Arbiter (`LeaseArbiter::on_request`, arbiter-local clock `now`):

1. During a post-restart quiet period (`now < quiet_until`): deny.
2. If `rq.from` is the recorded holder: set `expiry = now + D`, reply `granted(term)` (no new token).
3. Else if a holder exists and `now < expiry`: deny.
4. Else `term = Store::acquire_fence(key)` (strictly greater than every earlier term), record
   `{holder=from, term, expiry=now+D}`, reply `granted(term)`. If the Store is unavailable: deny.

Holder (`LeaseHolder`): sends a `Request` when it has no valid belief, or when within `renew_before` of
local expiry, retransmitting every `retry` (each retransmit has a fresh `id`; only the reply to the
*latest* `id` is applied, others are ignored). On `granted` it sets `valid_until = sent_at + D - m`,
where **`sent_at` is the local time of *sending* the request**, not of receipt, so delay can only
shorten belief. A denied holder backs off a randomised interval (liveness aid; elections are lease
races). It **keeps the last token** after belief lapses: that is what a paused ("zombie") process will
present, and the Store rejects it.

Arbiter restart (`restart`): in-memory lease state is lost; the durable truth is the Store's owner
token. The arbiter denies all requests for one full `D` so any pre-crash belief (at most `D` after a
send that preceded the crash) has lapsed. No safety property is lost because safety does not live in
that state.

## 3. Clock assumptions

Every node (holders and arbiter) uses its **own monotonic clock**. Only **intervals on a single
clock** are compared; absolute offsets between nodes are irrelevant (the simulation gives each clock an
arbitrary offset up to 10^12). The one assumption is a **bounded rate error**: each clock runs at
`(1 +/- rho)` of real time (default rho = 5%, `Config::drift_ppm_bound`). Then holder belief ends before
the arbiter's expiry in real time iff `m >= 2*rho*D/(1+rho)` (`Config::margin()`, plus 2 ticks for
quantisation). Derivation: belief lasts at most `(D-m)/(1-rho)` of real time after send; the arbiter's
expiry is at least `D/(1+rho)` after receipt, and receipt is not before send.

What this buys: **lease-belief exclusivity** (at most one node believes it leads at any real instant).
What it does not buy: anything if rho is violated (a clock stepped or slewed beyond rho, a VM resume that
jumps the monotonic source). A violated rho can yield two *believers*; **it cannot yield two accepted
writers** (section 4). A node whose monotonic clock is known to have jumped must drop its belief (rebuild
the `LeaseHolder`). Use a non-adjustable monotonic source (PAL steady clock), never wall-clock.

## 4. Safety invariants (asserted by the simulation)

* **S1 Term uniqueness and monotonicity.** Each term is issued to exactly one holder; new terms are
  strictly greater than all previous. The source of truth is the Store's `acquire_fence`, so this
  survives arbiter restart and (with ADR-055) backup/restore. Re-granting the same term to the same
  holder is not a new term.
* **S2 Deposed leader is fenced.** A write carrying any token below the highest term ever granted is
  rejected by the Store (`errc::unavailable`, 012). The Store accepts iff `tok >= owner` and `tok != 0`.
* **S3 No back-in-time accept.** Accepted writes carry non-decreasing tokens in real-time order.
* **S4 Log integrity.** The log is exactly the accepted writes, contiguous.
* **S5 Belief exclusivity**, only under bounded drift (section 3).

S1-S4 are independent of loss, delay, duplication, partitions, pauses, arbiter restart and clock rate.
"Two leaders whose writes the Store would both accept" cannot exist: two distinct nodes never hold the
same term (S1) and the Store accepts only `tok >= owner`, so once `t2 > t1` is issued, `t1` is below
owner forever.

## 5. Residual windows the protocol cannot close (documented, not hidden)

* **Check/act gap.** A leader that checks `believes_leader` and then *externally* acts (sends an email,
  calls a non-fenced service) can act after losing the lease. Only operations that carry the token to a
  fencing-aware sink (the Store; remote calls carrying the term) are protected. Effects beyond the Store
  are only as safe as the sink's own fencing.
* **The arbiter is a single point of availability** per key (not of safety). If it is unreachable no new
  leader is elected, and existing leaders lapse after `D-m`. Replicating the arbiter is the Store's job
  (a consensus-backed Store), not this layer's.
* **Liveness is best-effort.** With a reachable arbiter and no sustained partition a leader emerges
  within about `D` plus backoff; under sustained loss or partition none may. Nothing stronger is claimed.

## 6. Composition

* **012 / ADR-055.** Term == `FenceToken`; the restore epoch is a lease floor (`restore_epoch`). Every
  durable write by a leader goes through the existing `Store::append(id, tok, ...)`.
* **010 SWIM.** Membership is a **hint, never evidence**. A SWIM `Dead`/`Suspect` event for the current
  holder MAY prompt candidates to request early (skip backoff); it MUST NOT shorten or revoke the lease,
  or make the arbiter grant before `expiry`. A SWIM false positive (slow, not dead) therefore costs
  nothing in safety: the slow node is still bounded by its lease and fenced by the Store. A candidate
  SWIM believes dead MAY be skipped locally (liveness only).
* **026 / HRW placement.** The preferred holder MAY be the HRW owner; election remains a lease race, so
  a placement disagreement during membership change cannot produce two accepted writers.
* **022.** Requests are small and idempotent per `id`; lease traffic should be shed last.

## 7. Non-goals

Byzantine nodes; consensus among arbiters; wall-clock-based expiry; leases on the engine hot path;
multi-key atomic leadership. Wiring into the 010 transport is not part of this draft (arbiter and holder
are transport-agnostic state machines).

## 8. Open questions

Explicit `release` for fast handover (safe: arbiter clears holder, term unchanged until next grant).
Per-key vs batched arbiters at 026 scale. Whether renewal should piggyback on SWIM gossip.
