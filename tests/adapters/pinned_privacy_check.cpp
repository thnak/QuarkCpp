// "No identity before admission" for the pinned-key mode (Noise IK, #40 / ADR-068; AeroCoWorker RFC 002 section 6e findings
// B2/B3), observed on REAL TCP connections over the PAL: what a prober, an un-admitted device, an eavesdropper and a replayer
// actually receive. Each property has a control that must fail the other way.
//   P1  a prober without the responder's key gets ZERO bytes and the server closes (wrong-key Msg1, random bytes); a short
//       write gets silence. CONTROL: a Noise XX responder (the pattern with no pinned key) answers a bare ephemeral and
//       reveals its static key to the prober.
//   P2  an initiator that knows the responder's key but is not admitted gets ZERO bytes. CONTROL: an always-true provider
//       (and, separately, admitting it) completes.
//   P3  a replayed Msg1 is refused (zero bytes). CONTROL: with the replay rule disabled the responder answers the replay;
//       the clock-skew half of the rule still refuses a replay once the capture is stale.
//   P4  capture: two sessions by the same initiator share no bytes beyond fixed lengths, and no NodeId / key / scope id
//       appears in cleartext. CONTROL: today's TCP transport sends its HELLO, which shows the NodeId in cleartext to a
//       prober that has sent nothing at all.
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" (no pre-admission disclosure, silent refusal, replay rule).
#include <cstdio>

#include "pinned_test_util.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster{777};

struct Dial {
    bool completed = false;
    ProbeResult wire;
    Bytes sent;
};
// Run an INITIATOR engine over a real TCP connection to `port`: send what it emits, feed back what arrives.
Dial dial(HandshakeEngine& eng, std::uint16_t port) {
    Dial d;
    Bytes m1;
    if (eng.advance({}, m1) != HandshakeEngine::Step::WantWrite) return d;
    d.sent = m1;
    d.wire = probe(port, m1, 600, kPinnedMsg2Len);
    if (d.wire.received.size() == kPinnedMsg2Len) {
        Bytes sink;
        d.completed = eng.advance(d.wire.received, sink) == HandshakeEngine::Step::Done;
    }
    return d;
}

