// Implements 009-Observability §Metrics — per-shard, contention-free counters + fixed-bucket
// histograms, aggregate-on-scrape. The measurement surface for the 023 macrobenchmarks.
//
// HOT-PATH MODEL (CONVENTIONS.md hot-path rules; 009 §Storage: per-shard, contention-free):
//   * A shard is DRAINED by ONE worker at a time (002), so the counters that only change on the
//     drain-owner's lane (messages_processed, restarts, dead_letters, steals) are single-writer.
//   * The spec calls for "plain (non-atomic) integers" written on the lane and read by a scraper
//     "with a relaxed atomic snapshot". A plain non-atomic store racing an off-lane read is a data
//     race (UB, and TSan flags it), so we store each counter as std::atomic<uint64_t> and INCREMENT
//     with a relaxed load+store — `v.store(v.load(relaxed)+n, relaxed)`. At the ISA level this is a
//     `mov`/`mov` pair, NOT a `lock xadd`: there is no atomic RMW and no cross-core bus lock on the
//     hot path (the CONVENTIONS.md "0 cross-core atomic RMW on the sequential drain path" rule),
//     yet the access is well-defined and TSan-clean because it is a relaxed atomic, not a plain int.
//   * NOT every counter is drain-owned, though: mailbox_enqueued/activations/wakeups fire on the
//     PRODUCER side (Activation::post/notify_enqueued and Engine::schedule_and_wake), and more than
//     one producer thread can target the same shard concurrently (many actors hash to one shard).
//     `inc()`'s load+store pair is a lost-update race under concurrent writers there — no UB, no
//     TSan report (each op is individually a well-defined atomic access), just silent undercounting.
//     Those three use `inc_atomic()` (a real `fetch_add`) instead — precedented by the existing
//     producer-side `fetch_add` governance accounting in activation.hpp (GovernanceCore::enqueued).
//   * The scraper reads with a relaxed load and AGGREGATES across shards on read — the only
//     cross-thread interaction, and it is off the hot path.
//   * Every counter/histogram is a fixed-size member: 0 heap allocation on the record path.
//
// Because drain-side increments are NOT atomic RMWs, those Counters have exactly one writer (the
// shard's drain-owner lane). Producer-side counters use `inc_atomic()` and may have many writers.
// The scraper sums across shards (009 §aggregate-on-read).
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quark/core/config.hpp"

namespace quark {

// Number of user-defined counter slots per shard (009 OQ: dynamic vs compile-time slots — we take
// the fixed-slot side so the record path never allocates or locks; names are bound cold at startup).
inline constexpr std::size_t kUserCounterSlots = 16;

// --- MetricCounter: single-writer, relaxed. inc() is load+store (NOT an atomic RMW). -----------
class MetricCounter {
public:
    MetricCounter() noexcept = default;
    MetricCounter(const MetricCounter&) = delete;
    MetricCounter& operator=(const MetricCounter&) = delete;

