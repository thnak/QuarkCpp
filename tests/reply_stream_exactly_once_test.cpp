// Tests 006 §ask_stream / ADR-018 GATE-2 (intra-stream FIFO) + GATE-6 (per-item exactly-once via
// callee-assigned producer_seq, 017). Two halves, both single-thread deterministic (no wall clock):
//   (1) the ReplyDedup watermark in isolation — deliver 0..9 fresh, replay 5..9 (all dups dropped),
//       final delivered=10 dup=0 gap=0 (mirrors ADR-018 C3 "replay logical 5..9 -> delivered=10");
//       plus a caller-local-index CONTROL showing why the index is not valid identity.
//   (2) end-to-end through the flipped ring: push N items, drain via ReplyStream::next(), assert the
//       payloads come out in monotone producer order with 0 inversions / 0 dups / 0 loss.
#include <cstdint>
#include <cstdio>
#include <memory_resource>

#include "quark/core/reply_stream.hpp"

using namespace quark;

namespace {
struct Row {
    std::uint64_t id;
    std::uint64_t check;
};
static_assert(sizeof(ReplyItem<Row>) <= 4096, "reply item fits a ring slot");

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    // ---- (1) ReplyDedup watermark: producer_seq is exactly-once, replay-deterministic (017) -------
    {
        ReplyDedup d;
        std::uint64_t delivered = 0, dup = 0;
        for (std::uint64_t s = 0; s < 10; ++s) {
            if (d.accept(s)) ++delivered; else ++dup;
        }
        // Replay of logical 5..9 with the SAME producer_seq (a transport retransmit / re-activation).
        for (std::uint64_t s = 5; s < 10; ++s) {
            if (d.accept(s)) ++delivered; else ++dup;
        }
        check(delivered == 10, "producer_seq: each of 10 items delivered exactly once", ok);
        check(dup == 5, "producer_seq: the 5 replayed items are dropped as duplicates", ok);
        check(!d.gap(), "producer_seq: no false gap over an in-order+replay sequence", ok);
        check(d.watermark() == 10, "producer_seq: watermark advanced to 10", ok);
    }

    // CONTROL: a caller-local ring INDEX is not valid identity — on re-activation the same items are
    // re-assigned indices 0..4 (a "fresh" ring window), so an index-keyed watermark would treat the
    // replay as fresh and DELIVER DUPLICATES. We model that: a monotone index that resets per window.
    {
        ReplyDedup idx_gate;  // pretend seq == ring index, which restarts each activation window
        std::uint64_t delivered = 0;
        for (std::uint64_t i = 0; i < 5; ++i) delivered += idx_gate.accept(i) ? 1 : 0;  // window A: 0..4
        // re-activation: the SAME 5 logical items arrive again as indices 0..4 — but the gate already
        // advanced past 4, so it (correctly) drops them. The DEFECT the control demonstrates is that
        // WITHOUT a stable producer_seq the two windows are indistinguishable; here we show that reusing
        // low indices after a watermark reset WOULD double-deliver:
        ReplyDedup reset_gate;
        std::uint64_t dbl = 0;
        for (std::uint64_t i = 0; i < 5; ++i) dbl += reset_gate.accept(i) ? 1 : 0;  // window A
        ReplyDedup reset_gate2;  // a fresh window-local gate (index restarts) — the mis-design
        for (std::uint64_t i = 0; i < 5; ++i) dbl += reset_gate2.accept(i) ? 1 : 0;  // window B: DUPS
        check(delivered == 5, "control: in-order window delivers 5", ok);
        check(dbl == 10, "control fires: window-local index identity double-delivers on replay (10)", ok);
    }

    // ---- (2) end-to-end FIFO through the flipped ring -------------------------------------------
    {
        constexpr std::uint64_t kN = 200'000;
        std::pmr::monotonic_buffer_resource mr;
        ReplyStreamState<Row>::Config cfg;
        cfg.capacity = 256;
        auto req = make_ask_stream<int, Row>(0, &mr, cfg);
        auto producer = req.envelope.respond.accept();
        auto rs_res = block_on_open(std::move(req.future));
        check(rs_res.has_value(), "OPEN resolves and hands a ReplyStream", ok);
        if (!rs_res) { std::fprintf(stderr, "FAIL: no reply stream\n"); return 1; }
        auto rs = std::move(rs_res.value());

        std::uint64_t produced = 0, consumed = 0;
        std::uint64_t expect = 0;
        bool order_ok = true, check_ok = true;
        // Interleave: push until the ring stalls, then drain, so the batch-flush boundary is exercised.
        while (produced < kN) {
            ReplyPush pr = producer.try_push(Row{produced, produced * 2654435761u});
            if (pr == ReplyPush::Ok) {
                ++produced;
            } else if (pr == ReplyPush::WouldStall) {
                // drain a chunk to free credit (0-RMW pull)
                while (auto r = rs.next()) {
                    if (r->id != expect) order_ok = false;
                    if (r->check != r->id * 2654435761u) check_ok = false;
                    ++expect;
                    ++consumed;
                }
            } else {
                order_ok = false;  // unexpected Terminated
                break;
            }
        }
        producer.close();
        while (auto r = rs.next()) {
            if (r->id != expect) order_ok = false;
            if (r->check != r->id * 2654435761u) check_ok = false;
            ++expect;
            ++consumed;
        }
        check(produced == kN, "all items produced", ok);
        check(consumed == kN, "all items consumed — 0 loss", ok);
        check(order_ok, "FIFO: 0 inversions across the whole stream", ok);
        check(check_ok, "0 torn payloads (checksum intact)", ok);
        check(rs.done(), "stream drained + terminal", ok);
        check(rs.terminal() == ReplyStreamTerminal::Closed, "terminal is Closed (EoS)", ok);
        check(!rs.gap_detected(), "no gap detected on the caller watermark", ok);
    }

    std::fprintf(stderr, "reply_stream_exactly_once_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
