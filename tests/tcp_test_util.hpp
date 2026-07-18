// Shared helpers for the TcpTransport socket tests (NOT a *_test.cpp, so it is never compiled as a test
// of its own — just an include). A thread-safe frame recorder (the transport's on_receive fires on its
// I/O thread), a sleep-poll wait helper (no busy-spin — do not burn a core, repo machine-safety rule),
// and a MessageFrame builder that stamps a monotonic sequence in trace_id for FIFO assertions.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "quark/core/cluster.hpp"     // Endpoint
#include "quark/core/transport.hpp"   // MessageFrame
#include "quark/net/tcp_transport.hpp"

namespace quark::test {

// Thread-safe recorder: on_receive runs on the transport I/O thread; a test thread reads after a sync.
struct Recorder {
    std::mutex m;
    std::vector<std::uint64_t> seqs;  // trace_id of each received frame, in arrival order
    std::vector<std::vector<std::byte>> payloads;
    std::atomic<std::uint64_t> n{0};

    void operator()(MessageFrame f) {
        std::lock_guard<std::mutex> g(m);
        seqs.push_back(f.trace_id);
        payloads.push_back(std::move(f.payload));
        n.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return n.load(std::memory_order_acquire); }
};

// Poll `pred` every 1 ms until true or `timeout_ms` elapses. sleep-based, so it yields the CPU to the
// I/O threads instead of spinning. Returns pred()'s final value.
template <class Pred>
bool wait_until(Pred pred, int timeout_ms = 5000) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// A frame carrying `seq` in trace_id (the FIFO ordering marker) and a 1-byte payload derived from it.
inline MessageFrame seq_frame(NodeId from, NodeId to, std::uint64_t seq) {
    MessageFrame f;
    f.from = from;
    f.to = to;
    f.target = ActorId{TypeKey{0x5151u}, seq};
    f.msg_type = TypeKey{0x99u};
    f.mode = WireMode::Tagged;
    f.trace_id = seq;
    f.payload = {static_cast<std::byte>(seq & 0xFF)};
    return f;
}

inline Endpoint loopback_endpoint(NodeId node, std::uint16_t port) {
    return Endpoint{node, pal::ipv4_loopback, port};
}

}  // namespace quark::test
