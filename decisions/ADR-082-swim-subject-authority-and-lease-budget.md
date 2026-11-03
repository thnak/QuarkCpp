# ADR-082 — SWIM: only a member's own word ends a suspicion of it, and silent-lease time is budgeted and alarmed (issue #54)

Status: **Proposed, rev. 1** (2026-11-03). Design only; not red-teamed; no code. Closes ADR-071 §10 N4 and the "buy `max_lease_ns` of silence per proof of life, again and
again" residual of ADR-071 §8. Consumer: AeroCoWorker network security assessment M12 (R7, TH-14, TH-15, SEC-16, SEC-17). No wire change.

## Problem
1. **N4, gossip subjects are not authenticated.** `SwimMembership::apply_update` takes any `Alive` claim with a strictly higher incarnation, whoever sent it
   (`Src::Gossip`, and `PingReqAck`, which also passes the relay's claim as `Src::Gossip`). Authentication of the *sender* (SecureTransport) does not help: the sender is
   an admitted or log-only member lying about a *third* member. Three effects, all reachable by one such member:
   - **Keep a crashed X alive.** X crashes, we probe it and mark it Suspect; the liar gossips `X Alive @ inc+1`, the Suspect is cleared, the next probe fails, we suspect again,
     the liar gossips again. X is never declared Dead.
   - **Resurrect a Dead X.** A tombstone is lifted by a strictly higher incarnation (R3), and the claim is second-hand.
   - **Poison X's incarnation.** After `X Alive @ 1_000_000` we hold X at that number; X's real first-hand frames carry a lower incarnation and are ignored as stale.
     X is then Suspect-able and cannot refute from our point of view.
2. **Lease cycling.** `max_lease_ns` (15 min) bounds ONE lease, not the sum. A member that refutes (first-hand frame at a higher incarnation) and re-announces `Suspend`
   can stay silent for about 100 % of the time and still never be Suspect, probed or gossiped to. Nothing counts it.

## Decision
### D1. Authority: first-hand frames from X are the only authority on X's incarnation and on ending OUR suspicion of X
A frame whose sender is X (`Src::Direct`, `Src::Join`) is authoritative. A second-hand claim about X (`Src::Gossip`, `Src::JoinSnapshot`) is a hint, bounded as below. This is
one choke point (`apply_update`); `PingReqAck` already enters it as `Src::Gossip`.

Each member record keeps `confirmed_inc` (highest incarnation seen in a first-hand frame from it) and `refute_credit` (how many second-hand refutations may still clear a
Suspect; reset to `Config::gossip_refute_credit` by any first-hand frame from X).

- **D1a, bound the jump.** A second-hand claim about X with `inc > confirmed_inc + Config::max_gossip_incarnation_ahead` (default 4) is dropped
  (`gossip_subject_rejected`). The recorded incarnation can therefore never be more than `ahead` above what X itself last said. A member that legitimately refuted several
  times with no first-hand contact in between is simply confirmed by its next first-hand frame.
- **D1b, first-hand lowers a claimed incarnation.** A first-hand `Alive` from X at `inc < recorded` sets the record to `inc` (X is the authority on its own number; a restart
  or a poisoned claim ends here). This is the one place the incarnation goes down, and only on X's own word.
- **D1c, a second-hand Alive clears a Suspect only with credit.** Default credit 1: the first second-hand refutation is honoured (the asymmetric-partition case, where X reaches
  others but not us, keeps working), later ones are dropped (`gossip_refutes_refused`) until X speaks to us first-hand. `gossip_refute_credit = 0` is the strict setting.
- **D1d, a second-hand Alive never lifts a tombstone to Alive.** It lifts it to Suspect with `verify_pending` (the existing `JoinSnapshot` rule), so we probe X ourselves; a
  first-hand frame from X at a higher incarnation (R3) lifts it to Alive as before.

Stated bound (SEC-16): a crashed member held up only by second-hand claims becomes Dead within one suspicion window plus one protocol period of the first refused claim
(credit 1), or within one suspicion window (credit 0), however many claims arrive. Honest refutation latency is unchanged for first-hand refutations and costs one extra
probe period when the only refutation we get is the second claim in a row.

### D2. Lease cycling: count, bound the arithmetic, alarm; enforcement is opt-in
Mobile peers legitimately cycle (Android doze), so a hard cap by default would break them. `SuspendLeases` records, per leased member, the lease time granted in a rolling
window `MobileProfile::lease_budget_window_ns` (default 24 h).
- **Counter and gauge:** `lease_grants` and `lease_ns_granted_in_window`, per member and cluster-wide.
- **Alarm:** when a member's granted lease time in the window exceeds `lease_alarm_fraction` (default 0.75) of the window, raise `SwimEvent::LeaseBudgetExceeded` once per
  window (and count `lease_alarms`). No state change.
- **Enforcement (opt-in, `lease_budget_enforce`):** a lease request that would push the member past `lease_budget_fraction` (default 1.0 = off) is clamped to the remaining
  budget; at zero the member is treated by plain SWIM (probed, Suspect, Dead). The member's first-hand frames still refute, so a real phone recovers within one period.
- **Stated bound (SEC-17):** without enforcement, a refute-and-suspend loop keeps a member silent for at most `max_lease_ns` per proof of life, i.e. up to
  `max_lease / (max_lease + gap)` of the time (about 99 % at a 10 s gap), and the alarm fires within `lease_alarm_fraction * window` of the loop starting. With enforcement,
  silent time in any window is at most `lease_budget_fraction * window`.

## Alternatives rejected
- **Signed incarnation claims (X signs `{X, inc}`).** Strongest, but SwimMembership is crypto-free by design (the core is std-only), the pinned and mTLS engines sign
  differently, every gossip entry grows by a signature (64 B on a frame that carries several), and verification lands on the gossip hot path. D1 gives the same
  protection against the three effects with first-hand evidence we already have. Kept as the follow-up if a member must be defended against a liar that can also fake
  *first-hand* frames (it cannot: those ride authenticated sessions).
- **Accept a higher incarnation for X only from X.** That is D1 with credit 0 and no jump bound; it breaks the asymmetric-partition refutation. Credit 1 and the bound keep
  it while capping the harm.
- **Hard lease budget on by default.** Breaks doze cycling for honest phones; the alarm tells the application first, enforcement is its choice.

## Evidence required (not yet produced)
Virtual-clock tests on the deferred fabric (`swim_deferred_fabric.hpp`), each with a `test_faults` control that restores the old behaviour:
| id | property | control |
|---|---|---|
| E1 (SEC-16) | crashed X, a log-only member gossips `X Alive @ rising inc` every period: X Dead at all honest peers within the D1 bound | control: X stays Alive/Suspect for 20 windows |
| E2 | `X Alive @ 1_000_000`: dropped (`gossip_subject_rejected`), X's real frames still accepted | control: poisoned, X's frames ignored |
| E3 | a Dead X plus a second-hand Alive @ inc+1 gives Suspect with verification, not Alive | control: Alive |
| E4 | honest case: X partitioned from us, reachable by others, refutes once: we clear it (credit 1) | n/a |
| E5 (SEC-17) | a 24 h refute-and-suspend loop: silent fraction matches the stated bound, `lease_alarms >= 1`; with enforcement the silent time is at most the budget | control: no alarm path (counter only) |

## Residuals
- A liar that is the ONLY path to X (a relay) can still delay Dead by one credit; it cannot extend it.
- Incarnation poisoning below `ahead` (up to 4) remains until X's next first-hand frame; harmless (D1b lowers it).
- The alarm and counters are per node; correlating across the cluster is the application's.
