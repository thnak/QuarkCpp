// ADR-080: the device client's only platform seam. The application supplies a blocking socket for its
// OS (Android/iOS/desktop); the library never starts a PAL IoContext/reactor, so a phone does not need a
// PAL backend. All protocol knowledge stays in the library.
#pragma once

#include <cstddef>
#include <span>

namespace quark::client {

class ByteStream {
public:
    virtual ~ByteStream() = default;

    // Blocking read: >0 = bytes read, 0 = the peer closed (clean EOF), <0 = error. A concurrent
    // close() MUST unblock a blocked read (the transport's stop() relies on it).
    virtual std::ptrdiff_t read(std::span<std::byte> out) = 0;

    // Blocking write of every byte: returns in.size() on success, <0 on error. close() may be called
    // concurrently with write() and must be safe to call twice.
    virtual std::ptrdiff_t write(std::span<const std::byte> in) = 0;

    virtual void close() noexcept = 0;
};

}  // namespace quark::client
