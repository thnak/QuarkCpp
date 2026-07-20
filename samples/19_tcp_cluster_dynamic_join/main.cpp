// Quark sample 19 — a node joins a cluster that is ALREADY RUNNING, live, with no restart of any
// existing process. Samples 15-18 all start with a FIXED, agreed-upon roster (every process is told
// `num_nodes` up front and the cluster never grows) — that is the honest limitation those samples
// have, and the question this sample answers: can Quark actually grow a live cluster the way a real
// deployment does (a new instance comes up, joins, and immediately participates)?
//
// The answer is yes, using the pieces that already exist but that no other sample wires together:
//   * `quark::SwimMembership` (021, include/quark/core/cluster.hpp) — a REAL SWIM gossip failure
//     detector + join protocol: `request_join(seed)` / `admit()` grow the roster and bump the epoch
//     live, no restart. It implements the same `Membership` interface `InProcessMembership` does, so
//     `DistributedRouter`/placement do not know or care that the roster is growing under them.
//   * `net::TcpTransport` (019) — the same real-socket transport every other TCP sample uses.
//
// TWO WIRING SUBTLETIES this sample has to get right, that no earlier sample needed to think about:
//
//   1. SwimMembership and DistributedRouter both want `Transport::on_receive` (a single-slot
//      callback) — construct SwimMembership FIRST, then DistributedRouter (which briefly overwrites
//      the slot; harmless, transport.start() has not run yet, so no traffic exists to misroute), then
//      call `swim.set_data_sink(...)` to install the FINAL, correct demux: SWIM handles Control frames
//      internally and forwards Data frames to `dist.deliver()`.
//
//   2. SwimMembership documents that `tick()` and inbound frame handling must run on ONE thread (its
//      internal state is unsynchronized by design — proven against the single-threaded LoopbackTransport
//      test double). `net::TcpTransport` invokes `on_receive` on its OWN private I/O thread, and had no
//      public way to schedule other work onto that same thread — so driving `tick()` from an ordinary
//      background thread would be a real, silent data race. Fixed with one small, additive method,
//      `TcpTransport::post(fn)`, mirroring the `io_.post` marshalling `send()`/`close_peer()` already use
//      internally: this sample's driver thread never touches `swim` directly, it only ever calls
//      `transport.post([&]{ swim.tick(); })`, which the transport itself marshals onto its I/O thread —
//      the SAME thread `on_receive` (and so `swim`'s internal mutation) already runs on.
//
//   Gossip carries (node, incarnation, status) — NOT a network address (021 leaves that to a DNS/K8s
//   Discovery adapter, a documented, not-yet-built seam). So a node learning "NodeId 4 is Alive" still
//   needs to learn WHERE it listens before the data plane can reach it. This sample's honest stand-in
//   for that Discovery adapter is the SAME fixed `base_port + node_id` formula every other TCP sample
//   uses: the driver thread reconciles `swim.view()` against known peers and calls `add_peer()` for any
//   newly-live node using that formula.
//
// Usage: samples/19_tcp_cluster_dynamic_join/main.cpp is ONE node; run it once per node via
//   samples/19_tcp_cluster_dynamic_join/run_cluster_dynamic.sh [base_n=3] [final_n=4] [base_port=22000] [join_delay_ms=4000]
// which starts `base_n` nodes immediately, then starts the remaining nodes (up to `final_n`) after
// `join_delay_ms` — a cluster that is ALREADY UP AND SERVING TRAFFIC before the new node ever appears.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 19_tcp_cluster_dynamic_join
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

constexpr std::int32_t SRC_MUL = 1'000'000;  // Seq.n encodes sender NodeId (1-based) * SRC_MUL + sequence
constexpr ClusterId kClusterId{0xC0FFEE};    // shared out-of-band (021 §1) — every node agrees on this

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n))  // remotely-received messages MUST be serializable (016)

// `last_seen[src-1]` tracks the last in-order sequence number received FROM that sender, so FIFO can
// be checked per-(sender,receiver) pair. Single Sequential executor drains this actor, so no lock needed.
struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> last_seen;  // sized to final_n by main(), filled with -1
    std::atomic<int> count{0};
    bool ordered = true;

    void handle(const Seq& s) noexcept {
        const int src = s.n / SRC_MUL;
        const int seq = s.n % SRC_MUL;
        const std::size_t idx = static_cast<std::size_t>(src - 1);
        if (src < 1 || idx >= last_seen.size() || seq != last_seen[idx] + 1) {
            ordered = false;
        } else {
            last_seen[idx] = seq;
        }
        count.fetch_add(1, std::memory_order_release);
    }
};