    QUARK_ALWAYS_INLINE void inc(std::uint64_t n = 1) noexcept {
        v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    // Genuinely atomic increment (fetch_add) for the rare PRODUCER-side counters that may have more
    // than one concurrent writer on the same shard (mailbox_enqueued, activations, wakeups) — see the
    // file banner. NOT for the drain-owner-exclusive counters; those stay on the faster inc() above.
    QUARK_ALWAYS_INLINE void inc_atomic(std::uint64_t n = 1) noexcept {
        v_.fetch_add(n, std::memory_order_relaxed);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint64_t load() const noexcept {
        return v_.load(std::memory_order_relaxed);
    }
    void store(std::uint64_t x) noexcept { v_.store(x, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> v_{0};
};

// --- Histogram: fixed base-2 buckets (HDR-style), no allocation, mergeable on scrape ----------
// Bucket i counts values whose position of the highest set bit is i (bucket 0 == value 0). 64
// buckets span the whole uint64 range; recording is single-writer relaxed like a MetricCounter.
struct HistogramSnapshot {
    static constexpr std::size_t kBuckets = 64;
    std::array<std::uint64_t, kBuckets> buckets{};
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint64_t min = 0;
    std::uint64_t max = 0;

    void merge(const HistogramSnapshot& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) buckets[i] += o.buckets[i];
        if (o.count != 0) {
            if (count == 0 || o.min < min) min = o.min;
            if (o.max > max) max = o.max;
        }
        count += o.count;
        sum += o.sum;
    }
};

class Histogram {
public:
    static constexpr std::size_t kBuckets = HistogramSnapshot::kBuckets;

    Histogram() noexcept = default;
    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;

    QUARK_ALWAYS_INLINE void record(std::uint64_t v) noexcept {
        std::size_t b = bucket_of(v);
        add(buckets_[b], 1);
        add(count_, 1);
        add(sum_, v);
        const std::uint64_t mn = min_.load(std::memory_order_relaxed);
        if (v < mn) min_.store(v, std::memory_order_relaxed);
        const std::uint64_t mx = max_.load(std::memory_order_relaxed);
        if (v > mx) max_.store(v, std::memory_order_relaxed);
    }

    [[nodiscard]] HistogramSnapshot snapshot() const noexcept {
        HistogramSnapshot s;
        for (std::size_t i = 0; i < kBuckets; ++i)
            s.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
        s.count = count_.load(std::memory_order_relaxed);
        s.sum = sum_.load(std::memory_order_relaxed);
        const std::uint64_t mn = min_.load(std::memory_order_relaxed);
        s.min = (s.count == 0) ? 0 : mn;
        s.max = max_.load(std::memory_order_relaxed);
        return s;
    }

    [[nodiscard]] static std::size_t bucket_of(std::uint64_t v) noexcept {
        if (v == 0) return 0;
        std::size_t b = static_cast<std::size_t>(64 - std::countl_zero(v));
        return b >= kBuckets ? kBuckets - 1 : b;
    }

private:
    static QUARK_ALWAYS_INLINE void add(std::atomic<std::uint64_t>& a, std::uint64_t n) noexcept {
        a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_{0};
    std::atomic<std::uint64_t> min_{~std::uint64_t{0}};
    std::atomic<std::uint64_t> max_{0};
};

// --- ShardCounters: the per-shard block (009 §Storage). Cache-aligned so two shards' counters ---
// never share a line (023 false-sharing avoidance). Named engine hot events + user slots + two
// histograms. Non-copyable/non-movable (holds atomics) — owned in place, referenced by pointer.
struct QUARK_CACHE_ALIGNED ShardCounters {
    MetricCounter messages_processed;  // 001/002 drain
    MetricCounter mailbox_enqueued;    // 003 producer post
    MetricCounter activations;         // 002 Idle->Scheduled edges
    MetricCounter restarts;            // 007 supervision restarts
    MetricCounter steals;              // 002 work-stealing
    MetricCounter wakeups;             // 002 worker wakeups
    MetricCounter dead_letters;        // 007/009 undeliverable messages
    MetricCounter deadline_misses;     // 011/018 overruns accounted by 009
    MetricCounter user[kUserCounterSlots];

    Histogram message_latency_ns;  // handler start->end latency
    Histogram mailbox_depth;       // observed mailbox depth at drain

    ShardCounters() = default;
    ShardCounters(const ShardCounters&) = delete;
    ShardCounters& operator=(const ShardCounters&) = delete;
};

// --- Aggregated snapshot (plain ints — trivially copyable; safe to hand to tests/embedders) -----
struct MetricsSnapshot {
    std::uint64_t messages_processed = 0;
    std::uint64_t mailbox_enqueued = 0;
    std::uint64_t activations = 0;
    std::uint64_t restarts = 0;
    std::uint64_t steals = 0;
    std::uint64_t wakeups = 0;
    std::uint64_t dead_letters = 0;
    std::uint64_t deadline_misses = 0;
    std::array<std::uint64_t, kUserCounterSlots> user{};
    HistogramSnapshot message_latency_ns{};
    HistogramSnapshot mailbox_depth{};
};

// --- MetricsSink seam (009 §Export). The default is the in-memory snapshot below; heavy backends
// (Prometheus client, OTLP) are adapters over this — never linked into the core. -----------------
struct MetricsSink {
    void (*fn)(const MetricsSnapshot&, void* ctx) = nullptr;
    void* ctx = nullptr;
    void operator()(const MetricsSnapshot& s) const {
        if (fn) fn(s, ctx);
    }
};

// --- MetricsRegistry: the aggregate-on-scrape surface. Holds pointers to the per-shard blocks
// (registered cold at startup; the engine owns the shards, or the registry can own them for tests
// and embedders). snapshot()/to_prometheus() are OFF the hot path. --------------------------------
class MetricsRegistry {
public:
    MetricsRegistry() = default;
    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    // Register an externally-owned shard block (e.g. the engine's per-worker counters).
    void register_shard(ShardCounters& sc) { shards_.push_back(&sc); }

    // Own a shard block (tests / embedders). Cold allocation at startup — not the hot path.
    [[nodiscard]] ShardCounters& add_shard() {
        owned_.push_back(std::make_unique<ShardCounters>());
        shards_.push_back(owned_.back().get());
        return *owned_.back();
    }

    void set_user_counter_name(std::size_t slot, std::string name) {
        if (slot < kUserCounterSlots) user_names_[slot] = std::move(name);
    }

    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

    [[nodiscard]] MetricsSnapshot snapshot() const {
        MetricsSnapshot s;
        for (const ShardCounters* sc : shards_) {
            s.messages_processed += sc->messages_processed.load();
            s.mailbox_enqueued += sc->mailbox_enqueued.load();
            s.activations += sc->activations.load();
            s.restarts += sc->restarts.load();
            s.steals += sc->steals.load();
            s.wakeups += sc->wakeups.load();
            s.dead_letters += sc->dead_letters.load();
            s.deadline_misses += sc->deadline_misses.load();
            for (std::size_t i = 0; i < kUserCounterSlots; ++i) s.user[i] += sc->user[i].load();
            s.message_latency_ns.merge(sc->message_latency_ns.snapshot());
            s.mailbox_depth.merge(sc->mailbox_depth.snapshot());
        }
        return s;
    }

    // 009 §Export — Prometheus text exposition. Pure string formatting over the snapshot; NO
    // Prometheus client library. Off the hot path (allocates the result string).
    [[nodiscard]] std::string to_prometheus() const {
        const MetricsSnapshot s = snapshot();
        std::string out;
        auto counter = [&](const char* name, std::uint64_t v) {
            out += "# TYPE quark_";
            out += name;
            out += " counter\nquark_";
            out += name;
            out += ' ';
            out += std::to_string(v);
            out += '\n';
        };
        counter("messages_processed_total", s.messages_processed);
        counter("mailbox_enqueued_total", s.mailbox_enqueued);
        counter("activations_total", s.activations);
        counter("restarts_total", s.restarts);
        counter("steals_total", s.steals);
        counter("wakeups_total", s.wakeups);
        counter("dead_letters_total", s.dead_letters);
        counter("deadline_misses_total", s.deadline_misses);
        for (std::size_t i = 0; i < kUserCounterSlots; ++i) {
            if (s.user[i] == 0 && user_names_[i].empty()) continue;
            const std::string& nm = user_names_[i];
            out += "quark_user_counter{slot=\"";
            out += std::to_string(i);
            if (!nm.empty()) {
                out += "\",name=\"";
                out += nm;
            }
            out += "\"} ";
            out += std::to_string(s.user[i]);
            out += '\n';
        }
        histogram(out, "message_latency_ns", s.message_latency_ns);
        histogram(out, "mailbox_depth", s.mailbox_depth);
        return out;
    }

private:
    static void histogram(std::string& out, const char* name, const HistogramSnapshot& h) {
        out += "# TYPE quark_";
        out += name;
        out += " histogram\n";
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < HistogramSnapshot::kBuckets; ++i) {
            cumulative += h.buckets[i];
            if (h.buckets[i] == 0) continue;
            // le = upper bound of bucket i (2^i - 1), a base-2 HDR approximation. i < kBuckets (64).
            const std::uint64_t le = (i == 0) ? std::uint64_t{0} : ((std::uint64_t{1} << i) - 1);
            out += "quark_";
            out += name;
            out += "_bucket{le=\"";
            out += std::to_string(le);
            out += "\"} ";
            out += std::to_string(cumulative);
            out += '\n';
        }
        out += "quark_";
        out += name;
        out += "_sum ";
        out += std::to_string(h.sum);
        out += "\nquark_";
        out += name;
        out += "_count ";
        out += std::to_string(h.count);
        out += '\n';
    }

    std::vector<ShardCounters*> shards_;
    std::vector<std::unique_ptr<ShardCounters>> owned_;
    std::array<std::string, kUserCounterSlots> user_names_{};
};

}  // namespace quark
