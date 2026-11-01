// #65: a 4-node full mesh over real loopback sockets, every pair dialing simultaneously and every node
// sending a burst to every OTHER node at the same instant. This is the sample-17 shape with the engine
// removed: it asserts the TRANSPORT delivers the entire race burst exactly once per (sender,receiver),
// which is the property the DialHint handshake exists to give.
//
// Run: tcp_transport_dial_race_mesh_test [frames_per_lane=500]
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

constexpr std::size_t kNodes = 4;
constexpr std::uint64_t kSenderShift = 40;
constexpr std::uint64_t kSeqMask = (1ull << kSenderShift) - 1;

struct MeshRecorder {
    std::mutex m;
    std::vector<std::uint64_t> ids;  // trace_id == (sender << kSenderShift) | seq
    std::atomic<std::uint64_t> n{0};
    void operator()(MessageFrame f) {
        std::lock_guard<std::mutex> g(m);
        ids.push_back(f.trace_id);
        n.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return n.load(std::memory_order_acquire); }
    [[nodiscard]] std::vector<std::uint64_t> snapshot() {
        std::lock_guard<std::mutex> g(m);
        return ids;
    }
};

[[nodiscard]] NodeId node_of(std::size_t index) {
    return NodeId{static_cast<std::uint64_t>(index + 1)};
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 500;
    const std::uint64_t expected = static_cast<std::uint64_t>(kNodes - 1) * n;

    std::array<std::unique_ptr<net::TcpTransport>, kNodes> t;
    std::array<MeshRecorder, kNodes> rec;
    std::array<std::uint16_t, kNodes> port{};
    bool ok = true;

    for (std::size_t i = 0; i < kNodes; ++i) {
        t[i] = std::make_unique<net::TcpTransport>(node_of(i), pal::ipv4_loopback, 0);
        t[i]->on_receive([&, i](MessageFrame f) { rec[i](std::move(f)); });
        ok = ok && t[i]->start();
        port[i] = t[i]->listen_port();
    }
    for (std::size_t i = 0; i < kNodes; ++i)
        for (std::size_t j = 0; j < kNodes; ++j)
            if (i != j) t[i]->add_peer(loopback_endpoint(node_of(j), port[j]));

    // Fire every node's lane to every other node at the same instant: all six pairs' first dials race.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::array<std::thread, kNodes> threads;
    for (std::size_t i = 0; i < kNodes; ++i) {
        threads[i] = std::thread([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            const NodeId from = node_of(i);
            for (std::size_t j = 0; j < kNodes; ++j) {
                if (j == i) continue;
                const NodeId to = node_of(j);
                for (std::uint64_t k = 0; k < n; ++k)
                    t[i]->send(to, seq_frame(from, to,
                                             (static_cast<std::uint64_t>(i + 1) << kSenderShift) | k));
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(kNodes)) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    const bool converged = wait_until([&] {
        for (std::size_t i = 0; i < kNodes; ++i)
            if (t[i]->connections_open() != static_cast<std::uint64_t>(kNodes - 1)) return false;
        return true;
    }, 20000);
    const bool delivered = wait_until([&] {
        for (std::size_t i = 0; i < kNodes; ++i)
            if (rec[i].count() != expected) return false;
        return true;
    }, 20000);

    for (std::size_t i = 0; i < kNodes; ++i) {
        const std::vector<std::uint64_t> ids = rec[i].snapshot();
        std::array<std::uint64_t, kNodes> next{};
        bool ordered = true;
        for (const std::uint64_t id : ids) {
            const std::uint64_t sender = id >> kSenderShift;
            const std::uint64_t seq = id & kSeqMask;
            const std::size_t s = static_cast<std::size_t>(sender - 1);
            if (s >= kNodes || s == i || seq != next[s]) {
                ordered = false;
                break;
            }
            ++next[s];
        }
        if (!ordered) ok = false;
        std::printf(
            "node %zu: received=%llu/%llu ordered=%s hints=%llu/%llu deferred=%llu timeouts=%llu "
            "dedup=%llu\n",
            i + 1, static_cast<unsigned long long>(rec[i].count()),
            static_cast<unsigned long long>(expected), ordered ? "yes" : "NO",
            static_cast<unsigned long long>(t[i]->dial_hints_sent()),
            static_cast<unsigned long long>(t[i]->dial_hints_received()),
            static_cast<unsigned long long>(t[i]->settle_deferred()),
            static_cast<unsigned long long>(t[i]->settle_timeouts()),
            static_cast<unsigned long long>(t[i]->dedup_closed()));
        if (rec[i].count() != expected) ok = false;
    }
    if (!converged) std::printf("FAIL: not every node converged to one connection per peer\n");
    if (!delivered) std::printf("FAIL: burst not fully delivered within 20s\n");

    std::printf("tcp_transport_dial_race_mesh_test: %s (n=%llu per lane)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(n));
    for (auto& tr : t) tr->stop();
    return ok ? 0 : 1;
}
