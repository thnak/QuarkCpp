// Marketing copy for the landing page. Every figure here is transcribed from the
// repository's own records — README.md, PERFORMANCE.md, VERIFICATION.md and
// bench/caf_comparison/README.md — so the site never claims more than the repo proves.

export const REPO_URL = 'https://github.com/thnak/QuarkCpp'

export interface Stat {
  value: number
  label: string
  suffix: string
  note: string
}

export const STATS: Stat[] = [
  { value: 194, label: 'correctness tests', suffix: '', note: 'ASan / UBSan / TSan clean' },
  { value: 86, label: 'design documents', suffix: '', note: '28 RFC specs + ADRs + reference' },
  { value: 59, label: 'mailbox p50', suffix: ' ns', note: 'enqueue → dequeue, budget ≤ 100 ns' },
  { value: 11.0, label: 'messages / second', suffix: ' M', note: 'full tell lifecycle, one core' },
]

export interface Feature {
  title: string
  icon: string
  body: string
  tag: string
}

export const FEATURES: Feature[] = [
  {
    title: 'Header-first, std-only C++23',
    icon: 'header',
    body: '`std::expected` results, coroutine handlers, `std::stop_token` cancellation, `std::pmr` shard allocators, concepts and deducing-this. No RTTI or reflection on the hot path.',
    tag: 'core',
  },
  {
    title: 'Hybrid handler execution',
    icon: 'split',
    body: 'Synchronous by default and drained inline at zero cost. An actor opts into `quark::task<>` coroutine handlers per message type when it needs async I/O.',
    tag: '001 / 015',
  },
  {
    title: 'Zero-cost intent declaration',
    icon: 'policy',
    body: 'CRTP policy types — `Sequential`, `Priority<P>`, `Placement<…>`, `DrainBudget<N>` — are template parameters resolved to metadata at startup. No attributes, no runtime config.',
    tag: '005 / 013',
  },
  {
    title: 'Work-stealing scheduler',
    icon: 'sched',
    body: 'Workers borrow activations from shards across K priority bands. Per-actor mailbox FIFO is inviolable, and `UniformFIFO` objdumps byte-identical to a single MPSC.',
    tag: '002',
  },
  {
    title: 'Point-to-point and fan-out messaging',
    icon: 'fanout',
    body: '`tell` / `ask`, credit-controlled streaming replies (`ask_stream`), best-effort at-most-once broadcast (`Topic<M>`), and ordered reliable N-subscriber fan-out (`FanOut<M, Policy>`).',
    tag: '006 / 017',
  },
  {
    title: 'Inbound stream ingestion',
    icon: 'stream',
    body: 'A pre-allocated per-stream SPSC credit-ring with derived credit — no shared counter, zero-copy, and backpressure instead of shedding. 140.8 M frames/s sustained.',
    tag: '024',
  },
  {
    title: 'Cluster distribution at scale',
    icon: 'cluster',
    body: 'HRW / VirtualBins O(1) placement, SWIM membership, bounded partial-view plus DHT-relay for 10³–10⁴-node topologies, IPv4 and IPv6 endpoints.',
    tag: '010 / 026',
  },
  {
    title: 'Durable persistence and reminders',
    icon: 'store',
    body: 'Snapshot and event-sourced durability, plus at-least-once wall-clock wake-ups that flatten a 10⁶-at-9 PM mass-due wave to `peak == fire_rate`.',
    tag: '012 / 027',
  },
  {
    title: 'Failure supervision',
    icon: 'shield',
    body: 'A zero-cost guarded handler core with restart / resume / stop / escalate policies, escalation-storm guards, and a poison `ask` that returns an error instead of hanging.',
    tag: '007',
  },
  {
    title: 'Resource governance',
    icon: 'gauge',
    body: 'Per-node token-bucket rate limiting, deadline-aware load shedding, circuit breaking and bounded queues — with cross-node backpressure signalling on the transport.',
    tag: '022',
  },
  {
    title: 'Node security',
    icon: 'lock',
    body: 'mTLS node-to-node transport with live certificate rotation and revocation enforced against already-open sessions; wire-arrived principals propagate into handler context.',
    tag: '020',
  },
  {
    title: 'Deterministic simulation testing',
    icon: 'sim',
    body: 'Fault injection without real time or threads. BMC-DPOR interleaving exploration finds a planted bug 100/100 where random search hits 66–72%.',
    tag: '014',
  },
]

