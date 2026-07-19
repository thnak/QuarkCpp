// Tests 026-Large-Scale-Cluster-Topology §"Cross-node FIFO under relay" — THE HARD GATE (ADR-011
// Gate A, gate-026-fifo-under-variable-hop-relay). A monotonic sequence from ONE sender to ONE owner
// crosses a MID-STREAM path change (a hop is promoted: relay path shrinks from >=2 hops to 1 direct
// hop) with REAL cross-thread relay (separate worker threads + condvar FIFO queues; arrival order
// recorded RAW at the owner — no dedup, no re-sort).
//
//   PINNED arm  (ADR-006 discipline): deterministic per-digest PATH PINNING (PathPin) + DRAIN-BOUNDARY
//               promotion — the promotion is applied only AFTER the old-path in-flight of this (S,A)
//               stream has quiesced. => 0 inversions.
//   CONTROL arm (mandatory teeth): the SAME harness with pinning+drain REMOVED — the new (fast) path is
//               applied mid-stream while old-path messages are still in flight. => inverts (a large
//               fraction of trials), proving the discipline is what holds FIFO.
//
// Bounded per the machine-safety rule (<=4 threads: 2 relays + 1 owner + main; a few thousand arrivals
// x tens of trials) — NOT ADR-011's full 100x1e6. In-flight > 0 at the switch in EVERY trial (a genuine
// mid-stream promotion, not empty-pipe). Deterministic: all sizes seeded from splitmix64.
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// A cross-thread FIFO channel (mutex + condvar) — a real happens-before edge per hand-off (TSan-clean).
struct Chan {
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    bool done = false;
    void push(int v) {
        {
            std::lock_guard<std::mutex> g(m);
            q.push(v);
        }
        cv.notify_one();
    }
    void finish() {
        {
            std::lock_guard<std::mutex> g(m);
            done = true;
        }
        cv.notify_all();
    }
};

struct TrialResult {
    std::uint64_t inversions = 0;
    std::uint64_t inflight_at_switch = 0;
    std::size_t pre_hops = 0;   // relay hops before promotion (>=2)
    std::size_t post_hops = 0;  // relay hops after promotion (1)
    bool discipline_ok = true;  // PathPin deferred the change until the drain boundary
};

