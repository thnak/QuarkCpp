// Issue #17 — mixed-OS / mixed-ABI cluster conformance, simulated in ONE process over REAL loopback TCP.
// Implements 016-Serialization §"Wire fast path" (connect-time negotiation), 021 §2 (SeedListDiscovery +
// join) / §3 (SWIM leave), F17 (authenticated schema advert, secure_transport.hpp).
//
// Two full nodes, each: TcpTransport -> SecureTransport (mock mTLS handshake, see caveat) -> a Control/Data
// demux -> { SwimMembership, DistributedRouter }. MixedNode B joins via a SeedListDiscovery naming node A.
//   Phase 1 (positive control)  both nodes advertise the REAL platform ABI tag  => every data frame in
//                               both directions is negotiated WireMode::Tagless (fast path), 0 warnings.
//   Phase 2 (mismatch)          each node re-advertises an ALIEN ABI tag (what a peer on another OS/arch
//                               would send) => every data frame in both directions takes the canonical
//                               WireMode::Tagged fallback, the 009 fallback warning fires, and the
//                               payloads still decode to the right values.
//   Phase 3                     graceful leave: B leaves, A converges on Left.
// "ask/reply" is the application-level request + response pair (Req -> Responder -> Resp -> Collector):
// DistributedRouter::ask is local-only today (distribution.hpp, ask()), so a cross-node reply is a second
// tell, which is exactly what this exercises.
//
// SEAM / CAVEATS (all reported in MIXED-OS-AND-BULK-TRANSFER.md):
//  * The sender-side check compares the peer's ADVERTISED abi_tag with the compile-time constant
//    pal::platform_abi_tag (distribution.hpp peer_schema_ default, wire.hpp negotiate). There is no
//    per-node injectable tag, so the mismatch is forced by re-advertising an alien tag through the public
//    Transport::set_local_schema. Both directions then mismatch (each side compares the other's alien tag
//    to its own real constant).
//  * The handshake is the repo's mock engine/MockCipher (as in tcp_sealed_relay_test), NOT real TLS: this
//    proves negotiation + transport + membership, not certificate validation.
//  * One process proves the logic, not Windows<->Linux; the cross-runner job is a documented follow-up.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/detail/hash.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// ---- mock handshake (same shape as tcp_sealed_relay_test.cpp) ----------------------------------
void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::byte>((v >> (b * 8)) & 0xFF));
}
[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> in) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[static_cast<std::size_t>(b)])) << (b * 8);
    return v;
}
bool hello_ok(std::span<const std::byte> in, NodeId a, NodeId b, ClusterId c) {
    return in.size() == 24 && get_u64(in.subspan(0, 8)) == a.value && get_u64(in.subspan(8, 8)) == b.value &&
           get_u64(in.subspan(16, 8)) == c.value;
}
void put_hello(std::vector<std::byte>& out, NodeId a, NodeId b, ClusterId c) {
    put_u64(out, a.value);
    put_u64(out, b.value);
    put_u64(out, c.value);
}

class MockEngine final : public HandshakeEngine {
public:
    MockEngine(bool client, NodeId self, NodeId peer, ClusterId cluster)
        : client_(client), self_(self), peer_(peer), cluster_(cluster) {}
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (client_) {
            if (!sent_) {
                put_hello(out, self_, peer_, cluster_);
                sent_ = true;
                return Step::WantWrite;
            }
            if (!hello_ok(in, peer_, self_, cluster_)) {
                failure_ = "malformed/mismatched ServerHello";
                return Step::Failed;
            }
            return Step::Done;
        }
        if (!hello_ok(in, peer_, self_, cluster_)) {
            failure_ = "malformed/mismatched ClientHello";
            return Step::Failed;
        }
        put_hello(out, self_, peer_, cluster_);
        return Step::Done;
    }
    Result take_result() override {
        const std::uint64_t lo = self_.value < peer_.value ? self_.value : peer_.value;
        const std::uint64_t hi = self_.value < peer_.value ? peer_.value : self_.value;
        Result r;
        // client: send=H(lo,hi) recv=H(hi,lo); server: mirrored (matches tcp_sealed_relay_test).
        r.send_cipher = std::make_shared<MockCipher>(client_ ? detail::hash_combine(lo, hi) : detail::hash_combine(hi, lo));
        r.recv_cipher = std::make_shared<MockCipher>(client_ ? detail::hash_combine(hi, lo) : detail::hash_combine(lo, hi));
        r.peer_fingerprint[0] = static_cast<std::byte>(peer_.value & 0xFF);
        r.peer_node_id = peer_;
        r.peer_cluster_id = cluster_;
        return r;
    }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    bool client_;
    NodeId self_, peer_;
    ClusterId cluster_;
    bool sent_ = false;
    std::string failure_;
};

