// ADR-080 S3c: the dialling side of the HELLO wire (mTLS mode), for a device that must present a
// certificate rather than a pinned key. Mirrors TcpTransport's dialer behaviour: send the 14-byte HELLO,
// read and validate the acceptor's HELLO (it must be the node we dialled), consume the transport-internal
// DialHint frame, then pass application frames through with their real ids (the HELLO wire carries them).
//
// start() performs the exchange synchronously, so the application must construct SecureTransport and
// install its handshake BEFORE start(); the first send() then opens the TLS-in-Authenticate handshake.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "quark/client/byte_stream.hpp"
#include "quark/core/transport.hpp"
#include "quark/net/tcp_transport.hpp"  // hello_magic/version/size: one source of truth
#include "quark/net/wire_codec.hpp"

namespace quark::client {

class HelloClientTransport final : public Transport {
public:
    HelloClientTransport(ByteStream& stream, NodeId self, NodeId peer) noexcept
        : stream_(&stream), self_(self), peer_(peer) {}
    ~HelloClientTransport() override { stop(); }

    HelloClientTransport(const HelloClientTransport&) = delete;
    HelloClientTransport& operator=(const HelloClientTransport&) = delete;

    // Blocking: write our HELLO, read + validate the acceptor's, then start the reader. Returns false on
    // a bad magic/version, a different answering node, or a closed stream.
    bool start() {
        std::array<std::byte, net::hello_size> hello = encode_hello(self_);
        if (stream_->write(std::span<const std::byte>(hello)) !=
            static_cast<std::ptrdiff_t>(hello.size()))
            return false;
        std::array<std::byte, net::hello_size> peer_hello{};
        if (!read_exact(std::span<std::byte>(peer_hello))) return false;
        if (!parse_hello(peer_hello, peer_, peer_ver_)) return false;
        reader_ = std::thread([this] { read_loop(); });
        return true;
    }

    void stop() noexcept {
        closed_.store(true, std::memory_order_release);
        stream_->close();
        if (!reader_.joinable()) return;
        if (reader_.get_id() == std::this_thread::get_id())
            reader_.detach();
        else
            reader_.join();
    }

    void send(NodeId to, MessageFrame f) override {
        if (closed_.load(std::memory_order_acquire) || !(to == peer_)) return;
        // The HELLO wire carries real ids (unlike the pinned wire); only the origin is ours to set.
        if (!(f.from == self_) || !(f.to == peer_)) return;
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

    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint16_t peer_hello_version() const noexcept { return peer_ver_; }

private:
    [[nodiscard]] static std::array<std::byte, net::hello_size> encode_hello(NodeId self) noexcept {
        std::array<std::byte, net::hello_size> out{};
        put_le(out, 0, static_cast<std::uint32_t>(net::hello_magic), 4);
        put_le(out, 4, static_cast<std::uint16_t>(net::hello_version), 2);
        put_le(out, 6, self.value, 8);
        return out;
    }

    // Same acceptance rule as TcpTransport::check_hello, plus "the node that answered is the one dialled"
    // (a dialer already knows the peer; an impostor must not take the connection).
    [[nodiscard]] static bool parse_hello(const std::array<std::byte, net::hello_size>& h, NodeId expect,
                                          std::uint16_t& version) noexcept {
        const auto magic = get_le32(h, 0);
        const auto ver = get_le16(h, 4);
        const auto node = get_le64(h, 6);
        if (magic != net::hello_magic) return false;
        const auto diff = ver > net::hello_version ? ver - net::hello_version : net::hello_version - ver;
        if (diff > net::hello_version_window) return false;
        if (NodeId{node} != expect) return false;
        version = ver;
        return true;
    }

    static void put_le(std::array<std::byte, net::hello_size>& out, std::size_t off, std::uint64_t v,
                       std::size_t n) noexcept {
        for (std::size_t i = 0; i < n && off + i < out.size(); ++i)
            out[off + i] = static_cast<std::byte>(v >> (8 * i));
    }
    [[nodiscard]] static std::uint16_t get_le16(const std::array<std::byte, net::hello_size>& in,
                                                std::size_t off) noexcept {
        return static_cast<std::uint16_t>(get_le(in, off, 2));
    }
    [[nodiscard]] static std::uint32_t get_le32(const std::array<std::byte, net::hello_size>& in,
                                                std::size_t off) noexcept {
        return static_cast<std::uint32_t>(get_le(in, off, 4));
    }
    [[nodiscard]] static std::uint64_t get_le64(const std::array<std::byte, net::hello_size>& in,
                                                std::size_t off) noexcept {
        return get_le(in, off, 8);
    }
    [[nodiscard]] static std::uint64_t get_le(const std::array<std::byte, net::hello_size>& in,
                                              std::size_t off, std::size_t n) noexcept {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[off + i])) << (8 * i);
        return v;
    }

    [[nodiscard]] bool read_exact(std::span<std::byte> out) {
        std::size_t off = 0;
        while (off < out.size()) {
            const std::ptrdiff_t n = stream_->read(out.subspan(off));
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    void read_loop() {
        net::FrameStream frames;
        std::array<std::byte, 4096> buf{};
        while (!closed_.load(std::memory_order_acquire)) {
            const std::ptrdiff_t n = stream_->read(buf);
            if (n <= 0) break;
            const bool ok = frames.feed(buf.data(), static_cast<std::size_t>(n), [this](MessageFrame f) {
                if (f.kind == FrameKind::DialHint) return;  // transport-internal (ADR-065)
                if (!(f.from == peer_) || !(f.to == self_)) return;  // single peer, direct frames only
                std::function<void(MessageFrame)> cb;
                {
                    std::lock_guard<std::mutex> g(recv_mu_);
                    cb = recv_;
                }
                if (cb) cb(std::move(f));
            });
            if (!ok) break;
        }
        closed_.store(true, std::memory_order_release);
    }

    ByteStream* stream_;
    NodeId self_;
    NodeId peer_;
    std::uint16_t peer_ver_ = 0;
    std::mutex write_mu_;
    std::mutex recv_mu_;
    std::function<void(MessageFrame)> recv_;
    std::atomic<bool> closed_{false};
    std::thread reader_;
};

}  // namespace quark::client
