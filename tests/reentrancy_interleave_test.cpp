// Tests 015-Reentrancy-and-Quiescence §The execution model — a reentrant actor interleaves ONLY at
// `co_await`. The synchronous regions between suspension points run mutually exclusively on the
// actor's lane, so a SHARED NON-ATOMIC field mutated in those regions is race-free even though many
// handlers are suspended concurrently and completed by MULTIPLE carrier threads.
//
// LOAD-BEARING UNDER TSan. One worker thread owns the lane (single-executor across async completion:
// carriers re-admit through the 015 gate, they never resume a frame inline). Several carrier threads
// concurrently complete in-flight async ops. If the design ever let a carrier run a synchronous
// region — or let two workers run one actor — TSan would flag the non-atomic `counter` writes.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/policies.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

constexpr int kJobs = 400;
constexpr std::size_t kCap = 4;

struct Job {
    int id;
};

struct Mixer : Actor<Mixer, MaxConcurrency<kCap>> {
    using protocol = Protocol<Job>;
    Activation* lane = nullptr;

    // NON-ATOMIC actor state, mutated ONLY inside synchronous regions. Race-free iff those regions
    // never overlap — the whole point of the "interleave only at co_await" model.
    long counter = 0;
    long witness = 0;

    std::atomic<int> done{0};

    task<> handle(const Job& j) {
        // --- synchronous region A (lane-exclusive) ---
        counter += 1;
        witness = counter + j.id;  // read + write of non-atomic state

        co_await lane->async_suspend();  // <-- the ONLY interleave point

        // --- synchronous region B (lane-exclusive) ---
        counter += 1;
        witness = counter - j.id;
        done.fetch_add(1, std::memory_order_release);
        co_return;
    }
};

}  // namespace

int main() {
    bool ok = true;
    Mixer actor;
    Activation act{&actor, Mixer::dispatch_table(), {}, max_concurrency_of<Mixer>()};
    actor.lane = &act;

    // Enqueue all jobs up front (mailbox FIFO), then run the lane + carriers concurrently.
    std::vector<Job> jobs(static_cast<std::size_t>(kJobs));
    std::vector<Descriptor> descs(static_cast<std::size_t>(kJobs));
    for (std::size_t i = 0; i < static_cast<std::size_t>(kJobs); ++i) {
        jobs[i].id = static_cast<int>(i);
        descs[i].payload = &jobs[i];
        stamp<Mixer, Job>(descs[i]);
        act.post(&descs[i]);
    }

    std::atomic<bool> carriers_run{true};

    // The SINGLE lane worker — the only thread that ever starts/resumes a frame (touches actor state).
    std::thread worker([&] {
        while (actor.done.load(std::memory_order_acquire) < kJobs) {
            drive(act);
            std::this_thread::yield();
        }
        drive(act);  // flush any final completion + close out to Idle
    });

    // Carrier threads — complete in-flight async ops (re-admit through the 015 gate). They NEVER
    // touch actor state; they only hand frames back to the lane.
    std::vector<std::thread> carriers;
    for (int c = 0; c < 3; ++c) {
        carriers.emplace_back([&] {
            while (carriers_run.load(std::memory_order_acquire)) {
                act.complete_one();
                std::this_thread::yield();
            }
        });
    }

    worker.join();
    carriers_run.store(false, std::memory_order_release);
    for (auto& t : carriers) t.join();

    // Final flush on the main thread (single-executor: the workers are gone).
    while (act.in_flight() > 0 || act.state() != ExecState::Idle) {
        if (act.complete_one()) { /* re-admitted */
        }
        if (drive(act) == DriveEnd::Idle && act.in_flight() == 0) break;
    }

    check(actor.done.load() == kJobs, "every handler completed", ok);
    check(actor.counter == 2 * kJobs, "non-atomic counter == 2 per job (no lost update ⇒ no race)", ok);
    check(act.max_in_flight() >= 2, "async regions DID interleave (concurrency > 1 observed)", ok);
    check(act.max_in_flight() <= kCap, "in-flight never exceeded the cap", ok);
    check(act.in_flight() == 0, "in-flight set drained to empty", ok);

    std::printf("reentrancy_interleave_test: %s  (done=%d counter=%ld max_in_flight=%zu)\n",
                ok ? "OK" : "FAIL", actor.done.load(), actor.counter, act.max_in_flight());
    return ok ? 0 : 1;
}
