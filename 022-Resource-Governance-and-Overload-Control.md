# 022 — Resource Governance & Overload Control

020 named DoS/rate-limiting an explicit **non-goal** ("an availability concern;
backpressure is the nearest existing lever"), and 021 left **hand-off bandwidth**
as an open question. Both point here. This spec is the **availability** sibling of
020's confidentiality/integrity: how a fixed-capacity engine stays alive and fair
under more load than it can serve — whether the excess is malicious (a flood from
an authenticated-but-abusive source), accidental (a retry storm, a thundering
herd, a hot key), or structural (a slow downstream causing an in-flight pileup).

> Governance is about bounding **shared, exhaustible resources** so contention is
> fair and overload **degrades gracefully instead of collapsing**. A single-node
> engine still benefits (a runaway local sender), but like security most cost is
> incurred at boundaries — none of it is mandatory, all of it is a seam with a
> self-contained default.

## Scope — what this defends, and what it does not

The honest boundary, stated as sharply as 020 states its crypto boundary:

| Layer | Threat | Whose job |
|---|---|---|
| **L3/L4 volumetric** — SYN flood, amplification, bandwidth exhaustion | Saturate the pipe before a byte reaches the app | **Not the engine.** Kernel, firewall, load balancer, DDoS scrubbing. |
| **Connection/accept** (010, 021) | Exhaust FDs / accept queue with half-open or idle peers | Engine: accept-rate + concurrent-connection caps |
| **Ingress request rate** (external → actor) | Flood an actor faster than it can serve | Engine: rate limit keyed by principal (020) |
| **Internal amplification** | One message fans out to thousands; a hot key starves its shard-mates | Engine: fair-share + concurrency caps + fan-out budget |
| **Downstream pileup** (`ask` to slow/failing target) | Unbounded in-flight asks pin memory + worker time | Engine: circuit breaking + deadline shedding |
| **Memory / mailbox exhaustion** | Unbounded queue growth OOMs the node | Engine: bounded everything + load shedding |

The dividing line, made explicit: **the engine governs application-layer resource
use on traffic that has already arrived; it does not defend the wire.** Volumetric
network DoS is upstream infrastructure — pretending the engine solves it would be
the availability equivalent of rolling your own crypto (020).

## The core principle: bound every exhaustible resource

Every DoS, malicious or accidental, is ultimately *"an unbounded resource met an
unbounded demand."* The foundation is therefore not a clever algorithm but an
invariant:

> **No queue, pool, buffer, connection table, or in-flight set is unbounded.**
> 006's bounded mailbox is the archetype; this spec generalizes it to every
> exhaustible resource the engine owns.

Once everything is bounded, overload stops being *"collapse"* and becomes *"a
bound is hit"* — a well-defined event the engine can shed, delay, or fail on
predictably. The rest of the spec is about **hitting bounds gracefully**.

## The governed resources

| Resource | Bound (default) | Enforcement point |
|---|---|---|
| Mailbox depth | per-actor `Overflow` policy (006) | `tell`/`ask` enqueue |
| Payload memory (003) | per-shard + global budget | payload allocation |
| Worker time (002) | fair share across principals / actor-types | scheduler admission |
| In-flight concurrency (015) | `MaxConcurrency<N>` per actor | handler admission |
| Connections (010, 021) | accept-rate + concurrent-peer cap | connection accept |
| Ingress request rate | token bucket per principal (020) | ingress boundary |
| `ask` in-flight to a target | circuit breaker | send time |
| Inbound stream memory (024) | per-stream **credit window** (`capacity × slot`) | producer credit check |
| Outbound reply-stream memory (ADR-018) | per-stream reply **credit window** + concurrent-streaming-ask **admission cap** | callee (producer) credit check + `ask_stream` admission |

**The stream credit window is the per-stream backpressure lever** (024). The ring
bounds resident memory; a fast producer **stalls via credit depletion** — lossless,
no mid-stream drop — which is the streaming realization of the bound-every-resource
invariant. This is the one place governance is **flow control, not shedding**:
shedding a stream mid-flight corrupts it (the per-item-`tell` baseline shed
3.9–4.8M frames in the ADR-005 benchmark), so a stream is bounded by *stalling the
producer*, not dropping frames. `credit_limit` may narrow the window with a relaxed
store, never on the per-frame path.

The **reply direction is symmetric** ([ADR-018](decisions/ADR-018-outbound-streaming-replies.md)):
an outbound streaming reply is the 024 ring run backward (callee = producer, caller =
consumer), so its backpressure is likewise the **credit window — a producer stall,
never mid-stream shedding** (shedding a reply mid-flight corrupts it exactly as
inbound). Two 022 numbers govern it: the **admission cap on concurrent streaming
asks** (bounds the idle-density footprint of one whole ring per in-flight ask) and
the **default reply-ring capacity**. Both inherit 024's adaptive-`credit_limit` open
item — the default cap is static-and-roughly-right, adaptive tuning stays opt-in.

## Mechanisms

### 1. Rate limiting — one cheap decision, at the boundary

Rate limiting lives at the **same ingress boundary as authorization (020)** —
next to "*may* this principal?" sits "*may it, this fast?*" One seam, one hot-path
decision:

```cpp
enum class Admit { Accept, Shed, Delay };

struct RateLimiter {
    // keyed by principal (020) / actor-type / ingress source; Cost lets a
    // heavy message consume more than one token. O(1), no allocation.
    virtual Admit check(const GovernanceKey&, Cost) = 0;
};
```

- **Default algorithm: token bucket.** It permits bursts up to the bucket size
  (real traffic is bursty), needs O(1) state, and is trivially correct — versus a
  sliding-window log (O(window) memory) or a leaky bucket (no burst tolerance).
- **Ordering vs. authz:** an *unauthenticated* flood is shed **before** the authz
  check runs — cheap rejection first, expensive identity validation only on traffic
  that cleared the rate gate. This is the one place governance runs *ahead* of 020.

### 2. Load shedding — shed early, shed the doomed first (self-debate)

- **Queue-and-delay under overload.** Buffer the excess and serve it later. But
  Little's law is merciless: if arrival rate exceeds service rate, a deeper queue
  only converts a throughput problem into an **unbounded latency + memory** problem,
  and the node eventually collapses anyway — having wasted memory to get there.
- **Decision: shed, don't buffer.** When a bound is hit, **reject at admission**
  with a typed `overloaded` error (007) — fail-fast, before the work is done, so a
  shed message costs almost nothing. `Delay` (brief, bounded) is offered for
  smoothing, but deep queuing is refused by design.

**What to shed first** — shedding is not random; it drops the *lowest-value* work,
and the sharpest, cheapest signal is already in the engine:

> **Deadline-aware shedding (018).** A message whose deadline has already passed —
> or cannot be met given the remaining transit budget — is *doomed work*: running
> it produces a result no one can use. It is shed **first**, at admission, before
> it consumes a single cycle. This is the cheapest possible shed and it targets
> exactly the work that overload has already invalidated.

After doomed work, shedding follows `Priority<P>` (002) then age (006's
`DropOldest`) — a coherent order, not an ad-hoc drop.

### 3. Circuit breaking — bounding the downstream pileup

An `ask` to a slow or failing target is the classic self-inflicted DoS: retries
and timeouts pile up in-flight, each pinning a reply channel and memory, until the
*caller* falls over from a fault in the *callee*.

```cpp
// per (caller, logical target) — Closed → Open (fail fast) → Half-Open (probe)
struct CircuitBreaker { Admit on_send(); void on_result(bool ok); };
```

- After a failure/timeout threshold the circuit **opens**: further `ask`s fail
  **immediately** with `circuit_open` (007) instead of waiting out a deadline,
  capping in-flight work. A periodic **half-open probe** re-closes it on recovery.
- **Composes with SWIM (010):** a per-remote-node breaker rides the failure
  detector — a node SWIM already suspects short-circuits without waiting for
  per-`ask` timeouts. Node-level and target-level breakers stack.

### 4. Fair sharing — no single source starves the rest

Bounds alone don't prevent one hot key or one principal from consuming an entire
shard's worker time within the bound. Governance adds **weighted fair-share
admission** (002) keyed by a configurable dimension — per-principal (020) at
ingress, per-actor-type internally — so a misbehaving-but-authorized source
degrades *itself*, not its neighbors. Per-shard local accounting keeps this
contention-free (like 009's per-shard counters), reconciled approximately across
shards rather than through a hot global atomic.

### 5. Fan-out amplification governance — broadcast owns its own caps

One `Topic<M>` publish (ADR-019) fans a single message to N subscribers — the
sharpest internal-amplification source in the engine. Crucially, **broadcast
governs itself: there is no shipped concurrent-outstanding-broadcast cap today**,
and the fan-out path does **not** ride the ingress token-buckets above (those gate
external → actor arrival, not internal fan-out). The caps here are new and
broadcast-owned:

- **Per-`(topic, subscriber)` outstanding-broadcast counter.** A relaxed
  `fetch_add` on the broadcast leg, `fetch_sub` in the broadcast **reclaim** branch
  — it touches no ordinary-tell code. A subscriber over its outstanding bound is
  dropped (counted), not blocked. A **soft / approximate** bound is acceptable
  here: best-effort delivery tolerates a slightly-late or slightly-loose count, and
  the relaxed ordering keeps the publish leg off any cross-core RMW.
- **Per-shard live-payload cap `L`.** Bounds the resident `SharedPayload<M>` cells
  a shard holds in flight. On reaching `L`, publish **sheds — drops-all, counted**
  (the whole publish for that shard is dropped) rather than allocate past `L`. This
  is the drops-all-vs-allocate trade the bound-every-resource invariant demands,
  applied to the fan-out lane.

These two caps are **load-bearing, not optional** — ADR-019's firing control shows
linear footprint growth with them disabled, and there is no 022 fallback cap to
inherit. Both are best-effort *shedding* (drop-on-full, counted), the opposite of
024's stream credit *stall*: shedding a broadcast is correct precisely because it
is `AtMostOnce` (017), where stalling would violate GATE 1.

Cross-node amplification is governed not by subscriber count but by the number of
**distinct subscriber nodes** — one coalesced frame per node — bounded by 026's
`relay_cap = ⌈log₂N⌉` when routed through the relay tree.

## Self-debate

- **Central limiter vs. per-shard local?** A single global token bucket is a
  cross-core atomic on the hottest path — the very contention 002/009 designed
  away. **Decision: per-shard local buckets holding a share of the global rate**,
  reconciled periodically. Global enforcement becomes *approximate*, which is the
  right trade: overload protection needs to be *cheap and roughly right*, not
  exact. Exact global limits are an optional coordinator adapter (below).
- **Static limits vs. adaptive?** Static configured limits are predictable and easy
  to reason about but need hand-tuning and can be wrong under changing conditions.
  Adaptive limits (latency-gradient, à la TCP Vegas / concurrency-limit
  controllers) self-tune but are harder to reason about and can oscillate.
  **Decision: static is the default; adaptive concurrency limiting is an opt-in
  policy** that adjusts `MaxConcurrency<N>` (015) from observed latency. Don't
  mandate cleverness.
- **Reject vs. degrade?** Some systems return a cheaper/cached response under load
  instead of rejecting. That is a *handler* concern (the actor can inspect an
  overload hint in its `MessageContext` and choose to degrade); the *engine's* job
  is the honest default — a typed rejection — not to guess a degraded response.
- **Is governance policy (compile-time) or config (runtime)?** Split, consistent
  with 013: **participation** is a policy tag (an actor type declaring it is
  `Governed` / has a `MaxConcurrency<N>`), the **numbers** (rates, budgets, breaker
  thresholds) are operational config — tunable per environment without recompiling,
  never able to weaken a safety invariant.
- **Encrypt/authorize before or after rate-limiting?** Rate-limit *first* for
  unauthenticated ingress (cheap shed beats expensive crypto on a flood), but the
  *fairness* key is the authenticated principal (020) — so the coarse volumetric
  gate is pre-auth and the fair-share gate is post-auth. Two gates, two positions.

## Non-goals

- **Network/volumetric DoS.** L3/L4 floods, amplification, bandwidth exhaustion —
  kernel, firewall, load balancer, scrubbing. The engine governs resources on
  traffic that arrived, not the arrival itself.
- **Exact global distributed rate limiting.** Cross-node exact counts need a
  coordinator (Redis/etcd token store); offered as a `RateLimiter` adapter, never
  the default. The default is per-node approximate.
- **Adding capacity in response to load.** Autoscaling is 021's external control
  loop reacting to 009 metrics; governance keeps a *fixed-capacity* node alive and
  fair, it does not grow the cluster.
- **Metering / quotas / billing.** Governance protects; it does not account. It
  *emits* the counters (009) a chargeback layer could consume, but chargeback is not
  its job.
- **Hard real-time / QoS latency guarantees.** Best-effort fairness and shedding,
  not deadline-scheduling guarantees.

## Interaction

- **006** — the bounded mailbox + `Overflow<Block|Fail|DropOldest>` is the
  foundational per-actor lever this spec generalizes; resolves 006's backpressure
  open question by placing it in a whole-engine governance model.
- **018** — deadline-aware shedding drops doomed work first and cheapest; the single
  most valuable shed signal, reused, not reinvented.
- **015** — `MaxConcurrency<N>` is the per-actor concurrency cap; adaptive limiting
  tunes it. Overload shedding at admission composes with the admission gate but is a
  *load* decision, distinct from the *quiescence* seals.
- **002 / 003** — fair-share scheduling and per-shard memory budgets are enforcement
  points; `Priority<P>` orders shedding. Per-shard local accounting keeps it
  contention-free.
- **007** — a shed/rejected/circuit-open message is a typed failure (`overloaded` /
  `circuit_open`), returned as `std::expected` or dead-lettered — never a crash.
- **010 / 021** — per-peer connection + accept-rate caps; the per-remote-node
  circuit breaker composes with SWIM failure detection; the **hand-off bandwidth
  throttle** (021's open question) is a governance policy on state-transfer rate.
- **020** — governance is the availability sibling of security: rate limiting sits
  at the same ingress boundary as authz, keyed by the authenticated principal, but
  the coarse volumetric gate runs *ahead* of authz so a flood is shed before crypto.
- **009** — every shed, throttle, and breaker trip is a counter + event; overload is
  fully observable and drives both adaptive limits and 021's autoscaling loop.
- **023** — the shed-don't-buffer / bounded-queue rules here are what stop 023's
  throughput tiebreak from trading the latency tail away; the two specs' budgets are
  mutually reinforcing (governance protects the tail that performance budgets).
- **013** — rates, budgets, and breaker thresholds are operational config; governance
  *participation* is a policy tag. Config may not weaken a safety invariant (013).
- **008** — Validation checks governance config coherence (bounds > 0, bucket sizes
  sane) under the `std::expected<…, ValidationReport>` model.

## Dependencies

Std only. Token buckets, bounded queues, circuit breakers, and fair-share
accounting are small self-contained algorithms — **no dependency, nothing linked
into a single-node build beyond what's already there** (006's bounds, 018's
deadlines). A distributed exact-limit coordinator (Redis/etcd) is an optional
`RateLimiter` adapter behind the seam, never the default.

## Open questions

- **Fair-share key granularity.** Per-principal, per-actor-type, per-connection, or
  a composite — and whether the default should differ at ingress (principal) vs.
  internally (actor-type).
- **Adaptive limit stability.** If adaptive concurrency limiting ships, which
  controller (gradient / Vegas-style / PID) avoids oscillation under bursty load,
  and how it interacts with the circuit breaker's own feedback loop.
- **Cross-node fairness without a coordinator.** Per-node approximate shares can let
  a principal exceed a global rate by hitting many nodes; is a gossiped share
  estimate (over SWIM, like 010's RTT reuse) worth the complexity vs. accepting
  per-node-only enforcement by default?
- **Retry-storm damping.** Client retries after a shed can themselves become the
  DoS; should the `overloaded` error carry a server-authored backoff/`Retry-After`
  hint, and how does that compose with 017's at-least-once retry machinery?
- **Priority inversion under shedding.** Ensuring low-priority shedding never
  starves a high-priority actor that legitimately depends on a shed low-priority
  one (a governance analogue of the classic scheduling hazard).
