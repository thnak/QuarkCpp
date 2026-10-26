// Regression tests for the SecureTransport / PeerSession concurrency + nonce-uniqueness review items
// (REVIEW-BACKLOG D1, D3, D4, D6, A9, B12). Implements 020-Security §2 / ADR-040 hardening.
//
// Several of these are DATA-RACE / LOCK-RETENTION bugs: the deterministic ones (D1, D3, D6, A9, B12) fail
// on the pre-fix headers in any build; the pure data-race ones (D4) are caught by TSan (the suite's
// sanitizer run), and also run here as a stress so a regression shows up under -fsanitize=thread.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/audit.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/peer_session.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"

using namespace quark;
using namespace std::chrono_literals;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// Records outbound frames; captures the (first) inbound callback so tests can inject frames from any thread.
class RecordingTransport final : public Transport {
public:
    void send(NodeId to, MessageFrame frame) override {
        std::lock_guard<std::mutex> g(mu_);
        sent_.emplace_back(to, std::move(frame));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override {
        std::lock_guard<std::mutex> g(mu_);
        if (!cb_) cb_ = std::move(cb);  // later registrations ignored: tests inject through the first one
    }
    void inject(MessageFrame f) {
        std::function<void(MessageFrame)> c;
        {
            std::lock_guard<std::mutex> g(mu_);
            c = cb_;
        }
        c(std::move(f));
    }
    [[nodiscard]] std::size_t sent_count() {
        std::lock_guard<std::mutex> g(mu_);
        return sent_.size();
    }
    [[nodiscard]] MessageFrame sent_frame(std::size_t i) {
        std::lock_guard<std::mutex> g(mu_);
        return sent_[i].second;
    }

private:
    std::mutex mu_;
    std::function<void(MessageFrame)> cb_;
    std::vector<std::pair<NodeId, MessageFrame>> sent_;
};

[[nodiscard]] std::uint64_t seq_of(const MessageFrame& f) { return detail::get_u64_le(f.payload); }

[[nodiscard]] MessageFrame data_frame(NodeId from, NodeId to, std::byte tag = std::byte{9}) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    f.payload = {tag};
    return f;
}

// An AEAD that parks seal() for plaintexts tagged 0xEE until released — models "a long seal".
class GateCipher final : public Aead {
public:
    void seal(std::uint64_t nonce, std::span<const std::byte> aad, std::span<const std::byte> pt,
              std::vector<std::byte>& out) const override {
        if (!pt.empty() && pt[0] == std::byte{0xEE}) {
            std::unique_lock<std::mutex> lk(m_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lk, [&] { return release_; });
        }
        inner_.seal(nonce, aad, pt, out);
    }
    [[nodiscard]] bool open(std::uint64_t n, std::span<const std::byte> aad, std::span<const std::byte> s,
                            std::vector<std::byte>& out) const override {
        return inner_.open(n, aad, s, out);
    }
    [[nodiscard]] std::size_t tag_size() const noexcept override { return inner_.tag_size(); }
    [[nodiscard]] std::size_t seal_into(std::uint64_t n, std::span<const std::byte> aad,
                                        std::span<const std::byte> pt,
                                        std::span<std::byte> out) const override {
        return inner_.seal_into(n, aad, pt, out);
    }
    [[nodiscard]] bool open_into(std::uint64_t n, std::span<const std::byte> aad,
                                 std::span<const std::byte> s, std::span<std::byte> out,
                                 std::size_t& out_len) const override {
        return inner_.open_into(n, aad, s, out, out_len);
    }
    void wait_entered() const {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return entered_; });
    }
    void release() const {
        std::lock_guard<std::mutex> g(m_);
        release_ = true;
        cv_.notify_all();
    }

private:
    MockCipher inner_{9};
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    mutable bool entered_ = false;
    mutable bool release_ = false;
};

// Handshake engine that parks forever (WantRead); optionally runs a probe from its destructor.
struct ParkedEngine final : HandshakeEngine {
    std::function<void()> on_destroy;
    ~ParkedEngine() override {
        if (on_destroy) on_destroy();
    }
    Step advance(std::span<const std::byte>, std::vector<std::byte>&) override { return Step::WantRead; }
    Result take_result() override { return {}; }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return {}; }
};

