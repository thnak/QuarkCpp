// Tests 006 §ask_stream / ADR-018 GATE-4 (reply-lifetime, no UAF, reclaimed EXACTLY ONCE) + GATE-5
// (cancel/deadline teardown: return credit, stop the callee, deliver nothing PRODUCED after teardown,
// leak no ring). Also the reply-before-teardown guarantee for the OPEN handshake (a dropped
// StreamResponder fails OPEN so the caller never hangs). Single-thread deterministic. Best run under
// ASan/UBSan — a missed terminal interleaving here is a use-after-free, not a wrong value.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <memory_resource>

#include "quark/core/reply_stream.hpp"

using namespace quark;

namespace {
struct Row { std::uint64_t id; };
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}

// A memory_resource that counts outstanding allocations, so we can prove the reply ring is
// deallocated EXACTLY ONCE (net outstanding returns to 0) when the last handle drops — the GATE-4
// reclaim-once property, independent of (and complementary to) an ASan double-free trap.
class CountingResource : public std::pmr::memory_resource {
public:
    std::int64_t outstanding = 0;
    std::int64_t allocs = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        ++outstanding; ++allocs;
        return std::pmr::new_delete_resource()->allocate(bytes, align);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(p, bytes, align);
    }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }
};
}  // namespace

int main() {
    bool ok = true;
    std::pmr::monotonic_buffer_resource mr;

    // ---- (A) caller cancel: first-wins terminal, producer stops, no post-teardown production -------
    {
        auto req = make_ask_stream<int, Row>(0, &mr);
        auto producer = req.envelope.respond.accept();
        auto rs_res = block_on_open(std::move(req.future));
        if (!rs_res) { std::fprintf(stderr, "FAIL: no reply stream (A)\n"); return 1; }
        auto rs = std::move(rs_res.value());

        check(producer.try_push(Row{0}) == ReplyPush::Ok, "pre-cancel push ok", ok);
        check(producer.try_push(Row{1}) == ReplyPush::Ok, "pre-cancel push ok", ok);

        rs.cancel();  // caller tears the stream down
        check(rs.terminal() == ReplyStreamTerminal::Cancelled, "cancel latches Cancelled", ok);
        // Producing AFTER teardown must be refused (the callee observes Terminated and stops).
        check(producer.try_push(Row{2}) == ReplyPush::Terminated,
              "GATE-5: no item is produced after teardown", ok);
        check(producer.push(Row{3}) == ReplyPush::Terminated, "blocking push also stops", ok);
        // A late close() loses to the already-latched Cancelled (first-cause-wins).
        producer.close();
        check(rs.terminal() == ReplyStreamTerminal::Cancelled, "terminal is first-wins (Cancelled)", ok);
        // Items produced BEFORE teardown remain drainable (they were not "produced after").
        std::uint64_t n = 0;
        while (auto r = rs.next()) { (void)r; ++n; }
        check(n == 2, "pre-teardown buffered items still delivered (nothing lost, nothing new)", ok);
    }

    // ---- (B) deadline expiry maps to DeadlineExceeded ------------------------------------------
    {
        auto req = make_ask_stream<int, Row>(0, &mr);
        auto producer = req.envelope.respond.accept();
        auto rs = block_on_open(std::move(req.future)).value();
        rs.expire_deadline();
        check(rs.terminal() == ReplyStreamTerminal::DeadlineExceeded, "deadline latches DeadlineExceeded", ok);
        check(producer.try_push(Row{9}) == ReplyPush::Terminated, "callee stops on deadline", ok);
    }

    // ---- (C) reclaimed EXACTLY ONCE — no leak, no double free (GATE-4) -------------------------
    {
        CountingResource cr;
        {
            auto req = make_ask_stream<int, Row>(0, &cr);
            auto producer = req.envelope.respond.accept();
            auto rs = block_on_open(std::move(req.future)).value();
            (void)producer.try_push(Row{0});
            check(cr.outstanding > 0, "ring allocated from the caller-shard resource", ok);
            // Drop the stream first, then the producer.
            { auto sink = std::move(rs); (void)sink; }  // ~ReplyStream -> cancel + drop one ref
            check(producer.terminal() == ReplyStreamTerminal::Cancelled,
                  "dropping the ReplyStream cancels (producer sees teardown)", ok);
            check(cr.outstanding > 0, "ring NOT freed while the producer still holds a ref", ok);
        }  // ~ReplyStreamProducer drops the last ref -> ReplyStreamState reclaimed exactly once here
        check(cr.outstanding == 0,
              "ring reclaimed exactly once when the last handle drops (0 outstanding — no leak)", ok);
        check(cr.allocs > 0, "sanity: the counting resource actually served the ring", ok);
    }

    // ---- (D) OPEN reply-before-teardown: a dropped responder fails OPEN (caller never hangs) ------
    {
        auto req = make_ask_stream<int, Row>(0, &mr);
        // The "handler" drops the StreamResponder WITHOUT accept() (e.g. it threw / was reclaimed).
        { auto sink = std::move(req.envelope.respond); (void)sink; }
        auto rs_res = block_on_open(std::move(req.future));
        check(!rs_res.has_value(), "dropped responder fails OPEN — block_on returns an error, no hang", ok);
        if (!rs_res) check(rs_res.error().code == errc::supervised_stop, "OPEN failure carries no_reply", ok);
    }

    // ---- (E) reject before producing: OPEN carries the error --------------------------------------
    {
        auto req = make_ask_stream<int, Row>(0, &mr);
        req.envelope.respond.reject(error{errc::overloaded, "shed"});
        auto rs_res = block_on_open(std::move(req.future));
        check(!rs_res.has_value(), "reject() fails OPEN", ok);
        if (!rs_res) check(rs_res.error().code == errc::overloaded, "reject error propagates to caller", ok);
    }

    std::fprintf(stderr, "reply_stream_teardown_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