export interface ProofStep {
  step: string
  body: string
}

export const PROOF_STEPS: ProofStep[] = [
  {
    step: 'Design',
    body: 'An architect proposes a concrete design against the locked RFC decisions and states falsifiable fast / safe / correct claims — each paired with an experiment that would disprove it.',
  },
  {
    step: 'Red-team',
    body: 'An adversary steelmans the design, then attacks it hardest where it claims to be fast or safe: data races, UB, ABA, torn state, lost messages, hidden allocations.',
  },
  {
    step: 'Prove',
    body: 'Every design is implemented in real C++23, compiled under GCC and Clang, run under ASan/UBSan/TSan and benchmarked. Claims come back CORRECT, WRONG or INCONCLUSIVE.',
  },
  {
    step: 'Judge',
    body: 'A judge weighs which claims survived red-teaming AND were proven by executed code, picks a winner, and writes the durable ADR. Evidence outranks elegance.',
  },
]

export interface PerfRow {
  feature: string
  spec: string
  metric: string
  measured: string
  budget: string
  ratio: number
  verdict: 'goal' | 'free'
}

export const PERF_ROWS: PerfRow[] = [
  { feature: 'tell — mailbox', spec: '003', metric: 'enqueue → dequeue p50', measured: '59 ns', budget: '≤ 100 ns', ratio: 0.59, verdict: 'goal' },
  { feature: 'tell — scheduler', spec: '002', metric: 'full-lifecycle throughput', measured: '11.0 M/s', budget: '≥ 10 M/s', ratio: 0.91, verdict: 'goal' },
  { feature: 'priority', spec: '002', metric: 'UniformFIFO vs raw MPSC', measured: '+0.45 ns', budget: 'within noise', ratio: 0.05, verdict: 'free' },
  { feature: 'ask', spec: '006', metric: 'engine-overhead p50 / p99', measured: '147 / 226 ns', budget: 'p50 ≤ 1 µs', ratio: 0.15, verdict: 'goal' },
  { feature: 'streaming', spec: '024', metric: 'sustained ingest / per-frame', measured: '140.8 M/s · 7.1 ns', budget: '≥ 10 M/s · ≤ 100 ns', ratio: 0.07, verdict: 'goal' },
  { feature: 'streaming', spec: '024', metric: 'ingest vs discrete tell', measured: '5.0× cheaper', budget: '≥ 3×', ratio: 0.6, verdict: 'goal' },
  { feature: 'activate / deactivate', spec: '001', metric: 'cold activation p50', measured: '111 ns', budget: '≤ 10 µs', ratio: 0.011, verdict: 'goal' },
  { feature: 'idle density', spec: '003', metric: 'activations per GB', measured: '1.95 M/GB', budget: '≥ 1 M/GB', ratio: 0.51, verdict: 'goal' },
  { feature: 'serialize', spec: '016', metric: 'tagless wire encode p99', measured: '50 ns', budget: '≤ 200 ns', ratio: 0.25, verdict: 'goal' },
  { feature: 'placement', spec: '010/026', metric: 'VirtualBins lookup, N-independent', measured: '12.5 ns', budget: '≤ 20 ns', ratio: 0.62, verdict: 'goal' },
  { feature: 'supervision', spec: '007', metric: 'guarded vs unguarded success path', measured: '~1.0×', budget: '≤ noise', ratio: 0.05, verdict: 'free' },
]

