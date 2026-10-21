// Tests ADR-048 (021 §3): SWIM's indirect ping (PingReq relay) works when replies arrive LATER, as
// they do over TcpTransport. Runs on the deferred-delivery fabric (swim_deferred_fabric.hpp) with a
// per-wave latency, never delivering in the sender's call stack.
//
// Reproduces the 2026-10-21 run_swim_chaos.sh defect: on the pre-ADR-048 code the relay checked for
// the subject's Ack before returning from the PingReq handler, so over a real transport no PingReqAck
// was ever sent. With only the 1<->2 link cut, nodes 1 and 2 suspected and refuted each other about
// once a second (incarnation 1 → 31 in 22 s) although relays 3/4/5 could reach both.
#include <cstdint>
#include <cstdio>

#include "swim_deferred_fabric.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr ClusterId kCluster{0xC4A05};
constexpr std::int64_t MS = 1'000'000;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

bool cut_1_2(NodeId a, NodeId b) { return (a.value == 1 && b.value == 2) || (a.value == 2 && b.value == 1); }
}  // namespace

int main() {
    // Latency per hop. The relay chain is 4 hops (PingReq → Ping → Ack → PingReqAck), which must fit in
    // the indirect window (100 ms, quantized to the 120 ms tick), so every d here keeps 4d < 120 ms.
    for (std::int64_t d_ms : {0, 10, 25}) {
        for (std::uint64_t seed = 0; seed < 4; ++seed) {
            Sim sim;
            sim.cfg.cluster_id = kCluster;
            sim.cfg.ack_timeout_ns = 100 * MS;
            sim.cfg.suspicion_timeout_ns = 500 * MS;
            sim.cfg.seed = 0xA5A5;
            sim.d = d_ms * MS;
            sim.shuffle = seed;
            for (std::uint64_t i = 1; i <= 5; ++i) sim.add(i);
            sim.admit_all();
            sim.fab.link = [](NodeId a, NodeId b) { return cut_1_2(a, b) ? Link::Drop : Link::Deliver; };
            const std::uint64_t inc1 = sim.s(1).self_incarnation(), inc2 = sim.s(2).self_incarnation();
            bool ever_dead = false;
            for (int t = 0; t < 1000; ++t) {
                sim.round();
                ever_dead = ever_dead || !sim.s(1).view().contains(NodeId{2}) || !sim.s(2).view().contains(NodeId{1});
            }
            const std::uint64_t bumps = (sim.s(1).self_incarnation() - inc1) + (sim.s(2).self_incarnation() - inc2);
            check(!ever_dead, "1 and 2 never drop each other while relays 3/4/5 can reach both");
            check(bumps == 0, "no refutation was ever needed: the relays vouched for 1 and 2 every time");
            if (seed == 0)
                std::printf("  d=%lld ms: incarnation bumps over 1000 ticks = %llu (nodes 1+2; pre-ADR-048 code: 363)\n",
                            static_cast<long long>(d_ms), static_cast<unsigned long long>(bumps));
        }
    }
    std::printf("cluster_swim_async_relay_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
