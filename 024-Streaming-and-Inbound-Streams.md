# 024 — Streaming and Inbound Streams

How a **high-rate / large / unbounded stream of frames** reaches an actor without
fanning one discrete mailbox descriptor per frame. This is the gap
[006](006-Messaging-and-Addressing.md) records as "streaming replies" and leaves
unspecced for the **inbound** direction.

> **Design pinned by [ADR-005](decisions/ADR-005-inbound-stream-ingestion-hot-path.md)**
> — the `StreamChannel` credit-ring, the only one of three designs to sweep its
> safety, correctness, and speed claims under executed C++ (GCC 14.2 + Clang 20.1,
> ASan/UBSan/TSan, percentile benchmarks). It **composes beside** the Accepted
> Vyukov mailbox (ADR-002/003/004), which is unchanged.
>
> **Status: Accepted (x86-64).** The named promotion gate is met:
> [ADR-014](decisions/ADR-014-streaming-async-suspend-real-scheduler-gate.md) wired the
> async-suspend/resume seam to the **real 002 multi-threaded scheduler + 015 admission gate**
> (not a model resolver) and proved it under sanitizers on both g++ 14.2 and clang 20.1 —
> at 10⁷ frames: `lost = dup = torn = fifo_violations = 0`, max descriptor membership 1 (no
> double-enqueue/orphan), credit returned only for **completed** frames (0 for parked-not-completed),
> **0** steady-drain heap allocs and **0** cross-core RMW/frame, TSan clean; the three-lane
> transfer is genuinely taken (32282/32282 parked activations went Suspended and off all
> workers). All three mandatory controls fired non-vacuously — single-cursor tears/loses +
> credits parked frames, re-enqueue double-dispatches (dup up to 602016, two executors, wedge),
> and fence-removed loses ~194K. **Scope: x86-64** — exactly-once / no-wedge / 0-alloc / 0-RMW
> proven; the **Hard absolute-latency budget still awaits 023 reference silicon**, and the
> suspend seam's `seq_cst` Dekker close-out is TSO-proven only (ARM64 weak-memory re-gate
> deferred). See *Open questions* for the residual footprint/zero-copy items (inherited from ADR-005).

## The problem: a stream is not N messages

The obvious implementation — turn each frame into a `tell` — breaks three
assumptions of the discrete-message path (all three *measured*, ADR-005):

1. **No flow control.** `tell` is fire-and-forget; the mailbox is intentionally
   **unbounded** (003) with overload handled by **shedding** (022). But a stream must
   never be *shed* mid-flight — dropping frame N corrupts it. The per-item-tell
   baseline under `Overflow<DropOldest>` **dropped 3.9–4.8M frames mid-stream** in
   the benchmark. Shedding is the wrong tool; a stream needs **flow control** — slow
   the producer.
2. **Descriptor pressure.** One descriptor per frame couples stream *rate* to
   descriptor-pool exhaustion.
3. **Per-frame overhead.** The descriptor + scheduling cost a `tell` pays *once per
   message* is paid *per frame* — 12.6×–19.7× more than the streaming path amortizes
   it to.

So a real stream is modelled as a **Resource** (004), not a message.

## StreamChannel

One **pre-allocated, bounded, credit-controlled SPSC ring** per stream, owned by the
activation, sitting **off** the mailbox. The actor is scheduled by **one reusable
`StreamActivationDescriptor`** placed on its ordinary Vyukov mailbox — posted **only
on the ring's empty→nonempty edge**, never once per frame. When the actor runs, its
handler **drains a batch** from the ring in a single activation turn under the
single-executor guarantee, bounded by the drain budget (002).

```
transport (1 producer) ──frames──▶ [ SPSC credit-ring ] ──batch──▶ actor handler (1 consumer)
                          ▲                                  │
                          └────────── credit return ─────────┘
```

### Cursors — credit is derived, not counted

Three **single-writer, 64-bit monotone** cursors, never a shared RMW counter:

