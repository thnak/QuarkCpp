// ADR-080 slice S3a: the shipped desktop ByteStream. A real blocking TCP socket through the PAL, for
// applications that do not supply their own (a phone app still implements ByteStream over its platform
// socket). No IoContext/reactor: recv()/send() park the calling thread, and close() shuts the socket down
// first so a read blocked on another thread (the transport's reader) wakes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <system_error>

#include "pal/net.hpp"
#include "quark/client/byte_stream.hpp"

namespace quark::client {

class SocketByteStream final : public ByteStream {
public:
    // Blocking connect to addr:port (PAL ipv4 form). The stream owns the socket.
    [[nodiscard]] static std::expected<std::unique_ptr<SocketByteStream>, std::error_code> connect(
        std::uint64_t addr, std::uint16_t port) {
        auto fd = pal::tcp_connect_blocking(addr, port);
        if (!fd) return std::unexpected(fd.error());
        return std::unique_ptr<SocketByteStream>(new SocketByteStream(*fd));
    }

    explicit SocketByteStream(pal::fd_t fd) noexcept : fd_(fd) {}
    ~SocketByteStream() override { close(); }

    SocketByteStream(const SocketByteStream&) = delete;
    SocketByteStream& operator=(const SocketByteStream&) = delete;

    std::ptrdiff_t read(std::span<std::byte> out) override {
        if (out.empty()) return 0;
        for (;;) {
            const pal::fd_t fd = fd_.load(std::memory_order_acquire);
            if (fd == pal::invalid_fd) return 0;
            auto r = pal::recv_some(fd, out.data(), out.size());
            if (r) return static_cast<std::ptrdiff_t>(*r);
            if (r.error() == std::errc::interrupted) continue;
            return -1;  // a blocking socket does not return would_block; anything else is a failure
        }
    }

    std::ptrdiff_t write(std::span<const std::byte> in) override {
        std::size_t off = 0;
        while (off < in.size()) {
            const pal::fd_t fd = fd_.load(std::memory_order_acquire);
            if (fd == pal::invalid_fd) return -1;
            auto w = pal::send_some(fd, in.data() + off, in.size() - off);
            if (w) {
                off += *w;
                continue;
            }
            if (w.error() == std::errc::interrupted) continue;
            return -1;
        }
        return static_cast<std::ptrdiff_t>(in.size());
    }

    void close() noexcept override {
        const pal::fd_t fd = fd_.exchange(pal::invalid_fd, std::memory_order_acq_rel);
        if (fd == pal::invalid_fd) return;
        pal::shutdown_socket(fd);  // wake a blocked read()/write() in another thread with a clean EOF
        pal::close_fd(fd);
    }

private:
    std::atomic<pal::fd_t> fd_;
};

}  // namespace quark::client