struct ParkedFactory final : HandshakeEngineFactory {
    std::chrono::microseconds create_delay{0};
    std::function<void()> on_destroy;
    std::unique_ptr<HandshakeEngine> create(bool, NodeId, NodeId, ClusterId, IdentityMaterial::Snapshot,
                                            TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        if (create_delay.count() > 0) std::this_thread::sleep_for(create_delay);
        auto e = std::make_unique<ParkedEngine>();
        e->on_destroy = on_destroy;
        return e;
    }
};

[[nodiscard]] MessageFrame auth_frame(NodeId from, NodeId to) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Authenticate;
    f.payload = {std::byte{1}, std::byte{2}};
    return f;
}

// --- B12 -------------------------------------------------------------------------------------------
bool test_b12_seq_monotonic_across_reconnect() {
    bool ok = true;
    const NodeId A{1}, B{2};
    RecordingTransport ia, ib;
    MockCipher cipher(7);
    SecureTransport a(ia, cipher, A), b(ib, cipher, B);
    int delivered = 0;
    b.on_receive([&](MessageFrame) { ++delivered; });
    a.on_receive([](MessageFrame) {});

    a.send(B, data_frame(A, B));
    a.send(B, data_frame(A, B));
    a.on_peer_disconnected(B);  // reconnect: handshake disabled => shared cipher, no new key
    a.send(B, data_frame(A, B));
    check(ia.sent_count() == 3, "three frames sealed", ok);
    const std::uint64_t s0 = seq_of(ia.sent_frame(0)), s1 = seq_of(ia.sent_frame(1)),
                        s2 = seq_of(ia.sent_frame(2));
    check(s0 != s2 && s1 != s2, "B12: no (key, nonce) reuse across a reconnect", ok);
    check(s2 > s1 && s1 > s0, "B12: per-peer send seq stays strictly increasing across a reconnect", ok);

    // The receive side's replay window must survive the reconnect too.
    ib.inject(ia.sent_frame(0));
    check(delivered == 1, "first frame delivered", ok);
    b.on_peer_disconnected(A);
    ib.inject(ia.sent_frame(0));  // adversarial replay after the receiver's reconnect
    check(delivered == 1, "B12: replay after reconnect is still rejected", ok);
    check(b.replays_rejected() == 1, "B12: counted as a replay", ok);
    ib.inject(ia.sent_frame(2));
    check(delivered == 2, "a fresh frame still flows after the reconnect", ok);
    return ok;
}

// --- D1 --------------------------------------------------------------------------------------------
bool test_d1_sweep_does_not_hold_map_lock_over_session_lock() {
    bool ok = true;
    const NodeId A{1}, B{2}, C{3};
    RecordingTransport ia;
    GateCipher cipher;
    SecureTransport a(ia, cipher, A);
    a.on_receive([](MessageFrame) {});
    a.send(B, data_frame(A, B));  // creates session B

    // A long seal on session B (holds session B's lock).
    std::thread sealer([&] { a.send(B, data_frame(A, B, std::byte{0xEE})); });
    cipher.wait_entered();

    auto revoked = std::make_shared<std::unordered_set<Fingerprint>>();
    revoked->insert(Fingerprint{});
    std::thread sweeper([&] { a.sweep_revocations(revoked); });
    std::this_thread::sleep_for(100ms);  // let the sweep reach (and, pre-fix, wedge on) session B

    // Session creation for a NEW peer needs map_mu_ exclusively; pre-fix it queued behind the sweep.
    auto fut = std::async(std::launch::async, [&] { a.send(C, data_frame(A, C)); });
    const bool unblocked = fut.wait_for(2s) == std::future_status::ready;
    check(unblocked, "D1: sweep must not hold map_mu_ while waiting on a session lock", ok);

    cipher.release();
    sealer.join();
    sweeper.join();
    fut.wait();
    return ok;
}