| Cursor | Writer | Meaning |
|---|---|---|
| `head` | producer | next slot to fill |
| `disp` | consumer | next frame to dispatch |
| `tail` | consumer | oldest slot still owed credit (credit-return) |

- **Occupancy** = `head − disp` (frames waiting).
- **Credit available to the producer** = `window − (head − tail)` — a *derived*
  quantity from two single-writer cursors. There is **no shared credit counter to
  double-grant or lose** (GATE-5 is race-free *by construction*, not by locking).
- FIFO-per-stream is the monotone order of `head` (C1 proven: 10M frames, 0
  inversions).

### Why `disp` and `tail` are split (async-suspend exactly-once)

If a single consumer cursor served both dispatch and credit-return, a **suspended**
async handler would either advance it (releasing credit for a frame not yet
processed — a lost frame on resume) or block credit entirely. Splitting them:

- On dispatch of frame *k*: `disp` advances to *k+1*; `tail` does **not**.
- On **suspend**: neither cursor advances past the parked frame. The buffered window
  is pinned; no other worker may enter (single-executor, exec-state CAS).
- On **completion** (via the 015 admission gate — *not* a drain re-entry on the
  completing thread): `tail` advances, returning the frame's credit.

This gives **exactly-once** dispatch across suspension (GATE-1/GATE-2 proven:
`concur_violations = 0`, no wedge, no lost/duplicated frame).

### The drain path has zero cross-core RMW

The consumer inner loop is **plain acquire-load + release-store only** — objdump
confirmed, no `lock`/`mfence`/`cmpxchg` on either compiler (023 hard gate). The
readiness re-arm is a **plain release store** by the consumer; **only the producer**
performs the `armed.exchange` that posts the descriptor on the empty→nonempty edge.
This is the load-bearing fix that separates the winner from the runner-up (whose
consumer `exchange` failed the 0-RMW gate at 0.30 RMW/frame).

### Arming, wakeup, and un-stall

- **Wakeup rides the exec-state machine verbatim** (002): posting the descriptor
  drives `Idle → Scheduled`; the worker relinquishes through the **`seq_cst` Dekker
  close-out** (002, ADR-004) — **never keyed on ring emptiness** (ring emptiness, like
  mailbox emptiness, is non-linearizable). A budget-exhausted stream drain yields
  `Running → Scheduled` for fairness, exactly like a mailbox drain.
- **Hysteresis / low-watermark arming** (required): the ring re-arms only once
  occupancy crosses a low-watermark, so the emergent batch is large enough to hit the
  10M goal instead of re-scheduling per frame.
- **Producer un-stall** (a reverse Dekker rendezvous): a credit-starved producer that
  got `try_push == false` is woken on the next credit-return under edge-triggered
  readiness — so a stalled producer never sleeps forever against available credit.

## Backpressure — the credit window *is* the lever (022)

The ring bounds resident memory (`capacity × slot`). A fast producer **stalls via
credit depletion** — lossless, no mid-stream drop — rather than shedding. GATE-3 is
satisfied by construction (proven: occupancy pinned at capacity, **0 drops over 20M
frames at 4× overproduce**). This is the stream's realization of 022's
"bound-every-exhaustible-resource" invariant: the window is the bound, back-pressure
is the producer stall. `credit_limit` (022) may *narrow* the window with a relaxed
store, **never on the per-frame path**.

The stream's advantage over the per-item-tell baseline is **operational** — no shared
source lane, amortized descriptor + scheduling cost — not a correctness verdict
against the mailbox: the mailbox is correct, it is simply the wrong *shape* for a
stream.

## Buffer ownership and zero-copy (003, 004)

- The per-stream **ring + slots + payload arena** are **shard-`pmr`-owned**,
  pre-allocated at **stream-open (cold path)**, freed at close. **Per-frame hot-path
  allocations = 0** (S2 proven: 0 over 50M frames).