class MockFactory final : public HandshakeEngineFactory {
public:
    std::unique_ptr<HandshakeEngine> create(bool is_client, NodeId self, NodeId expected_peer, ClusterId cluster_id,
                                            IdentityMaterial::Snapshot, TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        return std::make_unique<MockEngine>(is_client, self, expected_peer, cluster_id);
    }
};

// ---- Control/Data demux: SWIM owns Control frames, the router owns Data frames. ------------------
// Both SwimMembership and DistributedRouter call on_receive() on the Transport they are given, and a
// transport has ONE sink, so each gets its own Face; the Demux routes by FrameKind. The tap records the
// negotiated WireMode of every inbound Data frame (the observable proof of which path was taken).
class Demux {
public:
    explicit Demux(Transport& inner) : inner_(&inner) {
        inner_->on_receive([this](MessageFrame f) {
            if (f.kind == FrameKind::Control) {
                if (ctl_) ctl_(std::move(f));
            } else if (f.kind == FrameKind::Data) {
                if (f.mode == WireMode::Tagless) tagless_.fetch_add(1, std::memory_order_relaxed);
                else tagged_.fetch_add(1, std::memory_order_relaxed);
                if (data_) data_(std::move(f));
            }
        });
    }
    class Face final : public Transport {
    public:
        Face(Demux& d, bool control) : d_(&d), control_(control) {}
        void send(NodeId to, MessageFrame f) override { d_->inner_->send(to, std::move(f)); }
        void on_receive(std::function<void(MessageFrame)> cb) override {
            (control_ ? d_->ctl_ : d_->data_) = std::move(cb);
        }
        [[nodiscard]] bool is_plaintext() const noexcept override { return d_->inner_->is_plaintext(); }
        void set_authenticated_transport(bool a) noexcept override { d_->inner_->set_authenticated_transport(a); }
        void set_local_schema(const SchemaAdvertisement& a) override { d_->inner_->set_local_schema(a); }
        [[nodiscard]] std::optional<SchemaAdvertisement> peer_schema(NodeId p) const override {
            return d_->inner_->peer_schema(p);
        }
        [[nodiscard]] bool peer_accepts_type(NodeId p, std::uint64_t k, std::uint32_t abi) const override {
            return d_->inner_->peer_accepts_type(p, k, abi);
        }
        [[nodiscard]] bool admit_send(NodeId p) noexcept override { return d_->inner_->admit_send(p); }
        void mark_congested(NodeId p, std::int64_t r) noexcept override { d_->inner_->mark_congested(p, r); }

    private:
        Demux* d_;
        bool control_;
    };
    std::atomic<std::uint64_t> tagless_{0}, tagged_{0};

private:
    Transport* inner_;
    std::function<void(MessageFrame)> ctl_, data_;
};

// ---- the application protocol: Req -> Responder (on the peer) -> Resp -> Collector (on the asker) ----
struct Req {
    std::uint64_t reply_to = 0;  // asker's NodeId
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Req, (1, reply_to), (2, n));
struct Resp {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Resp, (1, n));

constexpr std::uint64_t kResponderKey = 7, kCollectorKey = 9;

struct Collector : Actor<Collector, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.test.mixed_abi_cluster_conformance_test.Collector";
    using protocol = Protocol<Resp>;
    std::atomic<int> count{0};
    std::atomic<long long> sum{0};
    void handle(const Resp& r) noexcept {
        sum.fetch_add(r.n, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_release);
    }
};

struct Responder : Actor<Responder, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.test.mixed_abi_cluster_conformance_test.Responder";
    using protocol = Protocol<Req>;
    DistributedRouter* dist = nullptr;
    void handle(const Req& q) noexcept {
        dist->tell_on<Collector>(NodeId{q.reply_to}, kCollectorKey, Resp{q.n * 2});
    }
};

