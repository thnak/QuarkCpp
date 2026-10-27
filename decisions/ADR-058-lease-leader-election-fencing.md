# ADR-058 — Leases and leader election with fencing tokens (#22)

Status: Accepted as a **simulation-verified Draft** (spec 028). Evidence: `tests/lease_simulation_test.cpp`
(run under MSVC Release, Windows/x86-64). Reuses ADR-055's `FencingEpoch` = `FenceToken`.

## Decision
A lease's fencing token is the 012 Store's own `acquire_fence(key)`, issued by a single arbiter. Safety
rests **only** on the Store's `tok < owner` rejection; clocks gate only the holder's *belief*. Rejected
alternatives: (a) wall-clock-expiry leases whose safety needs synchronised clocks; (b) a token minted by
the arbiter independently of the Store (a second monotonic source to reconcile with restore epochs).
Reusing the Store token means arbiter restart, restore (ADR-055) and takeover inherit monotonicity.
Belief uses sender-anchored expiry with a drift margin `m = 2*rho*D/(1+rho) + 2`.

## Evidence (simulation, deterministic seeds 1..N)
`lease_simulation_test` default: **10,000 scenarios in ~0.8 s**; also run once at **200,000 scenarios
(18.7 s), PASS** (`QUARK_LEASE_SCENARIOS` overrides). Per scenario: 3-5 candidates + an arbiter over a
real `InMemoryStore`; loss 0-45%, delay up to 140 ticks (greater than lease D=100), duplication 0-15%,
partitions re-cut every 150 ticks; per-node clock offset up to 10^12 and rate error; long node pauses
with zombie writes; random arbiter restarts; 1500 ticks each. 10k run: 48,341 terms issued, 3.98M writes
accepted, 1.55M stale-token writes all rejected. Asserted: S1 term uniqueness/monotonicity; S2 stale
tokens always rejected (checked against the Store's owner AND an independent observer's max granted);
S3 accepted tokens non-decreasing; S4 log == accepted writes; S5 at most one believer in the 7,083
drift-bounded scenarios (0 overlap ticks). In the 2,917 scenarios with clock rates 0.4x-2.5x (far outside
the 5% bound) belief overlap DID occur (178 scenarios) while S1-S4 still held, confirming the layering.
Mutation check: forcing `margin()` to 0 makes S5 fail (8 overlap ticks in 10k), so the test is not vacuous.

## Liveness (best-effort, not a claim)
Only the fault-free class (1/8 of seeds) asserts that a leader exists at the end (1,250/1,250). Under the
full fault mix a leader existed at the final tick in only about 23% of scenarios (2,340/10,000), which is
expected with 45% loss, repeated partitions and a single arbiter. Reported, not asserted.

## What the simulation does NOT cover
* **Real clocks.** Rate error is a constant per-node ratio; no clock steps/back-jumps, NTP slew, VM
  pauses that freeze the monotonic clock, or non-linear drift. Tick quantisation is simplistic.
* **Real network.** No TCP semantics, head-of-line blocking, or the actual 010 transport; loss is
  independent per message; partitions are random per-link cuts.
* **Byzantine behaviour**, forged tokens, a compromised arbiter or Store (020 is the authenticity layer).
* **A replicated arbiter / consensus Store.** The arbiter crash is modelled as state loss plus a quiet
  period with the Store intact. Store unavailability is not injected, and Store calls are instantaneous
  (no in-flight write racing a newer `acquire_fence`); a remote Store's linearizability is assumed.
* **Effects outside the Store** (spec 028 section 5 check/act gap): only Store writes are fenced.
* **Concurrency/sanitizers.** The state machines are single-threaded by design; not run under
  ASan/UBSan/TSan or GCC/Clang (MSVC on Windows only in this session).
* SWIM integration is specified (hint-only) but not implemented or simulated.

## Follow-ups
Wire arbiter/holder to the 010 transport; clock-step/back-jump injection; Store-unavailable and
in-flight-write injection; run the simulation under GCC/Clang and sanitizers in CI.