- **Two slot regimes:**
  - **Inline** (≤ 56 B frame): the frame lives in the slot; immune to the overwrite
    hazard below. This is the default.
  - **By-reference / registered-RX**: the slot references a transport buffer (the
    transport may DMA directly into the registered arena — true zero-copy). The
    referenced buffer's lifetime is tied to the credit/read-cursor advance.
- **`StreamMode::ZeroCopyRetained`** (opt-in): for by-reference spans that must
  **outlive the drain step**, a strict **in-order-prefix byte-credit** counter
  governs reclamation (adopted from the SPSC-CHANNEL design's proven C4 — reclaiming
  bytes *past a retained hole* over-grants credit and reintroduces a use-after-free).
  Compiled away entirely in the inline/copy default.

## Single-producer precondition

The SPSC guarantee (and therefore GATE-5) holds **only for one writer per cursor**. A
**stream-open single-writer token** enforces it: a second bind is a typed
[007](007-Failure-and-Supervision.md) error plus a debug assertion. **Multi-source
fan-in must stay on the mailbox** — a two-writer control fired a TSan race and 30,282
lost updates in the negative control.

## Developer surface (006)

A stream reaches a handler as a **batch**, not a frame at a time:

```cpp
class Ingest : public Actor<Ingest, Sequential> {
public:
    // discrete control-plane messages, as always
    void handle(const Configure&);
    // inbound stream frames, drained in batches
    void handle(quark::StreamBatch<Sample>& batch);   // 024 dispatch
};

quark::StreamRef<Sample> s = system.open_stream<Sample>(ingest_id, transport_ep);
```

- `StreamRef<F>` is the handle to an open stream; `handle(StreamBatch<F>&)` is the
  drain overload.
- **A stream is a distinct sender from the control-plane `tell`s** to the same actor:
  frames preserve **FIFO per-stream** end-to-end, but a stream frame and a `tell` have
  **no mutual global order** (the transport is a separate sender, 006). The stream
  *descriptor* interleaves FIFO in the same mailbox as ordinary tells; the mailbox hot
  path is unchanged.

## How it composes with the rest of the engine

| Spec | Interaction |
|---|---|
| 001 | Stream draining runs inside one activation under the single-executor invariant; a suspended stream handler does not advance the drain (split `disp`/`tail` freeze at the parked frame). |
| 002 | Stream activation rides the exec-state wakeup + `seq_cst` Dekker close-out **verbatim**; budget-exhausted drain yields for fairness; 015 completion re-enters the drain via a stream-descriptor-aware continuation — the activation is **transferred, not parked**, so the descriptor is not re-enqueued and the window can't be orphaned. |
| 003 | Ring/slots/arena are shard-`pmr`-owned, pre-allocated cold; 0 per-frame alloc; zero-copy where the transport DMAs into the registered arena. |
| 004 | The inbound `Stream<F>` is a `Cached<>` activation resource, wired at metadata-compile time → zero dynamic resource resolution while a frame is processed. |
| 006 | `StreamRef<F>` + `handle(StreamBatch<F>&)` alongside `tell`/`ask`; FIFO-per-stream; distinct-sender rule. |
| 022 | The credit window is the per-stream backpressure lever; producer stall, not shedding. |
| 023 | Streaming budget block (throughput / amortization / latency / 0-alloc / 0-RMW). |
| 001/015 (ADR-015) | A stream handler may itself be a `BlockingHandler`/`FiberHandler` — e.g. a **foreign-C stream decoder** that cannot be colored as a coroutine. If so it follows the **same "transferred, not parked" 015 re-admission** as a suspended stream handler (ADR-014): the split `disp`/`tail` cursors freeze at the parked frame, the un-colorable leaf offloads off the mailbox lane, and the 015 gate re-enters `StreamChannel::drain` on completion. The stackful `quark::fiber<>` form additionally enables the **C4 multiplexing** case — many foreign-frame stream callbacks issuing nested `ask`s multiplexed on one cooperative driver, versus a thread-backed carrier ceiling of `P`. |

