// Implements 006-Messaging-and-Addressing §`ask_stream` (outbound streaming replies) as decided by
// ADR-018 (winner: Reply-Credit-Ring / PUSH). An `ask` whose reply is MULTI-ITEM cannot ride the
// single-shot ReplyCell (it resolves once; a stream resolves N times then completes). ADR-018's
// mechanism: outbound streaming replies are the 024 inbound credit-ring RUN BACKWARD — callee = head
// producer, caller = disp/tail consumer — reusing the shipped StreamChannel<F>/StreamActivation<F>
// (stream_channel.hpp / stream_activation.hpp) VERBATIM for the item-transport leg, with three
// outbound-specific seams sitting AROUND the ring (never inside it, so 024 is not forked):
//
//   (1) a SINGLE-RESOLVE StreamReplyCell OPEN handshake — hands the caller the drain handle exactly
//       once, ordered across concurrent asks by ADR-007 reply-ordering. It reuses the settled
//       detail::ReplyCell<Opened> win-arbitration + generation-fence VERBATIM (the ordinary
//       single-shot ReplyCell for scalar asks is UNTOUCHED — a stream never reuses a scalar cell).
//   (2) callee-assigned per-item `producer_seq` (017 identity): stream-relative, replay-deterministic;
//       the caller dedups by its `disp` high-watermark (a caller-local ring index is NOT valid
//       identity — it delivers dups on re-activation; 017 / ADR-018 C3/G6X).
//   (3) an in-band EoS + terminal (close / cancel / deadline / fail) with a TWO-PART terminal wake
//       (arm the caller drain AND wake a stalled callee), reclaimed EXACTLY ONCE (the ADR-007
//       reply-UAF gate extended to the multi-terminal surface; ADR-018 GATE-4/5).
//
// WHAT IS PROVEN vs WHAT IS DRAFT (ADR-018 §Promotion, honestly reproduced here):
//   * the ITEM-TRANSPORT leg IS the shipped 024 ring flipped — FIFO, 0 per-item heap, 0 caller-drain
//     cross-core RMW, split-cursor exactly-once-across-suspend all inherited PROVEN.
//   * the OPEN handshake inherits reply_cell.hpp's 015 seam: the on-lane `co_await` re-admit is
//     future work; `block_on` (off-lane) is wired. 006 outbound therefore stays DRAFT until the 015
//     OPEN-cell re-admit clears an ADR-014-grade real-scheduler gate — EXACTLY as an ordinary ask's
//     OPEN handshake still does. This header does NOT pretend to close that gate.
//
// x86-TSO ONLY. Every load-bearing order is inherited from the settled 024 ring + reply_cell; the
// AArch64 weak-memory re-gate defers with 024 (TODO(arm64) there).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <utility>

#include "quark/core/error.hpp"
#include "quark/core/stream_activation.hpp"
#include "quark/detail/reply_cell.hpp"