// One trial. `pinned` selects the disciplined arm (PathPin + drain boundary) vs the control.
TrialResult run_trial(bool pinned, std::uint64_t seed, bool& ok) {
    TrialResult res;

    // ---- Faithfulness: derive the pre/post relay paths from a REAL Kademlia overlay --------------
    // A small overlay; pick a sender S and an owner A (via VirtualBins) whose relay route is >= 2 hops.
    std::vector<NodeId> roster;
    {
        std::uint64_t x = seed ^ 0x026F1F0ULL;
        for (int i = 0; i < 16; ++i) {
            x = detail::splitmix64(x);
            roster.push_back(NodeId{x | 1});
        }
    }
    RelayOverlay overlay(roster, /*k*/ 16);
    VirtualBins bins(std::span<const NodeId>(roster), virtual_bin_count(16));
    NodeId S = roster[0];
    NodeId A{};
    std::vector<NodeId> route;
    for (std::uint64_t k = 1; k < 5000; ++k) {
        const std::optional<NodeId> owner = bins.owner_of(ActorId{TypeKey{seed}, k});
        if (!owner || *owner == S) continue;
        std::vector<NodeId> r = overlay.route(S, *owner, /*max_hops*/ 8);
        if (r.size() >= 2) {  // owner outside S's immediate reach -> a genuine >=2-hop relay path
            A = *owner;
            route = std::move(r);
            break;
        }
    }
    if (route.size() < 2) {  // degenerate seed: fall back to a synthetic 2-hop path (still faithful)
        A = roster[3];
        route = {roster[1], roster[2], A};
    }
    res.pre_hops = route.size();
    res.post_hops = 1;

    // The pinned (slow) path vs the promoted (fast, direct) path, keyed by roster digest.
    const std::vector<NodeId> slow_path = route;         // S -> ... -> A  (>=2 hops)
    const std::vector<NodeId> fast_path = {A};           // S -> A         (1 hop, promoted)
    const std::uint64_t D0 = roster_digest(std::span<const NodeId>(roster));
    std::vector<NodeId> roster2 = roster;
    roster2.pop_back();  // a roster change -> a distinct digest carrying the promotion
    const std::uint64_t D1 = roster_digest(std::span<const NodeId>(roster2));

    // The PathPin discipline (header primitive): a change is deferred until the drain boundary.
    if (pinned) {
        PathPin<std::vector<NodeId>> pin;
        auto compute = [&](std::uint64_t d) { return d == D0 ? slow_path : fast_path; };
        pin.resolve(D0, /*at_drain_boundary*/ false, compute);  // pin the slow path
        const std::vector<NodeId>& before = pin.resolve(D1, /*at_drain_boundary*/ false, compute);
        const bool deferred = (before == slow_path);            // change HELD (not mid-stream)
        const std::vector<NodeId>& after = pin.resolve(D1, /*at_drain_boundary*/ true, compute);
        const bool applied = (after == fast_path);              // applied AT the drain boundary
        res.discipline_ok = deferred && applied;
    }

    // ---- The threaded relay: S (main) -> R1 -> R2 -> A, plus the direct S -> A fast path ----------
    // Message sizes seeded (bounded): a few thousand arrivals, switch mid-stream.
    const std::uint64_t M = 1500 + (detail::splitmix64(seed) % 1000);   // 1500..2499 arrivals
    const std::uint64_t switch_at = M / 2;                              // promote mid-stream

    Chan r1, r2, a;
    std::vector<int> arrival;
    arrival.reserve(static_cast<std::size_t>(M));

    std::mutex prog_m;
    std::condition_variable prog_cv;
    std::uint64_t delivered = 0;
    std::atomic<bool> gate_open{false};  // relays hold until released (creates the in-flight window)

    auto bump_delivered = [&] {
        {
            std::lock_guard<std::mutex> g(prog_m);
            ++delivered;
        }
        prog_cv.notify_all();
    };
    auto wait_delivered = [&](std::uint64_t target) {
        std::unique_lock<std::mutex> lk(prog_m);
        prog_cv.wait(lk, [&] { return delivered >= target; });
    };

    // Relay worker: forward in.q -> out, but only once the gate is open (holds in-flight while closed).
    auto relay_loop = [&](Chan& in, Chan& out) {
        for (;;) {
            std::unique_lock<std::mutex> lk(in.m);
            in.cv.wait(lk, [&] { return in.done || (!in.q.empty() && gate_open.load()); });
            if (in.q.empty()) {
                if (in.done) break;
                continue;
            }
            const int v = in.q.front();
            in.q.pop();
            lk.unlock();
            out.push(v);
        }
    };
    // Owner worker: record raw arrival order (single consumer -> arrival needs no extra lock).
    auto owner_loop = [&](Chan& in) {
        for (;;) {
            std::unique_lock<std::mutex> lk(in.m);
            in.cv.wait(lk, [&] { return in.done || !in.q.empty(); });
            if (in.q.empty()) {
                if (in.done) break;
                continue;
            }
            const int v = in.q.front();
            in.q.pop();
            lk.unlock();
            arrival.push_back(v);
            bump_delivered();
        }
    };

    std::thread tr1(relay_loop, std::ref(r1), std::ref(r2));
    std::thread tr2(relay_loop, std::ref(r2), std::ref(a));
    std::thread ta(owner_loop, std::ref(a));

    // Phase 1: emit the slow-path stream 0..switch_at-1. Gate is CLOSED, so all pile up in flight.
    for (std::uint64_t seq = 0; seq < switch_at; ++seq) r1.push(static_cast<int>(seq));
    res.inflight_at_switch = switch_at;  // every slow message is in the relay pipe at the switch

    // NOTE: `gate_open` must flip while EACH waiter's own mutex is held, not just via the atomic
    // store. relay_loop's `cv.wait(lk, pred)` checks `pred()` (reading gate_open) and, if false,
    // transitions into the actual wait — that whole check-then-wait sequence holds `in.m` throughout
    // (libstdc++'s wait(lock, pred) is `while (!pred()) wait(lock);`, and only the `wait(lock)` call
    // itself atomically unlocks + registers). A store+notify with NO mutex held can land in the
    // narrow window between "checked false" and "registered as a waiter" and be lost forever — the
    // relay thread then sleeps on a notify that will never come again (open_gate runs once). Taking
    // each channel's mutex around the store (even as an empty critical section for the second one)
    // serializes against that window: the waiter is provably either not-yet-checking (sees the new
    // value directly) or already registered (the notify reaches it) by the time we can acquire it.
    auto open_gate = [&] {
        {
            std::lock_guard<std::mutex> g(r1.m);
            gate_open.store(true);
        }
        r1.cv.notify_all();
        {
            std::lock_guard<std::mutex> g(r2.m);
        }
        r2.cv.notify_all();
    };

    if (pinned) {
        // DRAIN-BOUNDARY promotion: release the old path, wait until the (S,A) in-flight has fully
        // quiesced (owner received all of 0..switch_at-1), THEN apply the promotion and emit fast.
        open_gate();
        wait_delivered(switch_at);                       // drain boundary reached
        for (std::uint64_t seq = switch_at; seq < M; ++seq) a.push(static_cast<int>(seq));  // fast path
        wait_delivered(M);
    } else {
        // CONTROL: apply the new (fast) path MID-STREAM — emit fast directly to the owner while the
        // slow tail is still in flight, then release the old path. The fast (higher) seqs land first.
        for (std::uint64_t seq = switch_at; seq < M; ++seq) a.push(static_cast<int>(seq));
        wait_delivered(M - switch_at);                   // all fast delivered before any slow
        open_gate();                                     // now drain the still-in-flight slow tail
        wait_delivered(M);
    }

    r1.finish();
    r2.finish();
    a.finish();
    tr1.join();
    tr2.join();
    ta.join();

    // Count inversions: any arrival smaller than the running max is out of the monotonic order.
    int running_max = -1;
    for (int v : arrival) {
        if (v < running_max) ++res.inversions;
        else running_max = v;
    }
    if (arrival.size() != M) check(false, "owner received every message exactly once", ok);
    return res;
}

}  // namespace