## Self-debate

### Off the mailbox, or a mailbox variant?

- **On the mailbox (per-frame descriptors):** uniform, but proven to shed mid-stream
  and to couple rate to pool pressure. Rejected — a stream is the wrong shape for the
  discrete path.
- **Fully separate transport→worker channel with its own scheduling:** more machinery,
  a second wakeup protocol to keep correct, and it duplicates the exec-state
  rendezvous the mailbox already proved. Rejected.
- **Decision:** a ring *off* the mailbox but scheduled *through* the existing
  descriptor + exec-state machine. One readiness descriptor per arm-edge, reusing the
  settled wakeup/close-out verbatim. New buffer, old scheduler.

### Derived credit vs. a credit counter

A shared credit counter needs an atomic RMW per grant/return and invites
double-grant/lost-credit. Deriving credit from two single-writer monotone cursors
removes the shared counter entirely — the correctness property is *structural*, which
is why GATE-5 needed no lock and no proof beyond "there is no shared write."

### Why batches, not a frame-at-a-time callback

A per-frame callback re-pays the dispatch cost the design exists to amortize and
defeats the 10M goal. The handler sees a `StreamBatch<F>` and the drain loop stays
tight (p50 15–18 ns/frame). Hysteresis arming makes the batch emerge naturally under
load and collapse to low latency when idle.

## Non-goals

- **Outbound streaming replies** (an `ask` returning a `std::generator<>`/stream) —
  006 still lists this; this spec is **inbound** only. The credit-ring is reusable for
  it but the reply-routing/one-shot-channel interaction is separate.
- **Multi-source fan-in into one stream** — stays on the mailbox (SPSC precondition).
- **Transport framing / wire format** — 010/016 own the bytes; 024 owns what happens
  once frames are in the ring.
- **Autoscaling stream buffers** — `credit_limit` narrows a fixed window; growing the
  window is a policy question (open below).

## Open questions

- **Async-suspend resume against the real scheduler.** The split-cursor no-wedge
  exactly-once property is proven with an in-thread model resolver + a fired wedge
  control, but the 015 admission-gate continuation re-entering `StreamChannel::drain`
  on the actor's lane **without re-enqueuing the descriptor** was *modelled, not wired*
  to the real 002 scheduler. This seam needs an ADR-004-grade executed run before 024
  leaves Draft. **This is the promotion gate.**
- **Idle-density footprint.** 64 B slots × cap-256 = 16 KB/stream → ~60K streams/GB,
  ~17× under 023's 1M-activations/GB target. Real for many-small-streams workloads.
  Mitigations (smaller cap, shared slab, packed slots) each trade against near-empty
  false sharing and must be `perf c2c`-measured before adoption.
- **Reference-silicon latency.** Absolute ns are from a conservative ~2.1 GHz
  turbo-off rig; ratios, 0-RMW and 0-alloc gates are host-independent and pass, but
  absolute latency percentiles must re-run on the 023 Zen4/SPR core before the 024
  Hard-latency budget is stamped.
- **Weak-memory (AArch64) — INCONCLUSIVE.** The `armed.exchange`-as-Dekker-arm relies
  on x86 `lock xchg` being a full StoreLoad barrier; on ARM it needs an explicit
  `seq_cst` fence or `seq_cst` exchange. Deferred with the rest of ARM (no hardware);
  herd7/GenMC litmus-checked before any ARM claim (OpenQuestions).
- **Zero-copy suspend head-of-line blocking** (only under `ZeroCopyRetained`): a
  suspended handler pins its referenced RX buffer, so on a shared provided-buffer pool
  one slow consumer can starve unrelated streams. 024 must cap per-stream pinned
  buffers or force copy-out on suspend.
- **Adaptive `credit_limit` stability.** Adaptive narrowing (022) interacting with the
  producer stall loop can oscillate; needs a stability test before adaptive
  backpressure ships.
