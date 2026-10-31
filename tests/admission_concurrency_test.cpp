// Tests ADR-069 claim C8 / 020-Security §1a: publish() racing with lookups and with live delivery.
// STRESS test (not TSan): the local toolchain is MSVC, which has no ThreadSanitizer; the same file is
// intended for the repo's TSan build on Linux. Threads: 3 senders + 1 publisher + 1 reader (<= 4 busy).
// Properties checked on every delivered frame (all must hold, 0 violations):
//   P1 its admission generation's verdict was Admit (here: odd generations admit, even deny);
//   P2 it was not delivered AFTER publish(g) had returned for a g newer than its generation (the fence:
//      when publish returns, nothing admitted under an older generation is still being delivered);
//   P3 delivered + discarded == sent (nothing lost, nothing double counted).
// Reader threads additionally check lookups are consistent with the snapshot's own generation and that the
// observed generation never goes backwards. CONTROL: with the admission gate removed the same run shows
// P1 violations (frames delivered under even/zero generations), so the checks can see a failure.
#include <atomic>
#include <cstdio>
#include <thread>

#include "admission_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Result {
    std::uint64_t sent = 0, delivered = 0, discarded = 0;
    std::uint64_t p1_violations = 0, p2_violations = 0, reader_violations = 0;
};

Result run(bool gate_removed, bool& ok) {
    constexpr int kSenders = 3;
    constexpr int kFramesPerSender = 3000;
    constexpr std::uint64_t kLastGen = 301;  // odd => admit
    const NodeId n1{1}, n2{2};
    AdmissionAuthority auth;  // declared before the transport that listens to it
    LoopbackFabric fabric;
    LoopbackTransport l1(fabric, n1), l2(fabric, n2);
    MockCipher cipher(0x77);
    SecureTransport s1(l1, cipher, n1), s2(l2, cipher, n2);  // no handshake: sessions auto-vivify
    s2.set_admission(auth);
    if (gate_removed) s2.set_admission_test_faults({.skip_inbound_gate = true});
    (void)auth.publish(snap(1, {n1}));

    std::atomic<std::uint64_t> returned{1};  // newest generation whose publish() has returned
    std::atomic<std::uint64_t> delivered{0}, p1{0}, p2{0}, reader_bad{0};
    s2.on_receive([&](MessageFrame f) {
        const std::uint64_t floor = returned.load(std::memory_order_acquire);
        delivered.fetch_add(1, std::memory_order_relaxed);
        if (f.admission_generation % 2 == 0) p1.fetch_add(1, std::memory_order_relaxed);  // even/0: never Admit
        if (f.admission_generation < floor) p2.fetch_add(1, std::memory_order_relaxed);
    });
    s1.on_receive([](MessageFrame) {});

    std::atomic<bool> stop{false};
    std::thread publisher([&] {
        for (std::uint64_t g = 2; g <= kLastGen; ++g) {
            const bool admit = g % 2 == 1;
            (void)auth.publish(admit ? snap(g, {n1}) : snap(g, {}));
            returned.store(g, std::memory_order_release);
            std::this_thread::yield();
        }
    });
    std::thread reader([&] {
        std::uint64_t last = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const auto s = auth.current();
            const bool want_admit = s->generation() % 2 == 1;
            if (s->generation() < last) reader_bad.fetch_add(1, std::memory_order_relaxed);
            if ((s->lookup(n1) == AdmissionVerdict::Admit) != want_admit) reader_bad.fetch_add(1, std::memory_order_relaxed);
            last = s->generation();
        }
    });
    std::vector<std::thread> senders;
    for (int t = 0; t < kSenders; ++t)
        senders.emplace_back([&] {
            for (int i = 0; i < kFramesPerSender; ++i) {
                s1.send(n2, make_frame(n1, n2));
                if (i % 64 == 0) std::this_thread::yield();
            }
        });
    for (auto& t : senders) t.join();
    publisher.join();
    stop.store(true, std::memory_order_release);
    reader.join();

    Result r;
    r.sent = static_cast<std::uint64_t>(kSenders) * kFramesPerSender;
    r.delivered = delivered.load();
    r.discarded = s2.admission_stats().inbound_discarded;
    r.p1_violations = p1.load();
    r.p2_violations = p2.load();
    r.reader_violations = reader_bad.load();
    if (!gate_removed) {
        check(r.p1_violations == 0, "T8: no frame delivered under a generation that denied the peer", ok);
        check(r.p2_violations == 0, "T8: nothing admitted under an older generation delivered after publish returned", ok);
        check(r.delivered + r.discarded == r.sent, "T8: delivered + discarded == sent", ok);
        check(r.reader_violations == 0, "T8: concurrent lookups consistent, generation monotonic", ok);
        check(r.delivered > 0 && r.discarded > 0, "T8: the run exercised both admit and deny phases", ok);
        check(auth.generation() == kLastGen, "T8: final generation published", ok);
    }
    return r;
}

}  // namespace

int main() {
    bool ok = true;
    const Result real = run(false, ok);
    const Result control = run(true, ok);
    check(control.p1_violations > 0, "T8 CONTROL(no gate): delivery under non-admitting generations is detected", ok);
    std::printf("  real: sent=%llu delivered=%llu discarded=%llu p1=%llu p2=%llu | control p1=%llu\n",
                static_cast<unsigned long long>(real.sent), static_cast<unsigned long long>(real.delivered),
                static_cast<unsigned long long>(real.discarded), static_cast<unsigned long long>(real.p1_violations),
                static_cast<unsigned long long>(real.p2_violations),
                static_cast<unsigned long long>(control.p1_violations));
    std::printf("admission_concurrency_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
