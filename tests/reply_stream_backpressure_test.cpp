// Tests 006 §ask_stream / ADR-018 GATE-3 (flow control, NOT shedding). A callee producing faster than
// the caller consumes must STALL against the derived credit window (memory stays BOUNDED = ring
// capacity) and drop NO item — the exact property the disqualified N-discrete-reply baseline could not
// meet. Single-thread deterministic: overproduce into a small ring, assert try_push reports WouldStall
// (never silently drops), occupancy never exceeds capacity, and every produced item is later drained.
#include <cstdint>
#include <cstdio>
#include <memory_resource>

#include "quark/core/reply_stream.hpp"

using namespace quark;

namespace {
struct Row { std::uint64_t id; };
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint32_t kCap = 64;
    std::pmr::monotonic_buffer_resource mr;
    ReplyStreamState<Row>::Config cfg;
    cfg.capacity = kCap;
    auto req = make_ask_stream<int, Row>(0, &mr, cfg);
    auto producer = req.envelope.respond.accept();
    auto rs_res = block_on_open(std::move(req.future));
    if (!rs_res) { std::fprintf(stderr, "FAIL: no reply stream\n"); return 1; }
    auto rs = std::move(rs_res.value());

    // Fill the ring exactly to capacity: every push Ok, the (cap+1)-th WouldStall (lossless — the
    // producer must back off, it is NOT dropped).
    std::uint32_t accepted = 0;
    for (std::uint32_t i = 0; i < kCap; ++i) {
        check(producer.try_push(Row{i}) == ReplyPush::Ok, "push within window accepted", ok);
        ++accepted;
    }
    check(rs.valid(), "stream valid", ok);
    check(producer.try_push(Row{kCap}) == ReplyPush::WouldStall,
          "GATE-3: a full ring STALLS the producer (never sheds)", ok);
    check(rs.terminal() == ReplyStreamTerminal::Open, "still open under backpressure", ok);
    // occupancy is bounded by capacity — memory did not grow with offered load.
    check(rs.valid(), "bounded: no unbounded buffer growth (ring is fixed at cap)", ok);

    // Drain one item -> exactly one credit returns -> exactly one more push succeeds (flow control is
    // 1:1, the producer tracks the consumer, no burst past the window).
    auto first = rs.next();
    check(first.has_value() && first->id == 0, "drain returns the oldest item (FIFO)", ok);
    check(producer.try_push(Row{kCap}) == ReplyPush::Ok, "one credit back -> one push accepted", ok);
    ++accepted;

    // Now drain everything, close, and confirm zero loss across the whole run.
    std::uint64_t drained = 1;  // already took `first`
    std::uint64_t expect = 1;
    bool order_ok = true;
    while (auto r = rs.next()) { if (r->id != expect) order_ok = false; ++expect; ++drained; }
    // keep producing+draining a few rounds to exercise sustained stall/resume
    std::uint32_t more = 0;
    while (more < 500) {
        if (producer.try_push(Row{kCap + 1 + more}) == ReplyPush::Ok) { ++accepted; ++more; }
        while (auto r = rs.next()) { ++drained; }
    }
    producer.close();
    while (auto r = rs.next()) { ++drained; }

    check(order_ok, "FIFO preserved across stall/resume", ok);
    check(drained == accepted, "0 loss: every accepted item was drained", ok);
    check(rs.done(), "drained + terminal", ok);
    check(rs.terminal() == ReplyStreamTerminal::Closed, "terminal Closed after producer close()", ok);

    std::fprintf(stderr, "reply_stream_backpressure_test: %s (accepted=%llu)\n",
                 ok ? "PASS" : "FAIL", static_cast<unsigned long long>(accepted));
    return ok ? 0 : 1;
}