// One node: engine + local router + actors + TcpTransport + SecureTransport + demux + SWIM + router.
struct MixedNode {
    NodeId self;
    ClusterId cluster;
    detail::MessagePool pool{4096};
    Responder responder;
    Collector collector;
    std::unique_ptr<Activation> act_resp, act_coll;
    Engine<> eng{EngineConfig{1, 1, 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial identity{id_blob};
    TrustStore trust{trust_blob};
    MockFactory factory;
    MockCipher cipher{0};
    net::TcpTransport tcp;
    SecureTransport secure;
    Demux demux;
    Demux::Face ctl_face, data_face;
    SwimMembership swim;
    DistributedRouter dist;
    AllowAllAuthorizer authz;

    static SwimMembership::Config swim_cfg(ClusterId c) {
        SwimMembership::Config cfg;
        cfg.cluster_id = c;
        cfg.seed = 0x17;
        return cfg;
    }

    MixedNode(NodeId n, ClusterId c)
        : self(n), cluster(c), tcp(n, pal::ipv4_loopback, 0), secure(tcp, cipher, n), demux(secure),
          ctl_face(demux, true), data_face(demux, false), swim(n, ctl_face, swim_cfg(c)),
          dist(swim, local, data_face) {
        secure.enable_handshake(factory, c, identity, trust);
        tcp.set_peer_down_hook([this](NodeId p) { secure.on_peer_disconnected(p); });
        act_resp = std::make_unique<Activation>(&responder, Responder::dispatch_table(), pool.sink());
        act_coll = std::make_unique<Activation>(&collector, Collector::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Responder>(kResponderKey), *act_resp);
        eng.register_activation(actor_id_of<Collector>(kCollectorKey), *act_coll);
        responder.dist = &dist;
        dist.set_authorizer(authz);
        dist.register_remote<Responder, Req>();
        dist.register_remote<Collector, Resp>();
    }

    // The alien advertisement: what a peer on another OS/arch (different pal::platform_abi_tag) sends.
    void advertise_abi(std::uint32_t abi) {
        SchemaAdvertisement adv;
        adv.abi_tag = abi;
        adv.types = {type_key_of<Req>().value, type_key_of<Resp>().value};
        secure.set_local_schema(adv);
    }

    // SwimMembership is single-driving-thread: run every access on the transport I/O thread.
    template <class F>
    auto on_loop(F f) {
        using R = decltype(f());
        auto p = std::make_shared<std::promise<R>>();
        auto fut = p->get_future();
        tcp.post([p, f = std::move(f)]() mutable { p->set_value(f()); });
        return fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready ? fut.get() : R{};
    }
};

std::atomic<int> g_warnings{0};
void count_warning(std::string_view) { g_warnings.fetch_add(1, std::memory_order_relaxed); }

constexpr std::uint32_t kAlienAbiA = 0xA1'1E'00'01u;
constexpr std::uint32_t kAlienAbiB = 0xB2'2E'00'02u;

// Both directions: each node sends `n` Reqs to the other; each other's Responder answers; each
// Collector must receive `n` Resps summing to 2*(0+..+n-1). Returns when done or on timeout.
bool exchange(MixedNode& a, MixedNode& b, int n, bool& ok, const char* label) {
    const int ca = a.collector.count.load(), cb = b.collector.count.load();
    const long long sa = a.collector.sum.load(), sb = b.collector.sum.load();
    for (int i = 0; i < n; ++i) {
        a.dist.tell_on<Responder>(b.self, kResponderKey, Req{a.self.value, i});
        b.dist.tell_on<Responder>(a.self, kResponderKey, Req{b.self.value, i});
    }
    const bool done = wait_until(
        [&] { return a.collector.count.load() >= ca + n && b.collector.count.load() >= cb + n; }, 10000);
    check(done, label, ok);
    const long long expect = 2LL * n * (n - 1) / 2;
    check(a.collector.sum.load() - sa == expect && b.collector.sum.load() - sb == expect,
          "ask/reply payloads decoded to the right values in BOTH directions", ok);
    return done;
}

}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{1717};
    // Joiner B must have the LOWER NodeId: SecureTransport only initiates the mTLS handshake from the lower-id
    // side (secure_transport.hpp local_is_client), and the seed has no endpoint for the joiner.
    const NodeId idA{2}, idB{1};
    g_wire_fallback_warn = &count_warning;

    MixedNode a(idA, cluster), b(idB, cluster);
    check(a.tcp.start() && b.tcp.start(), "both TCP listeners start (mock-mTLS-authenticated)", ok);
    if (!ok) {
        std::printf("mixed_abi_cluster_conformance_test: FAIL (startup)\n");
        return 1;
    }
    a.eng.start();
    b.eng.start();

    // ---- join: B discovers A through SeedListDiscovery, then SWIM takes over ---------------------
    SeedListDiscovery discovery({loopback_endpoint(idA, a.tcp.listen_port())});
    for (const Endpoint& seed : discovery.contacts()) b.tcp.add_peer(seed);

