// Issue #17 — the REAL cross-OS half of the mixed-OS conformance gate: ONE cluster node per process, so
// two processes on two different OS/arch (e.g. Windows/x86-64 and Linux/x86-64) form one cluster over real
// TCP and negotiate 016's wire mode from their genuine, differing pal::platform_abi_tag (nothing forced).
// Same scaffolding and protocol as tests/mixed_abi_cluster_conformance_test.cpp (which simulates the pair
// in one process); see MIXED-OS-AND-BULK-TRANSFER.md. Driven by tests/netem/run_mixed_os.sh. Built, NOT
// registered with CTest (needs two hosts).
//
//   mixed_os_node a <listen_port> [N=50] [expect=auto|tagged|tagless] [timeout_s=90]
//       the SEED  (NodeId 2): listens on loopback:<listen_port>, waits for the joiner.
//   mixed_os_node b <seed_port>   [N=50] [expect=auto|tagged|tagless] [timeout_s=90]
//       the JOINER (NodeId 1, the lower id: it initiates the handshake): dials 127.0.0.1:<seed_port>
//       (reach a remote seed through an ssh -L/-R tunnel), joins through SeedListDiscovery, exchanges N
//       Req/Resp pairs in BOTH directions, then leaves cleanly.
// expect=auto: tagless iff the peer's advertised abi_tag == our pal::platform_abi_tag, else the canonical
// tagged fallback. Each node prints one summary line and exits 0 (PASS) / 1 (FAIL).
// Caveat: the handshake is the repo's mock engine (as in the conformance test), not real mTLS.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace mixed_os {  // NAMED (not anonymous): canonical_type_name spells anon namespaces per-compiler, see #17 finding

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

struct Req {
    std::uint64_t reply_to = 0;
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Req, (1, reply_to), (2, n));
struct Resp {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Resp, (1, n));

constexpr std::uint64_t kResponderKey = 7, kCollectorKey = 9;

struct Collector : Actor<Collector, Sequential> {
    using protocol = Protocol<Resp>;
    std::atomic<int> count{0};
    std::atomic<long long> sum{0};
    void handle(const Resp& r) noexcept {
        sum.fetch_add(r.n, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_release);
    }
};

struct DblResponder : Actor<DblResponder, Sequential> {
    using protocol = Protocol<Req>;
    DistributedRouter* dist = nullptr;
    std::atomic<int> handled{0};
    void handle(const Req& q) noexcept {
        dist->tell_on<Collector>(NodeId{q.reply_to}, kCollectorKey, Resp{q.n * 2});
        handled.fetch_add(1, std::memory_order_release);
    }
};

struct MixedNode {
    NodeId self;
    ClusterId cluster;
    detail::MessagePool pool{4096};
    DblResponder responder;
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

    MixedNode(NodeId n, ClusterId c, std::uint16_t listen_port)
        : self(n), cluster(c), tcp(n, pal::ipv4_loopback, listen_port), secure(tcp, cipher, n), demux(secure),
          ctl_face(demux, true), data_face(demux, false), swim(n, ctl_face, swim_cfg(c)),
          dist(swim, local, data_face) {
        secure.enable_handshake(factory, c, identity, trust);
        tcp.set_peer_down_hook([this](NodeId p) { secure.on_peer_disconnected(p); });
        act_resp = std::make_unique<Activation>(&responder, DblResponder::dispatch_table(), pool.sink());
        act_coll = std::make_unique<Activation>(&collector, Collector::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<DblResponder>(kResponderKey), *act_resp);
        eng.register_activation(actor_id_of<Collector>(kCollectorKey), *act_coll);
        responder.dist = &dist;
        dist.set_authorizer(authz);
        dist.register_remote<DblResponder, Req>();
        dist.register_remote<Collector, Resp>();
        // Advertise the REAL platform ABI tag (SecureTransport does this by default; stated for clarity).
        SchemaAdvertisement adv;
        adv.abi_tag = pal::platform_abi_tag;
        adv.types = {type_key_of<Req>().value, type_key_of<Resp>().value};
        secure.set_local_schema(adv);
    }

    template <class F>
    auto on_loop(F f) {
        using R = decltype(f());
        auto p = std::make_shared<std::promise<R>>();
        auto fut = p->get_future();
        tcp.post([p, f = std::move(f)]() mutable { p->set_value(f()); });
        return fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready ? fut.get() : R{};
    }
};

}  // namespace mixed_os
using namespace mixed_os;

