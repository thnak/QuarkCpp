// Implements 009-Observability §Dead-letter inspection — the 009-level dead-letter registry that
// plugs into the `DeadLetterSink` function-pointer seam already exposed by activation.hpp (007
// §Per-message outcome). A dead-letter (a message that cannot be delivered/handled) is both
// COUNTED (the shard `dead_letters` counter, 009 §Metrics) and RETAINED in a bounded per-shard ring
// of recent records `{actor, msg type, error, trace_id, t}` for inspection and optional replay.
//
// BOUNDED — SHED, DON'T BUFFER (ADR-009 S4 / 022). The ring has a FIXED capacity fixed at
// construction (cold). Under a flood the memory footprint never grows: it retains the most-recent
// `capacity` records and drops the rest (counted as `dropped`). The record path does 0 heap
// allocation (writes in place into the pre-sized buffer) and is single-writer on the lane, so it is
// TSan-clean without a lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "quark/core/activation.hpp"  // the DeadLetterSink seam + Descriptor + error
#include "quark/core/descriptor.hpp"
#include "quark/core/error.hpp"

namespace quark {

// One retained dead-letter record (009 §Dead-letter inspection). Trivially-copyable; captured from
// the descriptor at sink time (the descriptor is Completed-but-not-yet-reclaimed, so its metadata
// fields are still valid — see activation.hpp `dead_letter_and_reclaim`).
struct DeadLetterRecord {
    std::uint64_t owner = 0;       // owning actor key (from the sink ctx, if wired per-activation)
    std::uint64_t trace_id = 0;    // 009 trace correlation id
    MessageId message_id{};        // per-message id (006)
    std::uint16_t msg_slot = 0;    // dense dispatch slot (ADR-007) — the message-type discriminator
    error err{};                   // the dead-letter reason (007)
    std::int64_t t_ns = 0;         // steady-clock ns at record time
};

// A bounded per-shard ring of recent dead-letter records. Single-writer append; snapshot off-lane.
class DeadLetterRegistry {
public:
    explicit DeadLetterRegistry(std::size_t capacity = 256, std::uint64_t owner = 0)
        : buf_(capacity == 0 ? 1 : capacity), cap_(capacity == 0 ? 1 : capacity), owner_(owner) {}

    DeadLetterRegistry(const DeadLetterRegistry&) = delete;
    DeadLetterRegistry& operator=(const DeadLetterRegistry&) = delete;

    // Record a dead-letter from a descriptor + reason. Single-writer, 0 allocation. `now_ns` is the
    // 018/pal steady instant supplied by the caller (kept a parameter so this header stays clock-free
    // and testable; the umbrella observability.hpp provides the pal::now()-stamped convenience).
    void record(Descriptor* d, error e, std::int64_t now_ns = 0) noexcept {
        DeadLetterRecord r;
        r.owner = owner_;
        if (d != nullptr) {
            r.trace_id = d->trace_id;
            r.message_id = d->message_id;
            r.msg_slot = d->reserved;  // ADR-007 dense slot rides Descriptor::reserved
        }
        r.err = e;
        r.t_ns = now_ns;
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        buf_[static_cast<std::size_t>(h % cap_)] = r;
        head_.store(h + 1, std::memory_order_relaxed);  // single-writer, no atomic RMW
    }

    void set_owner(std::uint64_t owner) noexcept { owner_ = owner; }

    // --- Observers (off-lane scrape) ------------------------------------------------------------
    [[nodiscard]] std::uint64_t total() const noexcept { return head_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] std::size_t size() const noexcept {
        const std::uint64_t t = total();
        return t < cap_ ? static_cast<std::size_t>(t) : cap_;
    }
    // Records shed because the ring was full (bounded-under-flood evidence).
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        const std::uint64_t t = total();
        return t > cap_ ? t - cap_ : 0;
    }

    // Copy the retained records (oldest->newest) for inspection / replay.
    void snapshot(std::vector<DeadLetterRecord>& out) const {
        const std::uint64_t t = total();
        const std::size_t n = t < cap_ ? static_cast<std::size_t>(t) : cap_;
        out.clear();
        out.reserve(n);
        const std::uint64_t start = t < cap_ ? 0 : t - cap_;
        for (std::uint64_t i = start; i < t; ++i)
            out.push_back(buf_[static_cast<std::size_t>(i % cap_)]);
    }

    // --- The seam adapter: plug this registry into an Activation's DeadLetterSink -----------------
    // `act.set_dead_letter_sink(registry.as_sink())`. The thunk is statically noexcept (the seam's
    // contract) and records on the lane. Note: activation.hpp calls the sink WITHOUT a timestamp, so
    // records made through the seam carry t_ns = 0; use record(d, e, now_ns) directly to timestamp.
    [[nodiscard]] DeadLetterSink as_sink() noexcept { return DeadLetterSink{&sink_thunk, this}; }

private:
    static void sink_thunk(Descriptor* d, error e, void* ctx) noexcept {
        static_cast<DeadLetterRegistry*>(ctx)->record(d, e, 0);
    }

    std::vector<DeadLetterRecord> buf_;
    std::size_t cap_;
    std::uint64_t owner_;
    std::atomic<std::uint64_t> head_{0};
};

}  // namespace quark