namespace quark {

// --- Terminal cause of an outbound reply stream (ADR-018 §three seams — in-band EoS + terminal) ---
// A stream is Open until exactly one terminal cause is latched. Closed = the callee finished normally
// (EoS); the other three are teardown causes. The caller's drain surfaces the latched cause AFTER the
// last in-flight item has been delivered (terminal never races ahead of a still-buffered item).
enum class ReplyStreamTerminal : std::uint8_t {
    Open = 0,
    Closed = 1,            // callee reached end-of-stream (in-band EoS) — the success terminal
    Cancelled = 2,         // caller tore the stream down (drop the ReplyStream / explicit cancel())
    DeadlineExceeded = 3,  // 018 deadline fired
    Failed = 4,            // callee failed mid-stream (carries an error)
};

// --- ReplyItem<F> — the wire/ring frame (ADR-018 seam 2: callee-assigned producer_seq identity) ---
// The ring carries ReplyItem<F>, not bare F, so the 017 identity `(stream_id, producer_seq)` travels
// WITH the item and survives a transport retransmit / re-activation (a caller-local ring index does
// not — ADR-018 C3). Trivially copyable iff F is (the 024 inline-slot precondition).
template <class F>
struct ReplyItem {
    std::uint64_t producer_seq;  // callee-assigned, stream-relative, replay-deterministic
    F payload;
};

// --- ReplyDedup — the caller-side exactly-once gate (017 / ADR-018 C3) --------------------------
// A monotone high-watermark over producer_seq. `accept(seq)` returns true exactly once per distinct
// seq delivered IN ORDER; a replay (seq < next) is a duplicate and is dropped; a forward gap (seq >
// next) under the 010 per-stream-FIFO precondition cannot occur locally and is reported so a caller
// can escalate. Single-consumer (the caller lane), so no atomics — this is lane-private state.
class ReplyDedup {
public:
    // true  -> deliver this item (fresh, in order); advances the watermark.
    // false -> a duplicate replay (seq already delivered); drop it.
    [[nodiscard]] bool accept(std::uint64_t seq) noexcept {
        if (seq < next_) return false;  // replay of an already-delivered item — dedup drop
        // seq >= next_. Under 010 per-stream FIFO the only in-order value is next_ itself; a strictly
        // greater seq is a genuine gap (a lost frame), surfaced via gap() for the caller to escalate.
        if (seq > next_) gap_ = true;
        next_ = seq + 1;
        return true;
    }
    [[nodiscard]] std::uint64_t watermark() const noexcept { return next_; }
    [[nodiscard]] bool gap() const noexcept { return gap_; }

private:
    std::uint64_t next_ = 0;
    bool gap_ = false;
};

// --- CrossNodeCredit — the 010 monotone-max-merge credit-return (ADR-018 seam / C5) -------------
// Cross-node, the caller's `tail` (credit) is shipped as an edge-triggered CreditReturn{stream_id,
// tail} carrying the ABSOLUTE tail. Applied `shadow_tail = max(shadow_tail, tail)`, it is idempotent
// under reorder and duplication (a stale/duplicated CreditReturn can only re-assert a tail the
// producer already saw — never regress it). An ADDITIVE credit-return (fetch_add) would over-credit
// under a duplicated packet and let the producer overwrite a live slot; max-merge cannot (ADR-018
// C5: additive control overshoots, max-merge overwrite_violations == 0).
class CrossNodeCredit {
public:
    // Apply a received CreditReturn. Returns true iff it advanced the shadow tail (a real edge — the
    // producer-side transport arms the ring's credit only on a true advance).
    bool apply(std::uint64_t remote_tail) noexcept {
        std::uint64_t cur = shadow_tail_.load(std::memory_order_acquire);
        while (remote_tail > cur) {
            if (shadow_tail_.compare_exchange_weak(cur, remote_tail, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                return true;
            // cur reloaded by CAS failure; loop (another apply raced — still monotone).
        }
        return false;  // stale / duplicate CreditReturn — no regression, no over-credit
    }
    [[nodiscard]] std::uint64_t shadow_tail() const noexcept {
        return shadow_tail_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> shadow_tail_{0};
};

// --- process-monotonic stream_id nonce (010: no ABA on the transport stream_id -> ring* map) -----
// A never-reused id for a reply stream, so a CreditReturn / late item addressed to a torn-down
// stream cannot land on a recycled ring (ADR-018 §010: stream_id is a process-monotonic nonce). One
// relaxed fetch_add per ask_stream (cold), never on the item path.
[[nodiscard]] inline std::uint64_t next_stream_id() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// The OPEN-handshake success tag. The StreamReplyCell resolves result<Opened> exactly once: Opened
// on accept (the ring is live, the caller may drain), or an error on reject/fail-before-open.
struct Opened {};

// ============================================================================================
// ReplyStreamState<F> — the shared control block for one outbound reply stream (ADR-018).
//
// Owns the caller-shard-allocated StreamChannel/StreamActivation ring (consumer-owned; ADR-018:
// "ring allocated on the CALLER shard") PLUS the three outbound seams' state: the terminal latch, the
// producer_seq counter (producer-owned single writer), the dedup watermark (consumer-owned), the OPEN
// cell, and a wrapper-level credit/terminal wake word. Reference-counted (shared_ptr) so the producer
// (callee) and consumer (caller) each hold a handle and the ring is reclaimed EXACTLY ONCE when the
// last handle drops — the multi-terminal UAF gate (ADR-018 GATE-4): neither side frees while the
// other may still touch the ring.
//
// The terminal wake is the wrapper's own reverse-Dekker (mirroring the ring's internal one, but
// spanning credit-return OR terminal): a callee stalled for credit in push_blocking() waits on
// `wake_gen_`; the caller bumps `wake_gen_` on credit-return AND on any terminal, so a teardown wakes
// a stalled callee immediately (two-part terminal wake — ADR-018 C2). 024 stays byte-for-byte
// unchanged: the terminal/seq/wake logic lives HERE, around the ring, never inside StreamChannel.
// ============================================================================================
template <class F>
class ReplyStreamState {
    static_assert(std::is_trivially_copyable_v<F>,
                  "ask_stream<F>: an inline reply frame must be trivially copyable (024 inline-slot "
                  "regime); a by-reference frame is the same declared 019/003 seam as inbound");

public:
    using Item = ReplyItem<F>;
    using Config = typename StreamChannel<Item>::Config;

    ReplyStreamState(const Config& cfg, std::pmr::memory_resource* mr)
        : stream_id_(next_stream_id()), act_(cfg, mr) {}

    ReplyStreamState(const ReplyStreamState&) = delete;
    ReplyStreamState& operator=(const ReplyStreamState&) = delete;

    [[nodiscard]] std::uint64_t stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] StreamActivation<Item>& activation() noexcept { return act_; }
    [[nodiscard]] StreamChannel<Item>& channel() noexcept { return act_.channel(); }

    // ---- Terminal latch (single-cause; the FIRST setter wins) -----------------------------------
    // A CAS from Open to the cause: exactly one terminal cause is ever latched, so close-vs-cancel and
    // deadline-vs-fail races resolve to one deterministic outcome (ADR-018 GATE-5: one terminal edge).
    // On the winning edge, do the wrapper half of the two-part wake so a stalled callee unblocks.
    bool latch_terminal(ReplyStreamTerminal cause, error e = error{}) noexcept {
        std::uint8_t expected = static_cast<std::uint8_t>(ReplyStreamTerminal::Open);
        if (terminal_.compare_exchange_strong(expected, static_cast<std::uint8_t>(cause),
                                              std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (cause == ReplyStreamTerminal::Failed) fail_error_ = e;
            wake_producer();  // two-part wake: a callee stalled in push_blocking() must observe teardown
            return true;
        }
        return false;  // already terminal — this cause loses to the latched one
    }
    [[nodiscard]] ReplyStreamTerminal terminal() const noexcept {
        return static_cast<ReplyStreamTerminal>(terminal_.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool is_open() const noexcept { return terminal() == ReplyStreamTerminal::Open; }
    [[nodiscard]] error fail_error() const noexcept { return fail_error_; }

    // ---- Producer seq (callee single writer) ----------------------------------------------------
    // Peek/advance are split so a producer only CONSUMES a seq on a SUCCESSFUL push: a credit-stalled
    // retry re-offers the SAME producer_seq (replay-deterministic identity; 017 / ADR-018 C3), and a
    // dropped-because-full attempt burns no seq (no gap on the caller's dedup watermark).
    [[nodiscard]] std::uint64_t peek_producer_seq() const noexcept { return producer_seq_; }
    void advance_producer_seq() noexcept { ++producer_seq_; }

    // ---- Wrapper reverse-Dekker credit/terminal wake --------------------------------------------
    // The callee half: after arming, capture the generation, re-check, then wait — the SAME ordering
    // the 024 ring uses internally (capture BEFORE the final check; a bump between check and wait
    // makes wait() return, so no lost wakeup — mailbox-closeout-head-race / stream-credit-gen memory).
    [[nodiscard]] std::uint32_t wake_generation() const noexcept {
        return wake_gen_.load(std::memory_order_acquire);
    }
    void wait_wake(std::uint32_t g) noexcept { wake_gen_.wait(g, std::memory_order_acquire); }
    // The caller half: called on a credit-return edge (batch boundary) AND on terminal — either
    // condition must wake a stalled callee.
    void wake_producer() noexcept {
        wake_gen_.fetch_add(1, std::memory_order_acq_rel);
        wake_gen_.notify_all();
    }

    // ---- OPEN handshake cell (reuses the settled reply_cell.hpp win-arbitration verbatim) --------
    [[nodiscard]] detail::ReplyCell<Opened>& open_cell() noexcept { return open_; }

private:
    const std::uint64_t stream_id_;
    StreamActivation<Item> act_;  // the caller-shard-owned 024 ring (flipped: callee produces here)

    std::atomic<std::uint8_t> terminal_{static_cast<std::uint8_t>(ReplyStreamTerminal::Open)};
    error fail_error_{};

    std::uint64_t producer_seq_ = 0;  // callee single-writer (producer lane)

    std::atomic<std::uint32_t> wake_gen_{0};  // wrapper credit/terminal wake word (reverse-Dekker)

    detail::ReplyCell<Opened> open_;  // single OPEN resolve (ADR-007 reply seam; scalar cell untouched)
};

// The push outcome (ADR-018 GATE-3: lossless — a full ring STALLS the producer, never drops).
enum class ReplyPush : std::uint8_t {
    Ok = 0,          // item accepted into the ring
    WouldStall = 1,  // credit depleted — the non-blocking try_push backed off (caller must retry/park)
    Terminated = 2,  // the stream is terminal (cancel / deadline / already closed) — stop producing
};

// ============================================================================================
// ReplyStreamProducer<F> — the CALLEE side. Handed to the handler by StreamResponder::accept(). Pushes
// reply items (assigning producer_seq), stalls losslessly on credit depletion, and latches the EoS /
// failure terminal. Move-only (single producer — the 024 single-writer token guards a second bind).
// ============================================================================================
template <class F>
class ReplyStreamProducer {
public:
    using Item = ReplyItem<F>;

    ReplyStreamProducer() noexcept = default;
    explicit ReplyStreamProducer(std::shared_ptr<ReplyStreamState<F>> st) noexcept : st_(std::move(st)) {}

    ReplyStreamProducer(const ReplyStreamProducer&) = delete;
    ReplyStreamProducer& operator=(const ReplyStreamProducer&) = delete;
    ReplyStreamProducer(ReplyStreamProducer&&) noexcept = default;
    ReplyStreamProducer& operator=(ReplyStreamProducer&&) noexcept = default;
    ~ReplyStreamProducer() {
        // If the handler drops the producer without an explicit close()/fail(), latch Closed so the
        // caller is never left hanging on an open stream (mirrors Responder's fire-default; ADR-018
        // GATE-5 — reply-before-teardown for the streaming case).
        if (st_ && st_->is_open()) st_->latch_terminal(ReplyStreamTerminal::Closed);
    }

    [[nodiscard]] bool valid() const noexcept { return st_ != nullptr; }
    [[nodiscard]] ReplyStreamTerminal terminal() const noexcept {
        return st_ ? st_->terminal() : ReplyStreamTerminal::Cancelled;
    }

    // Non-blocking, lossless. Ok on accept; WouldStall on credit depletion (retry later, same seq);
    // Terminated if the caller tore the stream down (stop producing — do NOT spin). A WouldStall burns
    // no producer_seq: the retry re-offers the same identity, so the caller's watermark sees no gap.
    [[nodiscard]] ReplyPush try_push(const F& payload) noexcept {
        if (!st_ || !st_->is_open()) return ReplyPush::Terminated;
        Item item{st_->peek_producer_seq(), payload};
        if (st_->activation().producer_push(item)) {
            st_->advance_producer_seq();  // consume the seq ONLY on a successful push
            return ReplyPush::Ok;
        }
        return ReplyPush::WouldStall;  // credit depleted — nothing consumed; retry
    }

    // Blocking, lossless (ADR-018 GATE-3). Stalls on credit depletion and waits on the wrapper's
    // reverse-Dekker wake — woken by a credit-return OR by a terminal (two-part terminal wake). Never
    // drops a frame. Returns Terminated if the caller cancelled/deadlined while we were stalled.
    [[nodiscard]] ReplyPush push(const F& payload) noexcept {
        if (!st_ || !st_->is_open()) return ReplyPush::Terminated;
        const Item item{st_->peek_producer_seq(), payload};  // same identity across every retry
        for (;;) {
            if (!st_->is_open()) return ReplyPush::Terminated;
            if (st_->activation().producer_push(item)) {
                st_->advance_producer_seq();  // consume the seq ONLY on success
                return ReplyPush::Ok;
            }
            // Capture the wake generation BEFORE the final credit/terminal re-check (load-bearing
            // ordering: a credit-return or terminal that bumps the gen after this load makes wait()
            // return immediately — no lost wakeup; see stream-credit-gen-lost-wakeup memory).
            const std::uint32_t g = st_->wake_generation();
            if (!st_->is_open()) return ReplyPush::Terminated;
            if (st_->channel().credit_available() > 0) continue;  // credit raced back — retry the push
            st_->wait_wake(g);  // sleep until a credit-return or a terminal wakes us
        }
    }

    // In-band EoS — the SUCCESS terminal. Idempotent; the caller's drain surfaces Closed after the
    // last buffered item.
    void close() noexcept {
        if (st_) st_->latch_terminal(ReplyStreamTerminal::Closed);
    }
    // Mid-stream failure terminal (the callee gives up). Carries the error to the caller.
    void fail(error e) noexcept {
        if (st_) st_->latch_terminal(ReplyStreamTerminal::Failed, e);
    }

private:
    std::shared_ptr<ReplyStreamState<F>> st_;
};

// ============================================================================================
// ReplyStream<F> — the CALLER side. The drain handle the caller receives from `co_await
// ask_stream<F>(...)`. Pulls items one batch per turn off the flipped ring (plain acquire-load +
// release-store — 0 cross-core RMW, ADR-018 F3), dedups by producer_seq (017), and surfaces the
// terminal cause after the last buffered item. Dropping it (or calling cancel()) tears the stream
// down (latches Cancelled + two-part wake); the shared state is reclaimed when the last handle drops.
// ============================================================================================
template <class F>
class ReplyStream {
public:
    using Item = ReplyItem<F>;

    ReplyStream() noexcept = default;
    explicit ReplyStream(std::shared_ptr<ReplyStreamState<F>> st) noexcept : st_(std::move(st)) {}

    ReplyStream(const ReplyStream&) = delete;
    ReplyStream& operator=(const ReplyStream&) = delete;
    ReplyStream(ReplyStream&&) noexcept = default;
    ReplyStream& operator=(ReplyStream&&) noexcept = default;
    ~ReplyStream() { cancel(); }  // drop == cancel: tear down, wake a stalled callee, reclaim once

    [[nodiscard]] bool valid() const noexcept { return st_ != nullptr; }

    // Pull the next reply item, or std::nullopt when none is buffered right now (drain to empty then
    // check done()). 0-RMW: peek + advance_dispatch (release) + advance_tail (release), then the
    // wrapper credit wake at the item boundary (a bounded, off-item-path notify). Dedup drops a
    // duplicate replay silently and pulls the next.
    [[nodiscard]] std::optional<F> next() noexcept {
        if (!st_) return std::nullopt;
        StreamChannel<Item>& ch = st_->channel();
        while (ch.occupancy() > 0) {
            const Item& it = ch.peek();
            const std::uint64_t seq = it.producer_seq;
            F payload = it.payload;      // copy out of the slot BEFORE returning credit
            ch.advance_dispatch();       // DISPATCH edge (release store) — 0 RMW
            ch.advance_tail();           // CREDIT-RETURN edge (release store) — 0 RMW
            st_->wake_producer();        // wrapper credit wake (off the RMW-free item copy; a bounded notify)
            ++delivered_raw_;
            if (dedup_.accept(seq)) return payload;  // fresh, in order -> deliver
            // else: duplicate replay -> dropped, pull the next
        }
        return std::nullopt;
    }

    // True once the stream is terminal AND fully drained (no buffered item remains). A caller loops
    // `while (auto x = rs.next()) …` then inspects terminal()/done().
    [[nodiscard]] bool done() const noexcept {
        return st_ && !st_->is_open() && st_->channel().occupancy() == 0;
    }
    [[nodiscard]] ReplyStreamTerminal terminal() const noexcept {
        return st_ ? st_->terminal() : ReplyStreamTerminal::Cancelled;
    }
    [[nodiscard]] error fail_error() const noexcept { return st_ ? st_->fail_error() : error{}; }
    [[nodiscard]] std::uint64_t stream_id() const noexcept { return st_ ? st_->stream_id() : 0; }
    [[nodiscard]] std::uint64_t watermark() const noexcept { return dedup_.watermark(); }
    [[nodiscard]] bool gap_detected() const noexcept { return dedup_.gap(); }

    // Caller-initiated teardown (018 deadline expiry maps here with DeadlineExceeded). Latches the
    // cause (first-wins) and wakes a stalled callee. Idempotent.
    void cancel(ReplyStreamTerminal cause = ReplyStreamTerminal::Cancelled) noexcept {
        if (st_) st_->latch_terminal(cause);
    }
    void expire_deadline() noexcept { cancel(ReplyStreamTerminal::DeadlineExceeded); }

private:
    std::shared_ptr<ReplyStreamState<F>> st_;
    ReplyDedup dedup_;
    std::uint64_t delivered_raw_ = 0;  // pre-dedup count (observability)
};

// ============================================================================================
// StreamResponder<F> — travels INSIDE the ask_stream envelope (the callee handler receives it, as
// Responder<R> rides an ordinary Ask<Q,R>). accept() binds the single producer, resolves the OPEN
// cell exactly once (ADR-007-ordered), and returns the ReplyStreamProducer; reject() fails OPEN. If
// the handler drops it without accepting, the destructor fails OPEN so the caller never hangs
// (reply-before-teardown, streaming flavor).
// ============================================================================================
template <class F>
class StreamResponder {
public:
    StreamResponder() noexcept = default;
    StreamResponder(std::shared_ptr<ReplyStreamState<F>> st, std::uint64_t open_gen) noexcept
        : st_(std::move(st)), open_gen_(open_gen), armed_(true) {}

    StreamResponder(const StreamResponder&) = delete;
    StreamResponder& operator=(const StreamResponder&) = delete;
    StreamResponder(StreamResponder&& o) noexcept
        : st_(std::move(o.st_)), open_gen_(o.open_gen_), armed_(o.armed_) {
        o.armed_ = false;
    }
    StreamResponder& operator=(StreamResponder&& o) noexcept {
        if (this != &o) {
            fire_default();
            st_ = std::move(o.st_);
            open_gen_ = o.open_gen_;
            armed_ = o.armed_;
            o.armed_ = false;
        }
        return *this;
    }
    ~StreamResponder() { fire_default(); }

    // Accept the stream: bind the single producer (the 024 single-writer token), resolve OPEN with
    // Opened{}, and hand the callee the producer. Returns the producer on success; a Terminated-only
    // producer if the stream was already torn down (a caller that cancelled before the callee ran).
    [[nodiscard]] ReplyStreamProducer<F> accept() noexcept {
        if (!armed_ || !st_) return ReplyStreamProducer<F>{};
        armed_ = false;
        // Bind the single producer token on the underlying ring (a second bind is a typed 007 error —
        // enforced by StreamActivation::bind_producer; here we hold the sole producer).
        (void)st_->activation().bind_producer();
        st_->open_cell().resolve(open_gen_, result<Opened>(Opened{}));  // OPEN resolves exactly once
        return ReplyStreamProducer<F>{st_};
    }

    // Reject before producing anything (e.g. authz/validation on the query). Fails OPEN with an error.
    void reject(error e) noexcept {
        if (!armed_ || !st_) return;
        armed_ = false;
        st_->latch_terminal(ReplyStreamTerminal::Failed, e);
        st_->open_cell().resolve(open_gen_, std::unexpected<error>(e));
    }

    [[nodiscard]] bool armed() const noexcept { return armed_; }

private:
    void fire_default() noexcept {
        if (armed_ && st_) {
            armed_ = false;
            const error e{errc::supervised_stop, "no_stream_reply"};
            st_->latch_terminal(ReplyStreamTerminal::Failed, e);
            st_->open_cell().resolve(open_gen_, std::unexpected<error>(e));  // caller's OPEN fails, no hang
        }
    }

    std::shared_ptr<ReplyStreamState<F>> st_;
    std::uint64_t open_gen_ = 0;
    bool armed_ = false;
};

// The ask_stream envelope the caller posts (mirrors Ask<Q,R>; the StreamResponder is the reply seam).
template <class Q, class F>
struct AskStream {
    Q query;
    StreamResponder<F> respond;
};

// ============================================================================================
// OpenStreamFuture<F> — the awaitable `ask_stream<F>` returns. Awaits the single OPEN resolve
// (co_await on-lane resume is the SAME 015 seam reply_cell.hpp documents — block_on is the wired
// off-lane drive); on Opened it yields the ReplyStream<F> drain handle, on error it propagates.
// ============================================================================================
template <class F>
class [[nodiscard]] OpenStreamFuture {
public:
    OpenStreamFuture() noexcept = default;
    explicit OpenStreamFuture(std::shared_ptr<ReplyStreamState<F>> st) noexcept : st_(std::move(st)) {}

    OpenStreamFuture(const OpenStreamFuture&) = delete;
    OpenStreamFuture& operator=(const OpenStreamFuture&) = delete;
    OpenStreamFuture(OpenStreamFuture&&) noexcept = default;
    OpenStreamFuture& operator=(OpenStreamFuture&&) noexcept = default;

    // ---- awaiter interface (co_await) — the on-lane re-admit is the 015 OPEN seam (see header) ----
    [[nodiscard]] bool await_ready() const noexcept { return st_->open_cell().ready(); }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        return st_->open_cell().suspend(h);
    }
    [[nodiscard]] result<ReplyStream<F>> await_resume() noexcept { return finish(); }

    // ---- off-lane blocking drive (block_on), wired verbatim like an ordinary ask ----
    [[nodiscard]] result<ReplyStream<F>> wait() noexcept {
        st_->open_cell().block_wait();
        return finish();
    }

private:
    result<ReplyStream<F>> finish() noexcept {
        result<Opened> o = st_->open_cell().take();
        if (!o) return std::unexpected<error>(o.error());
        return ReplyStream<F>{st_};  // hand the caller the drain handle over the ring it owns
    }
    std::shared_ptr<ReplyStreamState<F>> st_;
};

// ============================================================================================
// make_ask_stream<Q,F> — the std-only core `ask_stream<F>(M)` lowers onto. Allocates the caller-shard
// ring cold, builds the shared state, and returns {envelope-to-post, OpenStreamFuture}. The full
// `ActorRef<A>::ask_stream<F>(Q)` (identity resolution + transport binding) is the 006/010 addressing
// layer over this core — kept out of the ring so 024 stays the pure primitive (ADR-018 spec recs).
// ============================================================================================
template <class Q, class F>
struct AskStreamRequest {
    AskStream<Q, F> envelope;      // post this to the callee (carries the StreamResponder)
    OpenStreamFuture<F> future;    // co_await / block_on this for the ReplyStream<F>
    std::uint64_t stream_id = 0;   // the 010 nonce (transport keys CreditReturn / late items on it)
};

template <class Q, class F>
[[nodiscard]] AskStreamRequest<Q, F> make_ask_stream(
    Q query, std::pmr::memory_resource* mr,
    typename ReplyStreamState<F>::Config cfg = {}) {
    auto st = std::make_shared<ReplyStreamState<F>>(cfg, mr);
    const std::uint64_t open_gen = st->open_cell().arm();  // ready the OPEN cell for this ask
    const std::uint64_t sid = st->stream_id();
    AskStream<Q, F> env{std::move(query), StreamResponder<F>{st, open_gen}};
    OpenStreamFuture<F> fut{st};
    return AskStreamRequest<Q, F>{std::move(env), std::move(fut), sid};
}

// Off-lane bootstrap/edge drive: block until the OPEN resolves and return the ReplyStream (mirrors
// quark::block_on for a scalar ask). Asserts nothing about lanes here — the core is transport-agnostic.
template <class F>
[[nodiscard]] result<ReplyStream<F>> block_on_open(OpenStreamFuture<F> f) noexcept {
    return f.wait();
}

}  // namespace quark