export const INVARIANT_GATES: string[] = [
  'descriptor ≤ 64 B',
  '0 hot-path allocations',
  '0 cross-core RMW on the drain path',
  'objdump zero-cost parity',
]

export interface ComparisonRow {
  dimension: string
  winner: 'Quark' | 'CAF'
  margin: string
}

export const COMPARISON_ROWS: ComparisonRow[] = [
  { dimension: 'ask latency (p50, 1 & 12 workers)', winner: 'Quark', margin: '3.3× / ~11×' },
  { dimension: 'Spawn (10k actors, 1 & 12 workers)', winner: 'Quark', margin: '1.55× / 5.0×' },
  { dimension: 'Single-threaded throughput', winner: 'CAF', margin: '~1.04×' },
  { dimension: 'Shared-mailbox MPSC scaling (2–12 producers)', winner: 'CAF', margin: '1.24×–1.76×' },
  { dimension: 'Tail latency under thread oversubscription', winner: 'CAF', margin: 'gap widens past core count' },
]

export const CHOOSE_QUARK: string[] = [
  'Predictable, allocation-free, low-latency single-actor `ask` / `tell` as the primary workload',
  'Compile-time-typed actors — one fixed protocol per actor, checked by the compiler, zero RTTI',
  'Every hot-path and safety claim backed by an executed proof, not an assertion',
  'Deterministic simulation testing for fault injection without real time or threads',
]

export const CHOOSE_CAF: string[] = [
  'High aggregate throughput under many concurrent producers and contended mailboxes',
  'Runtime-flexible messaging — one actor handling many unrelated shapes, `become` / `unbecome`',
  'A large existing ecosystem, production track record and battle-tested maturity',
  'A mature reactive-streaming toolkit and broad, already-shipped platform support',
]

export interface PostureRow {
  subsystem: string
  spec: string
  std: string
  adapter: string
}

export const POSTURE_ROWS: PostureRow[] = [
  { subsystem: 'Transport', spec: '010', std: 'TCP + length-prefixed frames; epoll / io_uring · kqueue · IOCP via the PAL', adapter: 'gRPC / QUIC / RDMA' },
  { subsystem: 'Serialization', spec: '016', std: 'canonical tagged TLV + negotiated tagless fast path', adapter: 'protobuf / FlatBuffers / Cap’n Proto' },
  { subsystem: 'Membership', spec: '010', std: 'in-house SWIM gossip', adapter: 'etcd / Consul' },
  { subsystem: 'Persistence', spec: '012', std: 'InMemoryStore + FileStore (append-only WAL, fdatasync)', adapter: 'SqliteStore / RocksStore / Postgres' },
  { subsystem: 'Metrics & trace', spec: '009', std: 'snapshot API + Prometheus text', adapter: 'OpenTelemetry / OTLP' },
  { subsystem: 'Governance', spec: '022', std: 'per-node token buckets, bounded queues, circuit breakers', adapter: 'distributed exact-limit coordinator' },
  { subsystem: 'Large-scale topology', spec: '026', std: 'VirtualBins + bounded partial-view + Kademlia relay, coordinator-free', adapter: 'external coordinator behind the Membership seam' },
]

export interface CodeTab {
  id: string
  label: string
  caption: string
  source: string
  code: string
}

