// Tests 006 §ask_stream / ADR-018 GATE-1 (single-executor per leg) + GATE-7 (no lost wakeup) over a
// REAL producer/consumer thread pair across the flipped ring — the outbound analogue of the settled
// 024 stream_exactly_once_suspend_test. The callee thread push()es N items (blocking, lossless,
// waiting on the wrapper reverse-Dekker credit/terminal wake); the caller thread drains via next()
// until done(). Asserts: every item delivered exactly once, in FIFO order, 0 loss, and the run
// TERMINATES (no lost wakeup would hang the blocked producer against available credit).
//
// MACHINE SAFETY: exactly TWO threads (SPSC — the 024 precondition), modest N. A valid first-class
// TSan test (it is correct — it never wedges). Never saturates the box.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <thread>
#include <vector>

#include "quark/core/reply_stream.hpp"

using namespace quark;

namespace {
struct Row {
    std::uint64_t id;
    std::uint64_t check;
};
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
constexpr std::uint64_t kMix = 2654435761u;
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kN = 50'000;  // items; SPSC, two threads only — TSan-safe

    std::pmr::monotonic_buffer_resource mr;
    ReplyStreamState<Row>::Config cfg;
    cfg.capacity = 256;
    auto req = make_ask_stream<int, Row>(0, &mr, cfg);
    auto producer = req.envelope.respond.accept();
    auto rs_res = block_on_open(std::move(req.future));
    if (!rs_res) { std::fprintf(stderr, "FAIL: no reply stream\n"); return 1; }
    auto rs = std::move(rs_res.value());

    std::atomic<bool> producer_stopped{false};

    // Callee (producer) thread: push all N losslessly, then close (in-band EoS).
    std::thread callee([&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            ReplyPush pr = producer.push(Row{i, i ^ kMix});
            if (pr != ReplyPush::Ok) break;  // Terminated (should not happen in this test)
        }
        producer.close();
        producer_stopped.store(true, std::memory_order_release);
    });

    // Caller (consumer) thread: drain until the stream is terminal AND empty.
    std::uint64_t received = 0, expect = 0;
    bool order_ok = true, check_ok = true;
    std::thread caller([&] {
        for (;;) {
            bool drained_any = false;
            while (auto r = rs.next()) {
                if (r->id != expect) order_ok = false;
                if (r->check != (r->id ^ kMix)) check_ok = false;
                ++expect;
                ++received;
                drained_any = true;
            }
            if (rs.done()) break;
            if (!drained_any) std::this_thread::yield();  // ring momentarily empty, producer still live
        }
    });

    callee.join();
    caller.join();

    check(received == kN, "0 loss — every item delivered exactly once across the thread boundary", ok);
    check(order_ok, "FIFO order preserved end-to-end (GATE-2)", ok);
    check(check_ok, "0 torn payloads", ok);
    check(rs.done(), "stream terminated cleanly (no hang — GATE-7 no lost wakeup)", ok);
    check(rs.terminal() == ReplyStreamTerminal::Closed, "terminal Closed (EoS)", ok);
    check(!rs.gap_detected(), "no gap in the delivered sequence", ok);

    std::fprintf(stderr, "reply_stream_concurrency_test: %s (received=%llu)\n",
                 ok ? "PASS" : "FAIL", static_cast<unsigned long long>(received));
    return ok ? 0 : 1;
}
