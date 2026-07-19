# 018 — Clocks and Deadlines

**Cross-cutting resolution.** Defines which clock backs deadlines and how a
deadline survives crossing a node boundary. Touches the message context (004),
`ask` (006), the transport (010), and the timer wheel (011). The problem it
solves: a deadline is a *monotonic duration* concept, but the only cross-node
comparable clock (wall time) is neither monotonic nor trustworthy.

## Two clocks, two jobs

| Clock | Property | Used for | Not usable for |
|---|---|---|---|
| **Monotonic** (PAL canonical, `steady_clock`-class) | never runs backward; **per-node arbitrary epoch** | measuring durations, driving the timer wheel (011), all deadlines | comparing an instant across nodes |
| **Wall** (`system_clock`, UTC) | comparable across nodes *in principle* | calendar/"at 09:00 UTC" scheduling | deadlines — it steps (NTP), can jump backward, isn't monotonic |

Deadlines must be **monotonic**: a 500 ms timeout means 500 ms of elapsed time,
not "until a wall-clock instant" that an NTP correction could move. So deadlines
are always measured against the monotonic clock. But a monotonic *instant* from
node A (epoch = A's boot) is meaningless on node B. That contradiction is the
whole problem.

## Rule 1 — deadlines are local monotonic instants

Inside a node, `MessageContext::deadline` is an absolute **monotonic time_point**
(fast to compare in the wheel, 011). The monotonic clock is supplied by the
Platform Abstraction Layer (019) as a single canonical source across
Linux/Windows/macOS (so "monotonic" means the same thing on every platform — see
suspend note below).

## Rule 2 — across a node boundary, ship *remaining duration*, not the instant

The transport (010) translates the deadline at the boundary:

```
send (node A):     remaining = deadline_A − now_A            // a duration
wire:              carries `remaining_ns` (+ trace/stop, 004)
receive (node B):  deadline_B = now_B + remaining − transit  // reconstruct locally
```

The absolute instant never crosses the wire — only a duration does, reconstructed
against the receiver's own monotonic clock. This is a **special-cased field**
handled by the transport, *not* a normal `describe`d field (016): it must be
computed at send and reconstructed at receive.

### Rule 2a — charge network transit to the budget (reuse SWIM's RTT)

Naively, `deadline_B = now_B + remaining` is too lenient by the transit time — the
budget kept shrinking while the message was in flight. We subtract a transit
estimate. **We get it for free:** SWIM membership (010) already pings peers and
tracks a smoothed round-trip time, so `transit ≈ rtt/2`. No separate PTP/NTP
handshake is introduced.

## Rule 3 — deadline inheritance

When a handler sends outbound/derived messages (007, 017), they **inherit the
current `MessageContext`'s remaining deadline by default**, so a downstream call
can never outlive the deadline that caused it. Across `A → B → C`, each hop
recomputes remaining and subtracts transit, so the budget is **monotonically
non-increasing** along the causal chain — the end-to-end deadline is respected by
construction. A handler may explicitly set a *tighter* sub-deadline; it may not
loosen the inherited one.

## Rule 4 — bias lenient, never falsely strict

Clock and RTT estimates are imperfect. The translation is designed so errors bias
toward **slightly lenient**, because a false deadline miss (killing healthy
in-progress work) is worse than granting a few extra milliseconds:

- The transit subtraction uses a **conservative** (not inflated) `rtt/2`.
- If no RTT estimate exists yet (first message to a fresh peer), subtract **nothing**
  — lenient, never stricter than the sender intended.

## Suspend, resume, and the canonical monotonic clock (cross-platform)

`steady_clock` behavior across machine suspend differs by OS (some pause, some
count sleep time). The PAL pins a **single canonical monotonic clock that counts
suspend time** (e.g. `CLOCK_BOOTTIME` on Linux, the equivalent elsewhere), so a
node that was suspended sees its in-flight deadlines as **already expired on
resume** — the safe outcome, since that work is stale. Without this, a suspended
node could hold "live" deadlines that are hours stale.

## Wall-clock / calendar scheduling is a separate concern

The duration-propagation scheme covers *deadlines and relative timers* (011:
`schedule_after`, `schedule_every`). **Calendar** scheduling ("run at 09:00 UTC")
is inherently wall-clock and, if added, uses `system_clock` and does **not**
participate in remaining-duration propagation. It is out of scope here and noted
so the two are not conflated.

## Reply-stream deadlines (ADR-018)

An outbound streaming reply (`ask_stream`, ADR-018 — the 024 credit-ring run
backward, callee = producer, caller = consumer) carries **one** deadline for the
whole stream, and it obeys Rule 2 unchanged: the deadline travels as
**remaining-duration**, and the callee reconstructs it locally —

```
callee (node B):  deadline_B = now_B + remaining − transit   // pal::BootClock
```

— never a raw remote instant (same reconstruction as Rule 2/2a; `transit` reused
from SWIM's RTT). The clock is `pal::BootClock` (CLOCK_BOOTTIME, §Suspend), so a
stream whose callee was suspended sees its deadline **already expired on resume**.
On expiry the wheel entry (011) fires the **terminal CAS**, which performs the
ADR-018 **two-part wake**: it **arms the caller's drain** *and* **bumps
`credit_gen`**, so both a blocked consumer and a producer stalled on a full credit
window observe teardown (002 §Reply-stream terminal edge). Nothing is delivered
after the terminal CAS.

## Self-debate

- **Ship an absolute UTC deadline instead of a duration?** Both ends measure
  against shared UTC, so transit is handled implicitly and multi-hop needs no
  recomputation. Rejected: wall clocks step and drift (NTP), so a backward step
  could silently extend or violate a deadline — deadlines would lose monotonicity,
  the one property they exist to provide.
- **Add a dedicated PTP/NTP clock-offset handshake?** More accurate transit
  accounting. Rejected as unnecessary machinery: SWIM already measures RTT, and
  the *never-falsely-strict* bias means we don't need sub-millisecond precision —
  a rough `rtt/2` that errs lenient is sufficient.
- **Decision:** ship remaining duration, reconstruct on the receiver's monotonic
  clock, subtract a conservative SWIM-derived transit estimate, inherit down the
  causal chain.

## Interaction

- **011** — a reconstructed `deadline_B` is just a local wheel entry; expiry
  behaves identically to a locally-originated deadline.
- **017** — a missed deadline drives the message to failure/dead-letter; deadline
  inheritance keeps effectively-once chains bounded.
- **009** — `deadline_misses` counts expiries; a spike in cross-node misses is a
  signal of RTT underestimation or clock issues.

## Dependencies

The PAL's canonical monotonic clock; reuses SWIM's existing RTT measurement (010);
`std::chrono` otherwise. No time-sync daemon dependency is introduced.

## Open questions

- Asymmetric routes where `rtt/2` poorly estimates one-way transit — is a
  one-way-delay estimate worth the complexity, or does the lenient bias absorb it?
- Very long deadlines (hours/days) spanning a suspend: confirm `CLOCK_BOOTTIME`-class
  semantics are what such actors want, vs. an opt-in wall-clock deadline.
- Whether deadline inheritance should be opt-out per send for genuinely
  fire-and-forget outbound work that should outlive the triggering deadline.