export const CODE_TABS: CodeTab[] = [
  {
    id: 'counter',
    label: 'Hello counter',
    caption: 'One actor, driven by tell (fire-and-forget) and ask (request/reply) over the real engine.',
    source: 'samples/01_hello_counter',
    code: `#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

struct Add { int amount; };
struct GetTotal {};

// Policies in the CRTP base ARE the actor's metadata (band, budget, reentrancy).
struct Counter : Actor<Counter, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<Add, Ask<GetTotal, int>>;

    void handle(const Add& a) noexcept { total_ += a.amount; }
    void handle(const Ask<GetTotal, int>& m) noexcept { m.respond(total_); }

private:
    int total_ = 0;
};

int main() {
    detail::MessagePool pool(1024);
    Counter counter;
    auto activation = std::make_unique<Activation>(&counter, Counter::dispatch_table(), pool.sink());

    Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    register_actor<Counter>(eng, /*key*/ 42, *activation);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Counter> counter_ref = router.get<Counter>(42);
    eng.start();

    for (int i = 1; i <= 100; ++i) counter_ref.tell(Add{i});         // fire-and-forget
    result<int> total = block_on(counter_ref.ask<int>(GetTotal{}));  // request/reply

    eng.stop();
}`,
  },
  {
    id: 'supervision',
    label: 'Fault containment',
    caption:
      'A throwing handler is contained at the boundary. The lane survives, and a poison ask resolves as an error rather than hanging the caller.',
    source: 'samples/02_supervised_worker',
    code: `struct Work   { int n; bool boom; };
struct Square { int n; bool boom; };

// Default supervision == Restart(assert-intact): the guard contains the throw
// and the actor keeps serving. No policy is spelled out — this is out of the box.
struct Worker : Actor<Worker, Sequential> {
    using protocol = Protocol<Work, Ask<Square, int>>;

    void handle(const Work& w) {
        if (w.boom) throw std::runtime_error("poison tell");  // contained at the boundary
        processed_ += w.n;
    }
    void handle(const Ask<Square, int>& m) {
        if (m.query.boom) throw std::runtime_error("poison ask");  // faults BEFORE respond()
        m.respond(m.query.n * m.query.n);
    }

private:
    int processed_ = 0;
};

// 1) A poison tell throws inside the handler. It is contained — no crash.
ref.tell(Work{/*n*/ 5, /*boom*/ true});

// 2) A GOOD ask still works right after the fault: the lane survived.
result<int> good = block_on(ref.ask<int>(Square{/*n*/ 7, /*boom*/ false}));`,
  },
  {
    id: 'reminders',
    label: 'Durable reminders',
    caption:
      'Wall-clock, persisted, at-least-once wake-ups. 10,000 reminders due at one instant flatten to a peak of fire_rate per second — never a 10,000-wide spike.',
    source: 'samples/14_durable_reminders',
    code: `#include "quark/core/reminder_service.hpp"

using namespace quark;

InMemoryReminderStore store;

// The fire callback models the tell that lands on the actor's own lane (011 delivery).
// In a real engine this reactivates a passivated actor and delivers the payload.
auto deliver = [](const FireEvent& e) {
    std::printf("FIRE actor=%llu \\"%.*s\\" scheduled@%llds\\n",
                static_cast<unsigned long long>(e.actor.key),
                static_cast<int>(e.name.size()), e.name.data(),
                static_cast<long long>(e.scheduled_due_ns / kSec));
};

ReminderConfig cfg;  // fire_rate 0 => fire everything due immediately (low volume)
ReminderService<InMemoryReminderStore> svc(store, deliver, cfg);
svc.open();

// Deterministic: driven by tick(WallInstant), no wall-clock sleeps.
// In production you call svc.tick(quark::wall_now()) from the owner node's loop.
svc.tick(at(21 * 3600));`,
  },
]

export const QUICKSTART = `# Build + run the full correctness gate (Release)
cmake -S . -B build
cmake --build build -j4                          # -j4, never -j$(nproc)
ctest --test-dir build -j4 --output-on-failure   # 194 / 194

# Sanitizers (the same suite, minus by-design exclusions)
cmake -S . -B build-asan -DQUARK_SANITIZE="address;undefined"
cmake -S . -B build-tsan -DQUARK_SANITIZE="thread"   # build -j1

# Benchmarks (default ON) and the runnable samples (default OFF)
cmake -S . -B build -DQUARK_BUILD_SAMPLES=ON
taskset -c 0-3 build/samples/01_hello_counter    # prints OK / exit 0`

export const MAILBOX_ROUNDS = 11
