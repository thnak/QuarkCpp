// Tests 017-Delivery-Guarantees §"AtLeastOnce retry+ack" (#27): ReliableSender::pump() re-entrancy.
// A sink may call on_ack()/enqueue() on the SAME sender during pump(): acks are deferred until the
// outermost pump returns (the in-flight body reference stays valid), enqueue is safe (seq-cursor
// iteration), and non-re-entrant use is unchanged. Deterministic, single-threaded.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/delivery.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

struct Seen {
    std::uint64_t seq;
    std::uint64_t base;
    std::string body;
};

}  // namespace

int main() {
    bool ok = true;

    // (a) Sink acks the frame it just received, inside pump(). Pre-#27 this popped the deque under the
    //     range-for (heap-use-after-free). Now: no crash, every frame visited once in order, and the
    //     deferred acks are applied when pump() returns.
    {
        ReliableSender s(7);
        for (const char* b : {"a", "b", "c"}) s.enqueue(b);
        std::vector<Seen> seen;
        const std::size_t n = s.pump([&](const ReliableFrame& f, const std::string& body) {
            seen.push_back({f.seq, f.base, body});
            s.on_ack(f.seq);  // cumulative ack of the frame just sent
        });
        check(n == 3, "(a) pump sent three frames", ok);
        check(seen.size() == 3 && seen[0].seq == 1 && seen[1].seq == 2 && seen[2].seq == 3,
              "(a) frames visited in seq order, once each", ok);
        check(seen.size() == 3 && seen[0].body == "a" && seen[1].body == "b" && seen[2].body == "c",
              "(a) body references intact during sink", ok);
        check(s.pending() == 0, "(a) after pump the acks are applied", ok);
    }

    // (a2) Ack is deferred within the pump: acking frame 1 mid-pump does NOT stop frames 2..3 being sent
    //      in the same call, and the ack takes effect only when pump returns.
    {
        ReliableSender s(7);
        for (const char* b : {"a", "b", "c"}) s.enqueue(b);
        std::size_t pending_inside = 0;
        const std::size_t n = s.pump([&](const ReliableFrame& f, const std::string&) {
            if (f.seq == 1) s.on_ack(1);
            pending_inside = s.pending();
        });
        check(n == 3, "(a2) deferred ack does not truncate the pass", ok);
        check(pending_inside == 3, "(a2) pending() unchanged while inside pump", ok);
        check(s.pending() == 2, "(a2) ack of seq 1 applied after pump returns", ok);
    }

    // (b) Sink enqueues on the SAME sender during pump(). The new item is retained; with window=1 it is
    //     sent by the NEXT pump, with the same body and the next seq.
    {
        ReliableSender s(7);
        s.enqueue("a");
        bool did = false;
        const std::size_t n1 = s.pump([&](const ReliableFrame&, const std::string&) {
            if (!did) { did = true; s.enqueue("b"); }
        }, /*window=*/1);
        check(n1 == 1, "(b) first pump sent one frame", ok);
        check(s.pending() == 2, "(b) enqueued item retained", ok);
        s.on_ack(1);
        std::vector<Seen> seen;
        const std::size_t n2 = s.pump([&](const ReliableFrame& f, const std::string& body) {
            seen.push_back({f.seq, f.base, body});
        });
        check(n2 == 1, "(b) next pump sends the enqueued item", ok);
        check(seen.size() == 1 && seen[0].seq == 2 && seen[0].body == "b" && seen[0].base == 2,
              "(b) enqueued frame has seq 2, body 'b', base 2", ok);
    }

    // (b2) Sink enqueues during a window=0 pump: no crash, item retained (and reached by the cursor).
    {
        ReliableSender s(7);
        s.enqueue("a");
        bool did = false;
        const std::size_t n = s.pump([&](const ReliableFrame&, const std::string&) {
            if (!did) { did = true; s.enqueue("b"); }
        });
        check(n == 2, "(b2) window=0 pass also reaches the item enqueued mid-pump", ok);
        check(s.pending() == 2, "(b2) nothing acked, both retained", ok);
    }

    // (c) Sink acks everything while window=0 (all): the whole pass iterates, pending drains to zero.
    {
        ReliableSender s(7);
        for (int i = 0; i < 5; ++i) s.enqueue("m" + std::to_string(i));
        std::size_t visits = 0;
        const std::size_t n = s.pump([&](const ReliableFrame& f, const std::string& body) {
            ++visits;
            check(body == "m" + std::to_string(f.seq - 1), "(c) body matches seq", ok);
            s.on_ack(f.seq);
        }, /*window=*/0);
        check(n == 5 && visits == 5, "(c) window=0 visits all five frames", ok);
        check(s.pending() == 0, "(c) everything acked after pump", ok);
    }

    // (d) Non-re-entrant behaviour unchanged: same frames, same base, same return values as before #27.
    {
        ReliableSender s(9);
        for (int i = 0; i < 4; ++i) s.enqueue("x");
        std::vector<Seen> seen;
        auto record = [&](const ReliableFrame& f, const std::string& body) {
            seen.push_back({f.seq, f.base, body});
        };
        check(s.pump(record, /*window=*/2) == 2, "(d) window=2 sends two", ok);
        check(seen.size() == 2 && seen[0].seq == 1 && seen[1].seq == 2, "(d) first window: seq 1,2", ok);
        check(seen.size() == 2 && seen[0].base == 1 && seen[1].base == 1, "(d) base == oldest unacked", ok);
        check(s.pump(record) == 4, "(d) window=0 resends all four", ok);
        s.on_ack(2);
        seen.clear();
        check(s.pump(record) == 2, "(d) after ack(2) two frames remain", ok);
        check(seen.size() == 2 && seen[0].seq == 3 && seen[1].seq == 4, "(d) remaining seq 3,4", ok);
        check(seen.size() == 2 && seen[0].base == 3 && seen[1].base == 3, "(d) base advanced to 3", ok);
        s.on_ack(4);
        check(s.pending() == 0, "(d) empty after full ack", ok);
        check(s.pump(record) == 0, "(d) empty pump sends nothing", ok);
    }

    // (e) Nested pump from inside a sink: the inner pump must NOT flush the deferred ack while the outer
    //     sink call is still in flight; the ack lands when the OUTERMOST pump returns.
    {
        ReliableSender s(7);
        s.enqueue("a");
        s.enqueue("b");
        std::size_t inner_sent = 0;
        bool nested = false;
        const std::size_t n = s.pump([&](const ReliableFrame& f, const std::string& body) {
            if (f.seq == 1 && !nested) {
                nested = true;
                s.on_ack(1);
                inner_sent = s.pump([&](const ReliableFrame&, const std::string& b2) {
                    check(!b2.empty(), "(e) nested body valid", ok);
                }, /*window=*/1);
                check(s.pending() == 2, "(e) inner pump does not apply the outer's deferred ack", ok);
            }
            check(!body.empty(), "(e) outer body still valid after nested pump", ok);
        });
        check(n == 2, "(e) outer pump sent both frames", ok);
        check(inner_sent == 1, "(e) nested pump sent its frame", ok);
        check(s.pending() == 1, "(e) deferred ack applied when outermost pump returns", ok);
    }

    std::printf("delivery_pump_reentrancy_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
