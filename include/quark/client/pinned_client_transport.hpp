// ADR-080 slice S1: the dialling side of the ADR-072 pinned wire (no HELLO, no NodeId before the
// handshake), for a device's single connection to its home node. Wrap it in SecureTransport with a
// client-role NodeId (ADR-074 D3) to get the real handshake and sealed frames; this class only moves
// Quark frames over a blocking ByteStream and applies the pinned wire's id rules.
//
// One peer by construction: the transport is created for `peer`, so an inbound frame cannot be
// attributed to anyone else. There is no reconnect, HELLO, relay or SWIM here (ADR-080 non-goals).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "quark/client/byte_stream.hpp"
#include "quark/core/transport.hpp"
#include "quark/net/wire_codec.hpp"

namespace quark::client {

class PinnedClientTransport final : public Transport {
public:
    PinnedClientTransport(ByteStream& stream, NodeId self, NodeId peer) noexcept
        : stream_(&stream), self_(self), peer_(peer) {}
    ~PinnedClientTransport() override { stop(); }

    PinnedClientTransport(const PinnedClientTransport&) = delete;
    PinnedClientTransport& operator=(const PinnedClientTransport&) = delete;

    // Start the reader thread. Call after on_receive() and before the first SecureTransport::send().
    void start() {
        if (closed_.load(std::memory_order_acquire) || reader_.joinable()) return;
        reader_ = std::thread([this] { read_loop(); });
    }

    // Stop the reader. Closes the ByteStream to unblock read(); safe from the receive callback too.
    void stop() noexcept {
        closed_.store(true, std::memory_order_release);
        stream_->close();
        if (!reader_.joinable()) return;
        if (reader_.get_id() == std::this_thread::get_id())
            reader_.detach();  // called from the receive callback: cannot join ourselves
        else
            reader_.join();
    }

    void send(NodeId to, MessageFrame f) override {
        if (closed_.load(std::memory_order_acquire)) return;
        // One peer, direct frames only (ADR-080 non-goals: no relay). Reject anything that is not the
        // device's own frame to its home node, then strip the ids per ADR-072 item 3.
        if (!(to == peer_) || !(f.from == self_) || !(f.to == peer_)) return;
        f.from = NodeId{};
        f.to = NodeId{};
        const std::vector<std::byte> bytes = net::encode_frame(f);
        std::lock_guard<std::mutex> g(write_mu_);
        if (stream_->write(bytes) != static_cast<std::ptrdiff_t>(bytes.size())) {
            closed_.store(true, std::memory_order_release);
            stream_->close();
        }
    }

    void on_receive(std::function<void(MessageFrame)> cb) override {
        std::lock_guard<std::mutex> g(recv_mu_);
        recv_ = std::move(cb);
    }

    // The peer closed, the stream failed, or a malformed frame was received. The application reconnects
    // (ADR-080: reconnect/backoff stays with the application).
    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

private:
    void read_loop() {
        net::FrameStream frames;
        std::array<std::byte, 4096> buf{};
        while (!closed_.load(std::memory_order_acquire)) {
            const std::ptrdiff_t n = stream_->read(buf);
            if (n <= 0) break;
            const bool ok = frames.feed(buf.data(), static_cast<std::size_t>(n), [this](MessageFrame f) {
                // The pinned wire carries no ids at all; attribute every frame to the one peer this
                // transport dialled. Never trust a wire-supplied id.
                f.from = peer_;
                f.to = self_;
                std::function<void(MessageFrame)> cb;
                {
                    std::lock_guard<std::mutex> g(recv_mu_);
                    cb = recv_;
                }
                if (cb) cb(std::move(f));
            });
            if (!ok) break;  // malformed/oversized frame: the stream framing is untrustworthy
        }
        closed_.store(true, std::memory_order_release);
    }

    ByteStream* stream_;
    NodeId self_;
    NodeId peer_;
    std::mutex write_mu_;
    std::mutex recv_mu_;
    std::function<void(MessageFrame)> recv_;
    std::atomic<bool> closed_{false};
    std::thread reader_;
};

}  // namespace quark::client
