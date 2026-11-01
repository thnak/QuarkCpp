// Shared helpers for the pinned-key (Noise IK, #40 / ADR-068) checks: NOT a *_check.cpp, only an include. Node factories over
// PinnedKeyBook, a driver that runs two HandshakeEngines against each other while recording every byte, a crafted-Msg1
// builder for the attack cases, a tiny single-connection-at-a-time TCP responder + prober over the PAL for the "zero bytes
// on the wire" checks, and a byte-substring search for the capture checks.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pal/net.hpp"
#include "quark/adapters/sodium/pinned_handshake.hpp"
#include "quark/adapters/sodium/pinned_identity.hpp"
#include "quark/core/handshake.hpp"

namespace quark::test {

inline void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

using adapters::PinnedHandshakeEngineFactory;
using adapters::PinnedIdentity;
using adapters::PinnedKeyBook;
using adapters::PinnedOptions;
using adapters::noise::Bytes;

// A trust provider that admits everything: the "always-true" CONTROL provider.
class AdmitAllPinnedTrust final : public PinnedTrust {
public:
    [[nodiscard]] bool admitted(ClusterId, const Ed25519PublicKey&) const noexcept override { return true; }
};

// Controllable test clock (unix ns) shared by reference.
struct TestClock {
    std::shared_ptr<std::atomic<std::int64_t>> now = std::make_shared<std::atomic<std::int64_t>>(1'800'000'000'000'000'000LL);
    [[nodiscard]] std::function<std::int64_t()> fn() const {
        auto p = now;
        return [p] { return p->load(); };
    }
    void advance_s(std::int64_t s) { now->fetch_add(s * 1'000'000'000LL); }
};

// A node: its factory (identity inside), its key book (trust + directory), its derived NodeId and public key.
struct Node {
    std::shared_ptr<PinnedKeyBook> book = std::make_shared<PinnedKeyBook>();
    std::unique_ptr<PinnedHandshakeEngineFactory> factory;
    NodeId id{};
    Ed25519PublicKey pub{};

    explicit Node(PinnedOptions opt = {}, std::shared_ptr<const PinnedTrust> trust_override = nullptr) {
        if (!opt.clock_unix_ns) opt.clock_unix_ns = TestClock{}.fn();  // fixed 1.8e18 ns unless the test supplies one
        // A derived pinned NodeId has a random bit 63, which ADR-074 D3 reads as "client" (a member never dials a client,
        // and the client always initiates). These checks model member<->member pairs with a fixed dialler/responder, so draw
        // a member id; otherwise half of all pairs silently change roles and the checks flake (issue #49b).
        PinnedIdentity identity = PinnedIdentity::generate();
        while (is_client_node(identity.node_id())) identity = PinnedIdentity::generate();
        id = identity.node_id();
        pub = identity.public_key();
        factory = std::make_unique<PinnedHandshakeEngineFactory>(
            std::move(identity), trust_override ? trust_override : std::shared_ptr<const PinnedTrust>(book), book, opt);
    }
    std::unique_ptr<HandshakeEngine> engine(bool client, NodeId peer, ClusterId c,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>> revoked = nullptr) {
        return factory->create(client, id, peer, c, {}, {}, std::move(revoked));
    }
};

// Mutual admission: each node's book admits the other (and itself harmlessly) in `c`.
inline void pair_up(Node& a, Node& b, ClusterId c) {
    (void)a.book->admit(c, b.pub);
    (void)b.book->admit(c, a.pub);
}

struct Outcome {
    HandshakeEngine::Step init_step = HandshakeEngine::Step::Failed;
    HandshakeEngine::Step resp_step = HandshakeEngine::Step::Failed;
    Bytes msg1, msg2;  // exactly the bytes each side put on the wire (empty = it sent nothing)
    HandshakeEngine::Result init_result, resp_result;
    std::string init_reason, resp_reason;
    [[nodiscard]] bool both_done() const {
        return init_step == HandshakeEngine::Step::Done && resp_step == HandshakeEngine::Step::Done;
    }
};

// Run initiator `i` against responder `r`, in memory, delivering whatever each side emits.
inline Outcome run_pair(HandshakeEngine& i, HandshakeEngine& r) {
    Outcome o;
    Bytes none;
    o.init_step = i.advance({}, o.msg1);
    if (o.init_step != HandshakeEngine::Step::WantWrite) {
        o.init_reason = std::string(i.failure_reason());
        return o;
    }
    o.resp_step = r.advance(o.msg1, o.msg2);
    if (o.resp_step == HandshakeEngine::Step::Failed) o.resp_reason = std::string(r.failure_reason());
    if (o.resp_step != HandshakeEngine::Step::Done) return o;
    Bytes sink;
    o.init_step = i.advance(o.msg2, sink);
    if (o.init_step == HandshakeEngine::Step::Failed) o.init_reason = std::string(i.failure_reason());
    if (o.init_step == HandshakeEngine::Step::Done) o.init_result = i.take_result();
    o.resp_result = r.take_result();
    return o;
}

// Build a Msg1 by hand, as an ATTACKER (or a differently-behaving peer) would: `signer`'s Noise static authenticates it,
// the payload says whatever the caller wants. `responder_key` is the Ed25519 key the message is encrypted to.
inline Bytes craft_msg1(const PinnedIdentity& signer, const Ed25519PublicKey& responder_key,
                        const adapters::PinnedMsg1Payload& payload,
                        const Bytes& prologue = adapters::pinned_prologue()) {
    adapters::noise::Key32 rs{};
    (void)adapters::ed25519_to_x25519(responder_key, rs);
    adapters::noise::IkHandshake hs(adapters::noise::Role::Initiator, prologue, signer.noise_static(), &rs);
    Bytes msg;
    (void)hs.write_msg1(adapters::encode_pinned_msg1_payload(payload), msg);
    return msg;
}

[[nodiscard]] inline adapters::PinnedMsg1Payload honest_payload(const PinnedIdentity& id, ClusterId c, std::uint64_t ts) {
    adapters::PinnedMsg1Payload p;
    p.cluster = c;
    p.node = id.node_id();
    p.timestamp_ns = ts;
    p.ed25519 = id.public_key();
    return p;
}

// Feed a fixed byte string to a responder engine and report what it would put on the wire.
struct Feed {
    HandshakeEngine::Step step = HandshakeEngine::Step::Failed;
    Bytes out;
    std::string reason;
};
inline Feed feed_responder(HandshakeEngine& r, const Bytes& in) {
    Feed f;
    f.step = r.advance(in, f.out);
    f.reason = std::string(r.failure_reason());
    return f;
}

[[nodiscard]] inline bool contains(const Bytes& hay, std::span<const std::byte> needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::equal(needle.begin(), needle.end(), hay.begin() + static_cast<std::ptrdiff_t>(i))) return true;
    return false;
}
[[nodiscard]] inline Bytes le64(std::uint64_t v) {
    Bytes b;
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    return b;
}
// Length of the longest run of bytes that appears in BOTH captures (O(n*m), tiny inputs).
[[nodiscard]] inline std::size_t longest_common_run(const Bytes& a, const Bytes& b) {
    std::size_t best = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < b.size(); ++j) {
            std::size_t k = 0;
            while (i + k < a.size() && j + k < b.size() && a[i + k] == b[j + k]) ++k;
            if (k > best) best = k;
        }
    return best;
}

// ---- A real TCP listener + prober over the PAL ----------------------------------------------------------
// A handler gets everything received so far on a connection and may append a reply; returning Close ends the connection.
enum class Verdict { Continue, Close };
using ConnHandler = std::function<Verdict(Bytes& received, Bytes& reply)>;

class TcpResponder {
public:
    explicit TcpResponder(std::function<ConnHandler()> make_handler) : make_handler_(std::move(make_handler)) {
        auto l = pal::tcp_listen(pal::ipv4_loopback, 0);
        lfd_ = *l;
        port_ = *pal::local_port(lfd_);
        th_ = std::thread([this] { loop(); });
    }
    ~TcpResponder() {
        stop_.store(true);
        th_.join();
        pal::close_fd(lfd_);
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return sent_.load(); }
    [[nodiscard]] std::uint64_t connections() const noexcept { return accepted_.load(); }
    [[nodiscard]] std::uint64_t closed_by_server() const noexcept { return closed_.load(); }

private:
    struct Conn {
        pal::fd_t fd;
        ConnHandler h;
        Bytes in;
    };
    void loop() {
        std::vector<Conn> conns;
        while (!stop_.load()) {
            bool idle = true;
            if (auto a = pal::accept_one(lfd_)) {
                conns.push_back(Conn{*a, make_handler_(), {}});
                accepted_.fetch_add(1);
                idle = false;
            }
            for (std::size_t i = 0; i < conns.size();) {
                Conn& c = conns[i];
                std::byte buf[512];
                bool drop = false;
                auto r = pal::recv_some(c.fd, buf, sizeof(buf));
                if (r && *r == 0) {
                    drop = true;  // peer closed
                } else if (r) {
                    idle = false;
                    c.in.insert(c.in.end(), buf, buf + *r);
                    Bytes reply;
                    const Verdict v = c.h(c.in, reply);
                    for (std::size_t off = 0; off < reply.size();) {
                        auto s = pal::send_some(c.fd, reply.data() + off, reply.size() - off);
                        if (s) {
                            off += *s;
                            sent_.fetch_add(*s);
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                    if (v == Verdict::Close) {
                        closed_.fetch_add(1);
                        drop = true;
                    }
                } else if (r.error() != pal::would_block()) {
                    drop = true;  // reset
                }
                if (drop) {
                    pal::close_fd(c.fd);
                    conns.erase(conns.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            if (idle) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& c : conns) pal::close_fd(c.fd);
    }

    std::function<ConnHandler()> make_handler_;
    pal::fd_t lfd_{};
    std::uint16_t port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> sent_{0}, accepted_{0}, closed_{0};
};

// Connect, send `to_send`, then read for up to `listen_ms` and report what came back and whether the server closed.
struct ProbeResult {
    bool connected = false;
    Bytes received;
    bool closed_by_peer = false;
};
inline ProbeResult probe(std::uint16_t port, const Bytes& to_send, int listen_ms = 400, std::size_t want_bytes = 0) {
    ProbeResult pr;
    auto c = pal::tcp_connect(pal::ipv4_loopback, port);
    if (!c) return pr;
    const pal::fd_t fd = *c;
    std::size_t sent = 0;
    for (int i = 0; i < 2000 && sent < to_send.size(); ++i) {
        auto s = pal::send_some(fd, to_send.data() + sent, to_send.size() - sent);
        if (s) sent += *s;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pr.connected = sent == to_send.size();
    for (int i = 0; i < listen_ms; ++i) {
        std::byte buf[512];
        auto r = pal::recv_some(fd, buf, sizeof(buf));
        if (r && *r == 0) {
            pr.closed_by_peer = true;
            break;
        }
        if (r) {
            pr.received.insert(pr.received.end(), buf, buf + *r);
            if (want_bytes != 0 && pr.received.size() >= want_bytes) break;
            continue;
        }
        if (r.error() != pal::would_block()) {
            pr.closed_by_peer = true;  // reset counts as closed
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pal::close_fd(fd);
    return pr;
}

// The handler a pinned responder runs: one engine per connection, EXACTLY the transport's duty — send what the engine
// emits, close on Failed, keep the (completed) connection until the peer goes away. Records completed results.
struct PinnedServerState {
    std::atomic<int> completed{0};
    std::atomic<int> refused{0};
};
inline std::function<ConnHandler()> pinned_server_handler(Node& node, ClusterId cluster, PinnedServerState& st) {
    return [&node, cluster, &st]() -> ConnHandler {
        auto eng = std::shared_ptr<HandshakeEngine>(node.engine(false, NodeId{0}, cluster));
        auto fed = std::make_shared<std::size_t>(0);
        return [eng, fed, &st](Bytes& received, Bytes& reply) -> Verdict {
            // The engine is message-oriented, so THIS caller (the transport's job) reassembles the fixed 154-byte Msg1 from the
            // byte stream: nothing is fed, and nothing is said, until 154 bytes have arrived (or more: then it is refused).
            if (*fed != 0 || received.size() < adapters::kPinnedMsg1Len) return Verdict::Continue;
            *fed = received.size();
            const auto step = eng->advance(received, reply);
            if (step == HandshakeEngine::Step::Failed) {
                st.refused.fetch_add(1);
                // NOT cleared: if an engine ever emitted bytes on a refusal, bytes_sent() would show it.
                return Verdict::Close;
            }
            if (step == HandshakeEngine::Step::Done) st.completed.fetch_add(1);
            return Verdict::Continue;
        };
    };
}

}  // namespace quark::test