// --- D3 --------------------------------------------------------------------------------------------
struct SinkProbe {
    SecureTransport* st = nullptr;
    NodeId self{}, peer{};
    std::atomic<bool> probed{false};
    std::atomic<bool> completed_under_sink{false};
    std::future<void> pending;
};
void probe_sink(const AuditRecord&, void* ctx) noexcept {
    auto* p = static_cast<SinkProbe*>(ctx);
    if (p->probed.exchange(true)) return;
    // From ANOTHER thread, try to use the same peer's session while the sink runs. If deliver() still
    // holds the session lock here the send can't finish until the sink returns.
    p->pending = std::async(std::launch::async, [p] {
        MessageFrame f = data_frame(p->self, p->peer);
        p->st->send(p->peer, std::move(f));
    });
    p->completed_under_sink = p->pending.wait_for(1s) == std::future_status::ready;
}

bool test_d3_audit_sink_runs_outside_session_lock() {
    bool ok = true;
    const NodeId A{1}, B{2};
    RecordingTransport ia, ib;
    MockCipher cipher(7);
    SecureTransport a(ia, cipher, A), b(ib, cipher, B);
    a.on_receive([](MessageFrame) {});
    b.on_receive([](MessageFrame) {});
    a.send(B, data_frame(A, B));
    MessageFrame bad = ia.sent_frame(0);
    bad.payload.back() ^= std::byte{0xFF};  // corrupt the tag => AEAD open fails in b

    SinkProbe probe;
    probe.st = &b;
    probe.self = B;
    probe.peer = A;
    b.set_audit_sink(AuditSink{&probe_sink, &probe});
    ib.inject(std::move(bad));
    check(probe.probed.load(), "audit sink was invoked on the tamper drop", ok);
    check(probe.completed_under_sink.load(),
          "D3: the audit sink must run after the session lock is released", ok);
    if (probe.pending.valid()) probe.pending.wait();
    return ok;
}

// --- D6 --------------------------------------------------------------------------------------------
bool test_d6_engine_destroyed_outside_pending_lock() {
    bool ok = true;
    const ClusterId cluster{556};
    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    const NodeId self{1}, peer{5};
    RecordingTransport inner;
    MockCipher cipher(7);
    SecureTransport st(inner, cipher, self);
    IdentityMaterial identity(identity_blob);
    TrustStore trust(trust_blob);
    ParkedFactory factory;
    st.enable_handshake(factory, cluster, identity, trust);
    st.on_receive([](MessageFrame) {});

    std::atomic<bool> destroyed_probe_done{false};
    std::future<std::size_t> probe_future;  // joined below (pre-fix the probe is stuck until unlock)
    factory.on_destroy = [&] {
        // The engine's destructor needs pending_mu_ from another thread: it can't if the lock is held.
        probe_future = std::async(std::launch::async, [&] { return st.pending_handshake_count(); });
        destroyed_probe_done = probe_future.wait_for(500ms) == std::future_status::ready;
    };
    inner.inject(auth_frame(peer, self));  // parks an engine for `peer`
    check(st.pending_handshake_count() == 1, "engine parked", ok);
    st.on_peer_disconnected(peer);
    check(destroyed_probe_done.load(),
          "D6: parked engine must be destroyed after pending_mu_ is released", ok);
    if (probe_future.valid()) probe_future.wait();
    return ok;
}

// --- A9 --------------------------------------------------------------------------------------------
bool test_a9_pending_cap_not_overshot_by_concurrent_receive() {
    bool ok = true;
    const ClusterId cluster{556};
    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    const NodeId self{1};
    constexpr std::size_t kCap = 4;
    constexpr int kThreads = 16;
    RecordingTransport inner;
    MockCipher cipher(7);
    SecureTransport st(inner, cipher, self);
    IdentityMaterial identity(identity_blob);
    TrustStore trust(trust_blob);
    ParkedFactory factory;
    factory.create_delay = 3ms;  // widens the old check-then-insert window
    st.enable_handshake(factory, cluster, identity, trust);
    st.on_receive([](MessageFrame) {});
    st.set_max_pending_handshakes(kCap);
    st.set_reserved_handshake_slots(0);
    st.set_max_pending_per_ip(0);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            inner.inject(auth_frame(NodeId{static_cast<std::uint64_t>(100 + i)}, self));
        });
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    go = true;
    for (auto& t : ts) t.join();
    check(st.pending_handshake_count() <= kCap, "A9: pending handshakes never exceed the cap", ok);
    check(st.pending_handshake_count() == kCap, "A9: cap slots are all used", ok);
    return ok;
}

