// Tests ADR-040's OverlappedMaterial<T> (overlapped_material.hpp) — the hot-reload primitive behind
// rotating TLS identity/trust roots without a coordinated cluster restart: "current + grace-windowed
// previous", mutex-guarded, monotonic generation.
//
// Deterministic part: rotate() then snapshot() at explicit now_ns values proves the grace-window
// boundary (inside window -> previous visible; past deadline -> absent, even with no further
// rotate()/evict_previous() call) and monotonic generation. evict_previous() proves the S5
// no-grace-window escape hatch.
//
// CONCURRENT part (S1's proven shape): 1 writer rotating repeatedly + 3 readers snapshotting in a
// tight loop (4 threads total — machine-safety cap), under ASan — 0 reports, and every snapshot a
// reader observes is internally consistent (current is never null; generation is monotonic from that
// reader's own point of view).
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/overlapped_material.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    // --- Deterministic: grace-window boundary + monotonic generation. -----------------------------
    {
        auto v1 = std::make_shared<const int>(1);
        OverlappedMaterial<int> m(v1);

        auto s0 = m.snapshot(/*now_ns=*/0);
        check(s0.current && *s0.current == 1, "initial current == 1", ok);
        check(s0.generation == 0, "initial generation == 0", ok);
        check(!s0.previous, "initial previous absent", ok);

        auto v2 = std::make_shared<const int>(2);
        m.rotate(v2, /*now_ns=*/1000, /*grace_ns=*/500);  // deadline = 1500
        check(m.generation() == 1, "generation bumped to 1 after rotate", ok);

        auto s_inside = m.snapshot(/*now_ns=*/1400);
        check(s_inside.current && *s_inside.current == 2, "current == 2 inside window", ok);
        check(s_inside.previous && *s_inside.previous == 1, "previous == 1 still visible inside window", ok);
        check(s_inside.previous_generation == 0, "previous_generation == 0 (pre-rotation gen)", ok);

        auto s_boundary = m.snapshot(/*now_ns=*/1500);
        check(!s_boundary.previous, "previous absent AT the deadline (strict <, not <=)", ok);

        auto s_past = m.snapshot(/*now_ns=*/9999);
        check(s_past.current && *s_past.current == 2, "current still 2 well past deadline", ok);
        check(!s_past.previous, "previous absent well past deadline, no extra call needed", ok);

        // A second rotation with grace_ns == 0: previous is immediately absent (S5 no-overlap case).
        auto v3 = std::make_shared<const int>(3);
        m.rotate(v3, /*now_ns=*/2000, /*grace_ns=*/0);
        check(m.generation() == 2, "generation bumped to 2", ok);
        auto s_zero_grace = m.snapshot(/*now_ns=*/2000);
        check(s_zero_grace.current && *s_zero_grace.current == 3, "current == 3", ok);
        check(!s_zero_grace.previous, "grace_ns=0 leaves no visible previous, even at the same instant", ok);

        // evict_previous(): drop an in-window previous immediately (compromise-during-rotation case).
        auto v4 = std::make_shared<const int>(4);
        m.rotate(v4, /*now_ns=*/3000, /*grace_ns=*/10'000);  // long window, deadline = 13000
        check(m.snapshot(3000).previous != nullptr, "previous visible right after rotate with a long grace", ok);
        m.evict_previous();
        check(!m.snapshot(3000).previous, "evict_previous() drops it immediately, ignoring the deadline", ok);
    }

    // --- Concurrent: 1 writer + 3 readers, ASan-clean, internally-consistent snapshots. ------------
    {
        auto v0 = std::make_shared<const std::uint64_t>(0);
        OverlappedMaterial<std::uint64_t> m(v0);
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> rotations{0};
        std::atomic<bool> reader_saw_null_current{false};
        std::atomic<bool> reader_saw_nonmonotonic{false};

        std::thread writer([&] {
            std::uint64_t next = 1;
            std::int64_t now_ns = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                m.rotate(std::make_shared<const std::uint64_t>(next), now_ns, /*grace_ns=*/50);
                ++next;
                now_ns += 10;  // deadline sweeps past on roughly every other rotation
                rotations.fetch_add(1, std::memory_order_relaxed);
            }
        });

        auto reader_fn = [&] {
            std::uint64_t last_gen = 0;
            std::int64_t now_ns = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto s = m.snapshot(now_ns);
                if (!s.current) reader_saw_null_current.store(true, std::memory_order_relaxed);
                if (s.generation < last_gen) reader_saw_nonmonotonic.store(true, std::memory_order_relaxed);
                last_gen = s.generation;
                ++now_ns;
            }
        };
        std::vector<std::thread> readers;
        for (int i = 0; i < 3; ++i) readers.emplace_back(reader_fn);  // 3 readers + 1 writer == 4 threads

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        for (auto& t : readers) t.join();

        check(rotations.load() > 1000, "writer completed many rotations under contention", ok);
        check(!reader_saw_null_current.load(), "no reader ever observed a null current", ok);
        check(!reader_saw_nonmonotonic.load(), "generation was monotonic from every reader's own view", ok);
    }

    std::printf("security_overlapped_material_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