    const auto alive = [](MixedNode& n, NodeId who) {
        return n.on_loop([&n, who] { return n.swim.status_of(who) == MemberStatus::Alive; });
    };
    bool joined = false;
    for (int i = 0; i < 400 && !joined; ++i) {  // 20 ms period => <= 8 s
        if (i % 5 == 0)
            for (const Endpoint& seed : discovery.contacts()) b.tcp.post([&b, seed] { b.swim.request_join(seed.node); });
        a.tcp.post([&a] { a.swim.tick(); });
        b.tcp.post([&b] { b.swim.tick(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        joined = alive(a, idB) && alive(b, idA);
    }
    check(joined, "join via SeedListDiscovery: SWIM membership converges Alive both ways", ok);
    if (!joined) {
        std::fprintf(stderr, "  diag: A sees B alive=%d, B sees A alive=%d, sessA=%d sessB=%d, A tcp rx/tx=%llu/%llu, B rx/tx=%llu/%llu\n",
                     static_cast<int>(alive(a, idB)), static_cast<int>(alive(b, idA)), static_cast<int>(a.secure.has_session(idB)), static_cast<int>(b.secure.has_session(idA)),
                     static_cast<unsigned long long>(a.tcp.frames_received()), static_cast<unsigned long long>(a.tcp.frames_sent()),
                     static_cast<unsigned long long>(b.tcp.frames_received()), static_cast<unsigned long long>(b.tcp.frames_sent()));
        a.eng.stop(); b.eng.stop(); a.tcp.stop(); b.tcp.stop();
        std::printf("mixed_abi_cluster_conformance_test: FAIL (join)\n");
        return 1;
    }

    // Drive SWIM in the background for the rest of the run (probe/gossip keep membership honest).
    std::atomic<bool> stop_ticks{false};
    std::thread ticker([&] {
        while (!stop_ticks.load()) {
            a.tcp.post([&a] { a.swim.tick(); });
            b.tcp.post([&b] { b.swim.tick(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    const std::uint64_t kReq = type_key_of<Req>().value, kResp = type_key_of<Resp>().value;
    const std::uint32_t real = pal::platform_abi_tag;
    constexpr int N = 50;

    // ---- Phase 1: positive control — same ABI => tagless fast path --------------------------------
    check(wait_until([&] {
              return a.secure.peer_accepts_type(idB, kReq, real) && a.secure.peer_accepts_type(idB, kResp, real) &&
                     b.secure.peer_accepts_type(idA, kReq, real) && b.secure.peer_accepts_type(idA, kResp, real);
          }, 8000),
          "authenticated schema adverts exchanged (same real ABI tag, both types)", ok);
    {
        const int w0 = g_warnings.load();
        const auto tl_a = a.demux.tagless_.load(), tl_b = b.demux.tagless_.load();
        const auto tg_a = a.demux.tagged_.load(), tg_b = b.demux.tagged_.load();
        exchange(a, b, N, ok, "phase 1: ask/reply completes both directions");
        check(a.demux.tagless_.load() - tl_a >= static_cast<std::uint64_t>(N) &&
                  b.demux.tagless_.load() - tl_b >= static_cast<std::uint64_t>(N),
              "positive control: same-ABI peers take the TAGLESS fast path (both directions)", ok);
        check(a.demux.tagged_.load() == tg_a && b.demux.tagged_.load() == tg_b,
              "positive control: no Tagged data frame observed", ok);
        check(g_warnings.load() == w0, "positive control: 009 fallback warning did not fire", ok);
    }

    // ---- Phase 2: forced ABI mismatch => canonical tagged fallback --------------------------------
    a.advertise_abi(kAlienAbiA);
    b.advertise_abi(kAlienAbiB);
    check(wait_until([&] {
              const auto sa = a.secure.peer_schema(idB), sb = b.secure.peer_schema(idA);
              return sa && sb && sa->abi_tag == kAlienAbiB && sb->abi_tag == kAlienAbiA;
          }, 8000),
          "mismatched ABI adverts delivered over the authenticated channel", ok);
    check(!a.secure.peer_accepts_type(idB, kReq, real), "A no longer considers B tagless-compatible", ok);
    {
        const int w0 = g_warnings.load();
        const auto tl_a = a.demux.tagless_.load(), tl_b = b.demux.tagless_.load();
        const auto tg_a = a.demux.tagged_.load(), tg_b = b.demux.tagged_.load();
        exchange(a, b, N, ok, "phase 2: ask/reply completes both directions over the canonical form");
        check(a.demux.tagged_.load() - tg_a >= static_cast<std::uint64_t>(N) &&
                  b.demux.tagged_.load() - tg_b >= static_cast<std::uint64_t>(N),
              "mismatch: ABI-mismatched peers take the canonical TAGGED fallback (both directions)", ok);
        check(a.demux.tagless_.load() == tl_a && b.demux.tagless_.load() == tl_b,
              "mismatch: no Tagless data frame observed (never a silent reinterpretation)", ok);
        check(g_warnings.load() > w0, "mismatch: 009 wire-fallback warning fired", ok);
    }

    // ---- Phase 3: clean leave ---------------------------------------------------------------------
    b.tcp.post([&b] { b.swim.leave(b.self); });
    const bool left = wait_until([&] { return a.on_loop([&a, idB] { return a.swim.status_of(idB) == MemberStatus::Left; }); }, 8000);
    check(left, "clean leave: A converges on B == Left", ok);

    stop_ticks.store(true);
    ticker.join();
    a.eng.stop();
    b.eng.stop();
    a.tcp.stop();
    b.tcp.stop();
    g_wire_fallback_warn = nullptr;

    std::printf("mixed_abi_cluster_conformance_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