// --- D4 (TSan-detected) ----------------------------------------------------------------------------
bool test_d4_peer_session_lockfree_reads() {
    bool ok = true;
    PeerSession s(NodeId{2}, std::make_shared<MockCipher>(1), std::make_shared<MockCipher>(1), 0,
                  Fingerprint{}, 0);
    std::atomic<bool> stop{false};
    std::thread hot([&] {
        for (int i = 0; i < 20000; ++i) {
            std::lock_guard<std::mutex> g(s.session_mu());
            (void)s.next_send_seq(0);
            s.note_sealed();
        }
    });
    std::thread reneg([&] {
        for (std::uint64_t i = 1; i < 2000; ++i)
            s.install_renegotiated(std::make_shared<MockCipher>(i), std::make_shared<MockCipher>(i), i,
                                   Fingerprint{}, static_cast<std::int64_t>(i));
    });
    std::thread sweep([&] {
        while (!stop.load()) {
            (void)s.frames_sealed_on_key();
            (void)s.generation();
            (void)s.renegotiated_at_ns();
            (void)s.peer_fingerprint();
        }
    });
    hot.join();
    reneg.join();
    stop = true;
    sweep.join();
    check(true, "no crash", ok);
    return ok;
}

bool test_d4_transport_shared_state_stress() {
    bool ok = true;
    const NodeId A{1}, B{2};
    RecordingTransport ia, ib;
    MockCipher cipher(7);
    SecureTransport a(ia, cipher, A), b(ib, cipher, B);
    a.on_receive([](MessageFrame) {});
    std::atomic<int> got{0};
    b.on_receive([&](MessageFrame) { got.fetch_add(1); });
    constexpr int kFrames = 2000;
    for (int i = 0; i < kFrames; ++i) a.send(B, data_frame(A, B));

    std::atomic<bool> stop{false};
    std::thread rx([&] {
        for (int i = 0; i < kFrames; ++i) ib.inject(ia.sent_frame(static_cast<std::size_t>(i)));
        stop = true;
    });
    std::thread swap_rx([&] {  // re-registers the receiver while frames are being delivered
        while (!stop.load()) b.on_receive([&](MessageFrame) { got.fetch_add(1); });
    });
    std::thread revoke([&] {  // flips the handshake-time revocation snapshot concurrently
        while (!stop.load()) {
            b.set_revoked(std::make_shared<const std::unordered_set<Fingerprint>>());
            b.sweep_revocations(std::make_shared<const std::unordered_set<Fingerprint>>());
        }
    });
    rx.join();
    swap_rx.join();
    revoke.join();
    check(got.load() == kFrames, "all frames delivered exactly once despite receiver swaps", ok);
    return ok;
}
}  // namespace

int main() {
    bool ok = true;
    struct T {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"b12_seq_monotonic_across_reconnect", &test_b12_seq_monotonic_across_reconnect},
        {"d1_sweep_lock_nesting", &test_d1_sweep_does_not_hold_map_lock_over_session_lock},
        {"d3_audit_sink_outside_session_lock", &test_d3_audit_sink_runs_outside_session_lock},
        {"d6_engine_destroyed_outside_pending_lock", &test_d6_engine_destroyed_outside_pending_lock},
        {"a9_pending_cap_atomic", &test_a9_pending_cap_not_overshot_by_concurrent_receive},
        {"d4_peer_session_lockfree_reads", &test_d4_peer_session_lockfree_reads},
        {"d4_transport_shared_state_stress", &test_d4_transport_shared_state_stress},
    };
    for (const auto& t : tests) {
        const bool r = t.fn();
        std::printf("  %s: %s\n", t.name, r ? "ok" : "FAIL");
        ok = ok && r;
    }
    std::printf("secure_transport_concurrency_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