// ---- Noise XX responder (CONTROL): `-> e` / `<- e, ee, s, es` — it discloses its static key to whoever sends an ephemeral.
ConnHandler xx_handler(PinnedIdentity::NoiseStatic s) {
    return [s](Bytes& received, Bytes& reply) -> Verdict {
        if (received.size() < noise::kDhLen) return Verdict::Continue;
        noise::SymmetricState ss;
        ss.initialize("Noise_XX_25519_ChaChaPoly_BLAKE2b");
        ss.mix_hash({});
        noise::Key32 re{};
        std::memcpy(re.data(), received.data(), re.size());
        ss.mix_hash(re);
        noise::KeyPair e;
        ::randombytes_buf(e.sk.data(), e.sk.size());
        e.pk = noise::public_from_private(e.sk);
        reply.insert(reply.end(), e.pk.begin(), e.pk.end());
        ss.mix_hash(e.pk);
        noise::Key32 d{};
        if (!noise::dh(e.sk, re, d)) return Verdict::Close;
        ss.mix_key(d);                                                // ee
        if (!ss.encrypt_and_hash(s.pk, reply)) return Verdict::Close;  // s
        if (!noise::dh(s.sk, re, d)) return Verdict::Close;
        ss.mix_key(d);                                                // es
        if (!ss.encrypt_and_hash({}, reply)) return Verdict::Close;   // empty payload
        return Verdict::Continue;
    };
}
// The prober's side of XX: sends a bare ephemeral, decrypts the reply, returns the responder's static key if it learned it.
std::optional<noise::Key32> xx_prober(std::uint16_t port) {
    noise::SymmetricState ss;
    ss.initialize("Noise_XX_25519_ChaChaPoly_BLAKE2b");
    ss.mix_hash({});
    noise::KeyPair e;
    ::randombytes_buf(e.sk.data(), e.sk.size());
    e.pk = noise::public_from_private(e.sk);
    ss.mix_hash(e.pk);
    const Bytes hello(e.pk.begin(), e.pk.end());
    const ProbeResult r = probe(port, hello, 600, 96);
    if (r.received.size() < 80) return std::nullopt;
    noise::Key32 re{};
    std::memcpy(re.data(), r.received.data(), re.size());
    ss.mix_hash(re);
    noise::Key32 d{};
    if (!noise::dh(e.sk, re, d)) return std::nullopt;
    ss.mix_key(d);
    Bytes rs;
    if (!ss.decrypt_and_hash(std::span<const std::byte>(r.received.data() + 32, 48), rs) || rs.size() != 32) return std::nullopt;
    noise::Key32 out{};
    std::memcpy(out.data(), rs.data(), 32);
    return out;
}
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;

    // ---- P1: a prober without the responder's key --------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        PinnedServerState st;
        TcpResponder server(pinned_server_handler(b, kCluster, st));

        Bytes noise_bytes(kPinnedMsg1Len);
        ::randombytes_buf(noise_bytes.data(), noise_bytes.size());
        const ProbeResult r1 = probe(server.port(), noise_bytes);
        check(r1.connected && r1.received.empty() && r1.closed_by_peer, "P1: random bytes of Msg1 length => ZERO bytes back, server closes", ok);

        // A well-formed Msg1 from a prober that GUESSED the responder's key wrong (encrypted to another key).
        Node prober, decoy;
        (void)prober.book->admit(kCluster, decoy.pub);
        auto guess = prober.engine(true, decoy.id, kCluster);
        Bytes m1;
        (void)guess->advance({}, m1);
        const ProbeResult r2 = probe(server.port(), m1);
        check(r2.connected && m1.size() == kPinnedMsg1Len && r2.received.empty() && r2.closed_by_peer,
              "P1: a well-formed Msg1 encrypted to the WRONG key => zero bytes, server closes", ok);

        // Wrong length (too long): refused immediately; too short: silent wait (nothing to answer yet).
        Bytes too_long(kPinnedMsg1Len + 20);
        ::randombytes_buf(too_long.data(), too_long.size());
        const ProbeResult r3 = probe(server.port(), too_long);
        check(r3.received.empty() && r3.closed_by_peer, "P1: an over-long write => zero bytes, server closes", ok);
        const ProbeResult r4 = probe(server.port(), Bytes(10, std::byte{1}), 150);
        check(r4.received.empty(), "P1: a short write => silence", ok);

        check(server.bytes_sent() == 0, "P1: across every probe the server sent ZERO bytes in total", ok);
        check(st.completed.load() == 0 && st.refused.load() >= 3, "P1: no probe completed; the refusals were counted locally", ok);
        check(server.connections() >= 4, "P1: (what a prober DOES learn: something accepted TCP on the port)", ok);

        // CONTROL: the XX responder answers a bare ephemeral and discloses its static key.
        TcpResponder xx([&] { return xx_handler(b.factory->identity().noise_static()); });
        const auto leaked = xx_prober(xx.port());
        check(leaked.has_value() && *leaked == b.factory->identity().noise_static().pk,
              "P1 CONTROL: an XX responder reveals its static key to an unauthenticated prober", ok);
        check(xx.bytes_sent() == 96, "P1 CONTROL: ...by answering with 96 bytes", ok);
    }

    // ---- P2: known key, not admitted -----------------------------------------------------------------------------------------
    {
        Node b, c;
        (void)c.book->admit(kCluster, b.pub);  // C trusts B (so C's engine does speak); B has never admitted C
        PinnedServerState st;
        TcpResponder server(pinned_server_handler(b, kCluster, st));
        auto eng = c.engine(true, b.id, kCluster);
        const Dial d = dial(*eng, server.port());
        check(d.sent.size() == kPinnedMsg1Len && d.wire.connected, "P2: C sent a well-formed Msg1 to B", ok);
        check(!d.completed && d.wire.received.empty() && d.wire.closed_by_peer && server.bytes_sent() == 0,
              "P2: an initiator that knows B's key but is not admitted gets ZERO bytes and a close", ok);

        // CONTROL 1: the always-true provider completes.
        Node b2(PinnedOptions{}, std::make_shared<AdmitAllPinnedTrust>());
        Node c2;
        (void)c2.book->admit(kCluster, b2.pub);
        PinnedServerState st2;
        TcpResponder server2(pinned_server_handler(b2, kCluster, st2));
        auto eng2 = c2.engine(true, b2.id, kCluster);
        const Dial d2 = dial(*eng2, server2.port());
        check(d2.completed && d2.wire.received.size() == kPinnedMsg2Len, "P2 CONTROL: an always-true provider completes the same handshake", ok);

        // CONTROL 2: admitting C into B's book completes against the ORIGINAL server.
        (void)b.book->admit(kCluster, c.pub);
        auto eng3 = c.engine(true, b.id, kCluster);
        const Dial d3 = dial(*eng3, server.port());
        check(d3.completed, "P2 CONTROL: admitting C (live, no restart) makes the next handshake succeed", ok);
    }

    // ---- P3: replay ------------------------------------------------------------------------------------------------------------
    {
        auto run_case = [&](bool replay_guard, bool stale_replay) {
            TestClock clk;
            PinnedOptions opt;
            opt.clock_unix_ns = clk.fn();
            opt.replay_guard = replay_guard;
            Node a(opt), b(opt);
            pair_up(a, b, kCluster);
            PinnedServerState st;
            TcpResponder server(pinned_server_handler(b, kCluster, st));
            auto eng = a.engine(true, b.id, kCluster);
            const Dial first = dial(*eng, server.port());
            check(first.completed, "P3: the original handshake completes", ok);
            if (stale_replay) clk.advance_s(200);  // the capture is now older than the 120 s skew window
            const ProbeResult replay = probe(server.port(), first.sent, 600, kPinnedMsg2Len);
            return std::pair{replay.received.size(), replay.closed_by_peer};
        };
        const auto guarded = run_case(true, false);
        check(guarded.first == 0 && guarded.second, "P3: a replayed Msg1 is refused: zero bytes, server closes", ok);
        const auto unguarded = run_case(false, false);
        check(unguarded.first == kPinnedMsg2Len, "P3 CONTROL: with the replay rule disabled the responder ANSWERS the replay", ok);
        const auto stale = run_case(false, true);
        check(stale.first == 0 && stale.second, "P3: the clock-skew bound alone still refuses a stale capture (rule disabled, 200 s later)", ok);
    }

    // ---- P4: capture ------------------------------------------------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        PinnedServerState st;
        TcpResponder server(pinned_server_handler(b, kCluster, st));
        const Dial s1 = dial(*a.engine(true, b.id, kCluster), server.port());
        const Dial s2 = dial(*a.engine(true, b.id, kCluster), server.port());
        check(s1.completed && s2.completed, "P4: two sessions by the same initiator complete", ok);
        const Bytes& c2s1 = s1.sent;
        const Bytes& c2s2 = s2.sent;
        const Bytes& s2c1 = s1.wire.received;
        const Bytes& s2c2 = s2.wire.received;
        check(c2s1.size() == kPinnedMsg1Len && c2s2.size() == kPinnedMsg1Len && s2c1.size() == kPinnedMsg2Len && s2c2.size() == kPinnedMsg2Len,
              "P4: every capture is a fixed length (154 up, 58 down), no framing beyond that", ok);
        check(longest_common_run(c2s1, c2s2) < 5, "P4: two Msg1s by the same initiator share no run of 5 bytes", ok);
        check(longest_common_run(s2c1, s2c2) < 5, "P4: two Msg2s share no run of 5 bytes", ok);
        noise::Key32 ax{}, bx{};
        (void)ed25519_to_x25519(a.pub, ax);
        (void)ed25519_to_x25519(b.pub, bx);
        const Bytes all = [&] {
            Bytes x = c2s1;
            x.insert(x.end(), c2s2.begin(), c2s2.end());
            x.insert(x.end(), s2c1.begin(), s2c1.end());
            x.insert(x.end(), s2c2.begin(), s2c2.end());
            return x;
        }();
        const Bytes ida = le64(a.id.value), idb = le64(b.id.value), cl = le64(kCluster.value);
        check(!contains(all, ida) && !contains(all, idb), "P4: neither NodeId appears in cleartext", ok);
        check(!contains(all, cl), "P4: the scope/cluster id does not appear in cleartext", ok);
        check(!contains(all, a.pub) && !contains(all, b.pub) && !contains(all, ax) && !contains(all, bx),
              "P4: no public key (Ed25519 or X25519, either side) appears in cleartext", ok);
        // CONTROL: the checker CAN see an identifier when it is there (a non-vacuous search).
        Bytes planted = c2s1;
        planted.insert(planted.end(), ida.begin(), ida.end());
        check(contains(planted, ida), "P4 CONTROL: the search finds a NodeId planted in the capture", ok);

        // CONTROL: today's TCP transport (the one TLS mode rides on) discloses its NodeId in its HELLO to a prober that
        // has sent nothing at all.
        const NodeId tls_node{0x0123456789ABCDEFULL};
        net::TcpTransport t(tls_node, pal::ipv4_loopback, 0);
        t.set_authenticated_transport(true);
        check(t.start(), "P4 CONTROL: the TCP transport starts", ok);
        const ProbeResult h1 = probe(t.listen_port(), {}, 600, net::hello_size);
        const ProbeResult h2 = probe(t.listen_port(), {}, 600, net::hello_size);
        check(h1.received.size() == net::hello_size && contains(h1.received, le64(tls_node.value)),
              "P4 CONTROL: today's HELLO shows the NodeId in cleartext to a silent prober", ok);
        check(h1.received == h2.received && longest_common_run(h1.received, h2.received) == net::hello_size,
              "P4 CONTROL: ...identically on every connection (a stable, linkable identifier)", ok);
        t.stop();
    }

    std::printf(ok ? "pinned_privacy_check: OK\n" : "pinned_privacy_check: FAILED\n");
    return ok ? 0 : 1;
}
