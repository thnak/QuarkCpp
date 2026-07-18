// Tests 024-Streaming §Single-producer precondition (ADR-005 residual risk 3 / GATE-5): the SPSC
// guarantee holds only for ONE writer per cursor. The stream-open single-writer token enforces it —
// the FIRST bind hands out the StreamRef; a SECOND bind is a typed 007 error (errc::unavailable).
// Multi-source fan-in must stay on the mailbox (documented seam). Single-thread, deterministic.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory_resource>

#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

namespace {
struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    StreamActivation<Frame>::Config cfg;
    cfg.capacity = 64;
    std::pmr::monotonic_buffer_resource mr;
    StreamActivation<Frame> sa(cfg, &mr);

    // First bind wins — hands out the single-writer token.
    result<StreamRef<Frame>> first = open_stream(sa);
    check(first.has_value(), "first bind_producer() returns the single-writer token", ok);
    check(first.has_value() && first->valid(), "the token is valid", ok);

    // The token pushes frames.
    if (first.has_value()) {
        const bool pushed = first->try_push(Frame{0, detail::splitmix64(0)});
        check(pushed, "the bound producer can push", ok);
    }

    // Second bind is refused with a typed 007 error (the load-bearing precondition; a 2-writer control
    // fired a TSan race + 30,282 lost updates in ADR-005's negative control).
    result<StreamRef<Frame>> second = open_stream(sa);
    check(!second.has_value(), "second bind_producer() is REFUSED (single-producer precondition)", ok);
    check(!second.has_value() && second.error().code == errc::unavailable,
          "the refusal is a typed 007 error (errc::unavailable)", ok);

    std::printf("stream_single_producer_token_test: %s  (first=%d second_refused=%d code=%d)\n",
                ok ? "OK" : "FAIL", first.has_value() ? 1 : 0, !second.has_value() ? 1 : 0,
                second.has_value() ? -1 : static_cast<int>(second.error().code));
    return ok ? 0 : 1;
}