int main(int argc, char** argv) {
    if (argc < 3 || (std::strcmp(argv[1], "a") != 0 && std::strcmp(argv[1], "b") != 0)) {
        std::fprintf(stderr, "usage: %s a|b <port> [N=50] [expect=auto|tagged|tagless] [timeout_s=90]\n", argv[0]);
        return 2;
    }
    const bool seed = argv[1][0] == 'a';
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const int N = argc > 3 ? std::atoi(argv[3]) : 50;
    const std::string expect = argc > 4 ? argv[4] : "auto";
    const int timeout_ms = (argc > 5 ? std::atoi(argv[5]) : 90) * 1000;
    const char* role = seed ? "A(seed)" : "B(joiner)";

    const ClusterId cluster{1717};
    const NodeId idA{2}, idB{1};  // joiner has the LOWER id: it initiates the handshake
    const NodeId me = seed ? idA : idB, peer = seed ? idB : idA;
    MixedNode n(me, cluster, seed ? port : 0);
    bool ok = true;
    auto fail = [&](const char* what) {
        std::fprintf(stderr, "[%s] CHECK FAILED: %s\n", role, what);
        ok = false;
    };

    if (!n.tcp.start()) {
        std::printf("[%s] FAIL (tcp start; port %u in use?)\n", role, port);
        return 1;
    }
    n.eng.start();
    std::printf("[%s] up: local_abi=0x%08X listen=%u\n", role, pal::platform_abi_tag, n.tcp.listen_port());
    // Diagnostic: the content-addressed keys must be IDENTICAL on every OS/compiler or remote dispatch fails.
    std::printf("[%s] keys: Req=%016llx Resp=%016llx Responder=%016llx Collector=%016llx\n", role,
                static_cast<unsigned long long>(type_key_of<Req>().value),
                static_cast<unsigned long long>(type_key_of<Resp>().value),
                static_cast<unsigned long long>(type_key_of<DblResponder>().value),
                static_cast<unsigned long long>(type_key_of<Collector>().value));
    std::fflush(stdout);

    SeedListDiscovery discovery({loopback_endpoint(idA, port)});
    if (!seed)
        for (const Endpoint& s : discovery.contacts()) n.tcp.add_peer(s);

    std::atomic<bool> stop_ticks{false};
    std::thread ticker([&] {
        int i = 0;
        while (!stop_ticks.load()) {
            if (!seed && i++ % 5 == 0)
                n.tcp.post([&n, peer] { n.swim.request_join(peer); });
            n.tcp.post([&n] { n.swim.tick(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    const bool joined = wait_until([&] { return n.on_loop([&] { return n.swim.status_of(peer) == MemberStatus::Alive; }); },
                                   timeout_ms);
    if (!joined) fail("join: SWIM never saw the peer Alive");

    std::uint32_t peer_abi = 0;
    bool advert = false;
    if (joined)
        advert = wait_until([&] {
            const auto s = n.secure.peer_schema(peer);
            if (s) peer_abi = s->abi_tag;
            return s.has_value() && !s->types.empty();
        }, 15000);
    if (joined && !advert) fail("authenticated schema advert never arrived");

    const bool same_abi = peer_abi == pal::platform_abi_tag;
    const bool want_tagless = expect == "tagless" || (expect == "auto" && same_abi);
    long long expect_sum = 2LL * N * (N - 1) / 2;
    if (joined && advert) {
        // Both sides send N Reqs to the peer; each peer's DblResponder answers; our Collector gets N Resps.
        for (int i = 0; i < N; ++i) n.dist.tell_on<DblResponder>(peer, kResponderKey, Req{me.value, i});
        const bool done = wait_until(
            [&] { return n.collector.count.load() >= N && n.responder.handled.load() >= N; }, timeout_ms);
        if (!done) fail("ask/reply did not complete in both directions");
        if (n.collector.sum.load() != expect_sum) fail("ask/reply payload sum wrong");
    }

    const auto tl = n.demux.tagless_.load(), tg = n.demux.tagged_.load();
    if (joined && advert) {
        if (want_tagless && !(tl >= static_cast<std::uint64_t>(N) && tg == 0)) fail("expected TAGLESS fast path only");
        if (!want_tagless && !(tg >= static_cast<std::uint64_t>(N) && tl == 0)) fail("expected canonical TAGGED fallback only");
    }

    if (seed) {
        // Stay up until the joiner has left (or time out), so the joiner's last frames are served.
        const bool left = wait_until([&] { return n.on_loop([&] { return n.swim.status_of(peer) == MemberStatus::Left; }); },
                                     timeout_ms);
        if (!left) fail("clean leave: peer never reached Left");
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the seed's last Resps land
        n.tcp.post([&n, me] { n.swim.leave(me); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    stop_ticks.store(true);
    ticker.join();
    n.eng.stop();
    n.tcp.stop();
    std::printf("[%s] local_abi=0x%08X peer_abi=0x%08X same_abi=%d inbound tagless=%llu tagged=%llu "
                "req_handled=%d resp_got=%d sum=%lld expect=%s -> %s\n",
                role, pal::platform_abi_tag, peer_abi, same_abi ? 1 : 0, static_cast<unsigned long long>(tl),
                static_cast<unsigned long long>(tg), n.responder.handled.load(), n.collector.count.load(),
                static_cast<long long>(n.collector.sum.load()), expect.c_str(), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