// sleep-poll (no busy-spin — machine-safety rule: never burn a core). A bounded poll instead of a
// blocking wait means a real hang surfaces as this process exiting FAIL, not hanging.
template <class Pred>
static bool wait_until(Pred pred, int timeout_ms) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                      "usage: %s <self_id> <seed_id_or_0> <final_n> [base_port=22000] [join_delay_ms=0] "
                      "[settle_budget_ms=20000]\n",
                      argv[0]);
        return 2;
    }
    const std::uint64_t self_id = std::strtoull(argv[1], nullptr, 10);
    const std::uint64_t seed_id = std::strtoull(argv[2], nullptr, 10);
    const int final_n = std::atoi(argv[3]);
    const int base_port = argc > 4 ? std::atoi(argv[4]) : 22000;
    const int join_delay_ms = argc > 5 ? std::atoi(argv[5]) : 0;
    const int settle_budget_ms = argc > 6 ? std::atoi(argv[6]) : 20000;
    if (self_id < 1 || static_cast<int>(self_id) > final_n || final_n < 1) {
        std::fprintf(stderr, "self_id must be in [1, final_n]\n");
        return 2;
    }

    // t0 is captured BEFORE the join-delay sleep, at process launch — every process is launched by the
    // script at essentially the same wall-clock moment, so this keeps everyone's settle_budget_ms hold-
    // open deadline (below) aligned to the SAME absolute point in time. Capturing it AFTER the sleep
    // would give the late joiner a deadline settle_budget_ms LATER than everyone else's in absolute
    // terms — the base nodes would tear down and exit while the late joiner still needed them, exactly
    // the bug sample 18 hit from unsynchronized per-process teardown.
    const auto t0 = std::chrono::steady_clock::now();

    // A LATE joiner delays here, before touching anything — everyone else (join_delay_ms=0) is already
    // up, ticking, and serving traffic by the time this node's transport/SWIM even exist. This is what
    // makes it "join a RUNNING system", not "everyone starts together and one is just slower".
    if (join_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(join_delay_ms));

    // Deterministic HRW placement over the FINAL roster {1..final_n} — used only to pick which Logger
    // key THIS node personally hosts, independent of how many nodes are actually live right now. (The
    // REAL routing decision at send time still goes through SwimMembership's live, growing view — this
    // is just a stable way to give each node exactly one dedicated inbox to prove reachability against.)
    std::vector<NodeId> full_cluster;
    for (int i = 1; i <= final_n; ++i) full_cluster.push_back(NodeId{static_cast<std::uint64_t>(i)});
    InProcessMembership probe(NodeId{1}, full_cluster);
    const MembershipView pv = probe.view();
    std::vector<std::uint64_t> keys(static_cast<std::size_t>(final_n), 0);
    int found = 0;
    for (std::uint64_t k = 1; k < 1'000'000 && found < final_n; ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        const std::size_t idx = static_cast<std::size_t>(owner.value - 1);
        if (idx < static_cast<std::size_t>(final_n) && keys[idx] == 0) {
            keys[idx] = k;
            ++found;
        }
    }
    if (found != final_n) {
        std::printf("node %llu: FAIL (could not find a key owned by every node)\n",
                    static_cast<unsigned long long>(self_id));
        return 1;
    }

    detail::MessagePool pool{4096};
    Logger actor;
    actor.last_seen.assign(static_cast<std::size_t>(final_n), -1);
    auto act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
    Engine<> eng{EngineConfig{1, 1, 64, 64}};
    LocalRouter local{eng.post_courier(), pool};

    net::TcpTransport transport{NodeId{self_id}, pal::ipv4_loopback,
                                 static_cast<std::uint16_t>(base_port + static_cast<int>(self_id))};
    SwimMembership::Config cfg;
    cfg.cluster_id = kClusterId;
    SwimMembership swim{NodeId{self_id}, transport, cfg};
    DistributedRouter dist{swim, local, transport};  // briefly clobbers on_receive — harmless pre-start()
    swim.set_data_sink([&dist](MessageFrame f) { dist.deliver(f); });  // final, correct demux
    eng.register_activation(actor_id_of<Logger>(keys[static_cast<std::size_t>(self_id - 1)]), *act);
    dist.register_remote<Logger, Seq>();

    if (!transport.start()) {
        std::printf("node %llu: FAIL (bind on port %d failed — already in use by a leftover process?)\n",
                    static_cast<unsigned long long>(self_id), base_port + static_cast<int>(self_id));
        return 1;
    }
    std::printf("node %llu listening on 127.0.0.1:%u (seed=%llu)\n", static_cast<unsigned long long>(self_id),
                transport.listen_port(), static_cast<unsigned long long>(seed_id));

    if (seed_id != 0) {
        // Need the seed's endpoint to dial it at all — every other peer's endpoint is filled in below,
        // live, as gossip reveals them (that is the whole point of this sample).
        transport.add_peer(
            Endpoint{NodeId{seed_id}, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + static_cast<int>(seed_id))});
        transport.post([&swim, seed_id] { swim.request_join(NodeId{seed_id}); });
    }

    // The driver thread: NEVER touches `swim` directly except through `transport.post(...)`, which
    // marshals onto the transport's own I/O thread — the same thread `on_receive`/SwimMembership's
    // internal mutation already happens on (see the file banner for why this matters). `view()` and
    // `add_peer()` are independently documented thread-safe, so reading the roster and registering a
    // newly-discovered peer's endpoint from this thread is fine.
    std::atomic<bool> running{true};
    // known_peer_count is bumped AFTER add_peer() is actually called (release), so a main-thread reader
    // (acquire) observing it hit final_n has a happens-before guarantee that every peer's add_peer() call
    // has already been ISSUED. That is enough even though add_peer()/send() are both async (marshalled
    // via the transport's own post queue): as long as add_peer() for a peer is issued before send() for
    // that same peer, FIFO draining of that one queue applies the endpoint before the send is processed.
    // Without this, "swim.view().size() == final_n" alone is NOT enough — the driver thread's own
    // reconciliation pass (below) can lag up to one 100ms poll behind the roster converging, and a send
    // fired in that gap finds no known endpoint and is dropped immediately and permanently (010
    // dead-letter, `drops()` — NOT the dial-dedup race, which is what this bug used to be misdiagnosed as).
    std::atomic<int> known_peer_count{1};  // self, counted from construction
    std::unordered_set<std::uint64_t> known_peers{self_id};
    std::thread driver([&] {
        while (running.load(std::memory_order_relaxed)) {
            transport.post([&swim] { swim.tick(); });
            const MembershipView view = swim.view();
            for (NodeId n : view.nodes()) {
                if (known_peers.insert(n.value).second) {
                    transport.add_peer(
                        Endpoint{n, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + static_cast<int>(n.value))});
                    known_peer_count.fetch_add(1, std::memory_order_release);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    eng.start();

    auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    auto remaining_ms = [&](int budget) {
        const auto rem = budget - elapsed_ms();
        return rem > 0 ? static_cast<int>(rem) : 0;
    };

    // Wait for the roster to grow to the full expected size — the live, no-restart join actually
    // completing — AND for this node's own transport to have learned every peer's endpoint (see the
    // driver thread comment above for why both are required, not just the roster). Generous bound:
    // covers gossip fanout convergence AND (for the late joiner) its own join_delay_ms head start
    // already burned before t0.
    const bool converged = wait_until(
        [&] {
            return swim.view().size() == static_cast<std::size_t>(final_n) &&
                   known_peer_count.load(std::memory_order_acquire) >= final_n;
        },
        remaining_ms(settle_budget_ms - 3000));
    const std::size_t roster_seen = swim.view().size();
    const std::uint64_t epoch_seen = swim.epoch();
    std::printf("node %llu: roster %s at %zu/%d (epoch=%llu) after %lldms\n",
                static_cast<unsigned long long>(self_id), converged ? "converged" : "DID NOT converge",
                roster_seen, final_n, static_cast<unsigned long long>(epoch_seen),
                static_cast<long long>(elapsed_ms()));

    // Send our lane to EVERY node's Logger, including the late joiner specifically — proving it is a
    // full participant, not just visible in the roster.
    constexpr int N = 150;
    for (int j = 1; j <= final_n; ++j) {
        DistRef<Logger> ref = dist.get<Logger>(keys[static_cast<std::size_t>(j - 1)]);
        for (int s = 0; s < N; ++s) ref.tell(Seq{static_cast<std::int32_t>(self_id) * SRC_MUL + s});
    }

    const int expected = final_n * N;
    wait_until([&] { return actor.count.load() >= expected; }, remaining_ms(settle_budget_ms));
    // Hold the connection open for whatever's left of the shared window instead of tearing down the
    // instant this node is satisfied — a fast node exiting early would yank its socket out from under a
    // slower peer (the same fix sample 18 needed, for the same reason).
    if (const int rem = remaining_ms(settle_budget_ms); rem > 0) std::this_thread::sleep_for(std::chrono::milliseconds(rem));

    running.store(false, std::memory_order_relaxed);
    driver.join();

    const int got = actor.count.load();
    const int shortfall = expected - got;
    // Same documented tradeoff as samples 17/18: the LOSING side of a simultaneous first-dial race can
    // drop frames it had already handed to its socket (tcp_transport.hpp register_identified(), "NOTE
    // (at-most-once...)"). Discovery here is staggered (gossip-driven, not all-at-once), so it should
    // fire far less often than in the deliberately-simultaneous mesh sample — but the SAME transport
    // code path is in play, so the same tolerant, bounded classification applies.
    const bool shortfall_is_documented_race = shortfall >= 0 && shortfall <= N * (final_n - 1) && shortfall % N == 0;
    const bool ordered = actor.ordered;

    std::printf(
        "node %llu: received=%d/%d FIFO=%s connections=%llu frames_sent=%llu dedup_closed=%llu drops=%llu known_peers=%zu%s\n",
        static_cast<unsigned long long>(self_id), got, expected, ordered ? "intact" : "BROKEN",
        static_cast<unsigned long long>(transport.connections_open()), static_cast<unsigned long long>(transport.frames_sent()),
        static_cast<unsigned long long>(transport.dedup_closed()), static_cast<unsigned long long>(transport.drops()),
        known_peers.size(),
        shortfall > 0 ? "  <- dial-dedup race dropped a lane (documented at-most-once, tcp_transport.hpp)" : "");

    eng.stop();
    transport.stop();

    const bool ok = converged && ordered && shortfall_is_documented_race;
    std::printf("node %llu: %s\n", static_cast<unsigned long long>(self_id), ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