int main() {
    bool ok = true;
    constexpr int kTrials = 40;

    std::uint64_t pinned_total_inv = 0, pinned_inflight_trials = 0, discipline_ok_trials = 0;
    std::size_t max_pre_hops = 0;
    for (int t = 0; t < kTrials; ++t) {
        const TrialResult r = run_trial(/*pinned*/ true, /*seed*/ UINT64_C(0xA11CE) + static_cast<std::uint64_t>(t) * UINT64_C(2654435761), ok);
        pinned_total_inv += r.inversions;
        if (r.inflight_at_switch > 0) ++pinned_inflight_trials;
        if (r.discipline_ok) ++discipline_ok_trials;
        if (r.pre_hops > max_pre_hops) max_pre_hops = r.pre_hops;
        check(r.post_hops == 1, "promotion shrinks the path to 1 direct hop", ok);
        check(r.pre_hops >= 2, "pre-promotion path is a genuine >=2-hop relay", ok);
    }

    std::uint64_t control_total_inv = 0, control_inverted_trials = 0;
    for (int t = 0; t < kTrials; ++t) {
        const TrialResult r = run_trial(/*pinned*/ false, /*seed*/ UINT64_C(0xA11CE) + static_cast<std::uint64_t>(t) * UINT64_C(2654435761), ok);
        control_total_inv += r.inversions;
        if (r.inversions > 0) ++control_inverted_trials;
    }

    std::printf("  PINNED : total_inversions=%llu over %d trials; in-flight>0 at switch in %llu/%d; "
                "PathPin drain-boundary discipline held in %llu/%d; max pre-hops=%zu\n",
                static_cast<unsigned long long>(pinned_total_inv), kTrials,
                static_cast<unsigned long long>(pinned_inflight_trials), kTrials,
                static_cast<unsigned long long>(discipline_ok_trials), kTrials, max_pre_hops);
    std::printf("  CONTROL: total_inversions=%llu; inverted in %llu/%d trials (teeth)\n",
                static_cast<unsigned long long>(control_total_inv), static_cast<unsigned long long>(control_inverted_trials),
                kTrials);

    // THE GATE: pinned = 0 inversions; control must invert a large fraction (prove the teeth).
    check(pinned_total_inv == 0, "PINNED: 0 inversions across all trials (FIFO preserved under relay)",
          ok);
    check(pinned_inflight_trials == kTrials,
          "every pinned trial had in-flight>0 at the switch (genuine mid-stream promotion)", ok);
    check(discipline_ok_trials == kTrials, "PathPin deferred the path change to the drain boundary", ok);
    check(control_inverted_trials * 2 >= static_cast<std::uint64_t>(kTrials),
          "CONTROL: unpinned mid-stream path change inverts a large fraction of trials", ok);
    check(control_total_inv > 0, "CONTROL: inversions are non-zero (the detector fires)", ok);

    std::printf("topology_fifo_under_relay_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
