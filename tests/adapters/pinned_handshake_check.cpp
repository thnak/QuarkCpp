// The pinned-key (Noise IK) handshake engine, driven in memory (no sockets): ADR-068 claims H1-H10. Every property has a
// control that must fail the other way.
//   H1  a pinned pair completes; both ends bind the peer's FULL Ed25519 key, the DERIVED NodeId and the cluster; the wire
//       messages have the fixed lengths 154 / 58; the directional keys agree (and are NOT the same key both ways)
//   H2  two sessions between the same pair use different keys (control: one session twice is deterministic)
//   H3  trust change: un-admit => the NEXT handshake is refused (initiator sends nothing, responder sends nothing);
//       re-admit => it succeeds; a revoked fingerprint is refused
//   H4  man in the middle with its own key: refused by both ends; control: the same MITM, honestly pinned, completes
//   H5  the NodeId cannot be chosen: a Msg1 claiming another key's NodeId, or carrying a key that is not the one that
//       authenticated it, is refused; control: the honest payload from the same signer completes
//   H6  cross-mode: a different mode byte in the prologue never completes (no downgrade); control: same mode completes
//   H7  payload gates: version window, cluster, clock skew; each with a passing control
//   H8  a NodeId is never self-claimed: an engine whose `self` is not derived from its key fails
//   H9  forward secrecy: an attacker holding BOTH long-term secrets and the capture cannot open session traffic;
//       documented limit: Msg1's payload (who dialled) IS readable with the responder's long-term key
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)".
#include <cstdio>
#include <unordered_set>

#include "pinned_test_util.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster{4242};

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> b;
    for (char c : s) b.push_back(static_cast<std::byte>(c));
    return b;
}

bool seal_open(const Aead& tx, const Aead& rx, std::uint64_t nonce, const char* msg) {
    const auto pt = bytes_of(msg);
    const auto aad = bytes_of("aad");
    std::vector<std::byte> sealed, opened;
    tx.seal(nonce, aad, pt, sealed);
    return rx.open(nonce, aad, sealed, opened) && opened == pt;
}
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;

    // ---- H1: a pinned pair ------------------------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        auto ie = a.engine(true, b.id, kCluster);
        auto re = b.engine(false, a.id, kCluster);
        Outcome o = run_pair(*ie, *re);
        check(o.both_done(), "H1: handshake completes for a mutually admitted pair", ok);
        check(o.msg1.size() == kPinnedMsg1Len && o.msg2.size() == kPinnedMsg2Len, "H1: wire messages are 154 / 58 bytes", ok);
        check(kPinnedMsg1Len == 154 && kPinnedMsg2Len == 58, "H1: the documented lengths", ok);
        check(o.init_result.peer_node_id == b.id && o.resp_result.peer_node_id == a.id,
              "H1: each side binds the peer's DERIVED NodeId", ok);
        check(std::equal(b.pub.begin(), b.pub.end(), o.init_result.peer_fingerprint.begin()) &&
                  std::equal(a.pub.begin(), a.pub.end(), o.resp_result.peer_fingerprint.begin()),
              "H1: peer_fingerprint carries the peer's FULL Ed25519 key", ok);
        check(o.init_result.peer_cluster_id == kCluster && o.resp_result.peer_cluster_id == kCluster, "H1: cluster bound", ok);
        check(derive_pinned_node_id(a.pub) == a.id && !(a.id == b.id), "H1: NodeId = f(public key), distinct per key", ok);
        // Directional keys agree: initiator send == responder recv, responder send == initiator recv; and the two
        // directions are different keys (control: a side cannot read its own outbound traffic).
        check(seal_open(*o.init_result.send_cipher, *o.resp_result.recv_cipher, 5, "i->r"), "H1: initiator->responder key agrees", ok);
        check(seal_open(*o.resp_result.send_cipher, *o.init_result.recv_cipher, 5, "r->i"), "H1: responder->initiator key agrees", ok);
        check(!seal_open(*o.init_result.send_cipher, *o.init_result.recv_cipher, 5, "x"),
              "H1 CONTROL: the two directions use different keys", ok);
    }

    // ---- H2: two sessions, different keys ----------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        auto run = [&] {
            auto ie = a.engine(true, b.id, kCluster);
            auto re = b.engine(false, a.id, kCluster);
            return run_pair(*ie, *re);
        };
        Outcome s1 = run(), s2 = run();
        check(s1.both_done() && s2.both_done(), "H2: both sessions complete", ok);
        const auto pt = bytes_of("same plaintext");
        std::vector<std::byte> c1, c1b, c2;
        s1.init_result.send_cipher->seal(1, {}, pt, c1);
        s1.init_result.send_cipher->seal(1, {}, pt, c1b);
        s2.init_result.send_cipher->seal(1, {}, pt, c2);
        check(c1 == c1b, "H2 CONTROL: one session sealing twice is deterministic (so the comparison below means something)", ok);
        check(c1 != c2, "H2: a second session seals the same plaintext/nonce to different bytes (different key)", ok);
        std::vector<std::byte> o;
        check(!s2.resp_result.recv_cipher->open(1, {}, c1, o), "H2: session 2's key cannot open session 1's traffic", ok);
        check(s1.resp_result.recv_cipher->open(1, {}, c1, o) && o == pt, "H2 CONTROL: session 1's own key opens it", ok);
        check(s1.msg1 != s2.msg1 && s1.msg2 != s2.msg2, "H2: the handshake messages differ between sessions", ok);
    }

    // ---- H3: trust change ---------------------------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        check(run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster)).both_done(), "H3: admitted => succeeds", ok);

        // Responder un-admits the initiator: next handshake refused, responder silent.
        check(b.book->revoke(kCluster, a.pub), "H3: revoke reports the removal", ok);
        {
            Outcome o = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
            check(!o.both_done() && o.msg2.empty() && o.resp_step == HandshakeEngine::Step::Failed,
                  "H3: responder un-admitted the initiator => refused, nothing sent back", ok);
        }
        check(b.book->admit(kCluster, a.pub), "H3: re-admit", ok);
        check(run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster)).both_done(),
              "H3: re-admitted => the next handshake succeeds", ok);

        // Initiator un-admits the responder: it sends NOTHING (no encrypted Msg1 to a removed device).
        check(a.book->revoke(kCluster, b.pub), "H3: initiator side revoke", ok);
        {
            Outcome o = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
            check(o.init_step == HandshakeEngine::Step::Failed && o.msg1.empty(),
                  "H3: initiator un-admitted the responder => it emits no bytes at all", ok);
        }
        // Wrong cluster: admitted in kCluster is not admitted in another.
        check(a.book->admit(kCluster, b.pub), "H3: re-admit responder", ok);
        {
            Outcome o = run_pair(*a.engine(true, b.id, ClusterId{1}), *b.engine(false, a.id, ClusterId{1}));
            check(!o.both_done(), "H3: admitted in one cluster is not admitted in another", ok);
        }
        // A revoked fingerprint (the transport's revocation set, here carrying Ed25519 keys) is refused by both ends.
        auto revoked = std::make_shared<std::unordered_set<Fingerprint>>();
        Fingerprint fp{};
        std::memcpy(fp.data(), a.pub.data(), fp.size());
        revoked->insert(fp);
        {
            Outcome o = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster, revoked));
            check(!o.both_done() && o.msg2.empty(), "H3: a revoked initiator key is refused silently by the responder", ok);
        }
        {
            Fingerprint fpb{};
            std::memcpy(fpb.data(), b.pub.data(), fpb.size());
            auto rv2 = std::make_shared<std::unordered_set<Fingerprint>>();
            rv2->insert(fpb);
            Outcome o = run_pair(*a.engine(true, b.id, kCluster, rv2), *b.engine(false, a.id, kCluster));
            check(o.init_step == HandshakeEngine::Step::Failed && o.msg1.empty(), "H3: a revoked responder key is never dialled", ok);
        }
        // Idempotence + collision-safe API of the book.
        check(a.book->admit(kCluster, b.pub) && !a.book->revoke(ClusterId{99}, b.pub),
              "H3: admit is idempotent; revoke of a never-admitted (cluster,key) reports false", ok);
    }

    // ---- H4: man in the middle ----------------------------------------------------------------------------------
    {
        Node a, b, m;  // m = attacker with its own key
        pair_up(a, b, kCluster);
        (void)m.book->admit(kCluster, a.pub);  // the MITM even admits A, as generously as it can
        // A dials "B" (pinned to B's key). The MITM answers instead of B: it holds M's key, not B's.
        auto ie = a.engine(true, b.id, kCluster);
        auto mre = m.engine(false, NodeId{0}, kCluster);
        Outcome o = run_pair(*ie, *mre);
        check(!o.both_done() && o.msg2.empty() && o.resp_step == HandshakeEngine::Step::Failed,
              "H4: a responder with the wrong key cannot even read Msg1, so it says nothing", ok);
        // The MITM fabricates a Msg2 anyway (right length, garbage): the initiator refuses.
        {
            Bytes m1;
            auto ie2 = a.engine(true, b.id, kCluster);
            (void)ie2->advance({}, m1);
            Bytes fake(kPinnedMsg2Len);
            for (std::size_t i = 0; i < fake.size(); ++i) fake[i] = static_cast<std::byte>(i * 31 + 7);
            Bytes sink;
            check(ie2->advance(fake, sink) == HandshakeEngine::Step::Failed, "H4: a fabricated Msg2 is refused by the initiator", ok);
        }
        // The MITM dials B pretending to be A (A's NodeId and key in the payload, its own Noise static signing).
        {
            PinnedIdentity mi = PinnedIdentity::generate();
            adapters::PinnedMsg1Payload p = honest_payload(mi, kCluster, 1'800'000'000'000'000'001LL);
            p.node = a.id;
            p.ed25519 = a.pub;  // claims to be A
            auto re = b.engine(false, NodeId{0}, kCluster);
            const Feed f = feed_responder(*re, craft_msg1(mi, b.pub, p));
            check(f.step == HandshakeEngine::Step::Failed && f.out.empty(),
                  "H4: M claiming A's identity under M's own Noise key is refused silently", ok);
        }
        // CONTROL: the same MITM, honestly pinned by A (A admits M and dials M's NodeId), completes.
        (void)a.book->admit(kCluster, m.pub);
        Outcome c = run_pair(*a.engine(true, m.id, kCluster), *m.engine(false, NodeId{0}, kCluster));
        check(c.both_done(), "H4 CONTROL: pinned by A, the very same node M completes (the refusal above was the key)", ok);
    }

    // ---- H5: the NodeId cannot be chosen --------------------------------------------------------------------------
    {
        Node victim, target;
        PinnedIdentity attacker = PinnedIdentity::generate();
        (void)target.book->admit(kCluster, attacker.public_key());  // the attacker IS admitted: only the NodeId claim is false
        const std::uint64_t ts = 1'800'000'000'000'000'001LL;
        {
            adapters::PinnedMsg1Payload p = honest_payload(attacker, kCluster, ts);
            p.node = victim.id;  // claim another key's NodeId, keep own key
            auto re = target.engine(false, NodeId{0}, kCluster);
            const Feed f = feed_responder(*re, craft_msg1(attacker, target.pub, p));
            check(f.step == HandshakeEngine::Step::Failed && f.out.empty(),
                  "H5: a peer cannot claim another key's NodeId (payload id != derived id)", ok);
        }
        {
            adapters::PinnedMsg1Payload p = honest_payload(attacker, kCluster, ts);
            p.ed25519 = victim.pub;  // carry the victim's key while authenticating with the attacker's static
            p.node = victim.id;
            (void)target.book->admit(kCluster, victim.pub);
            auto re = target.engine(false, NodeId{0}, kCluster);
            const Feed f = feed_responder(*re, craft_msg1(attacker, target.pub, p));
            check(f.step == HandshakeEngine::Step::Failed && f.out.empty(),
                  "H5: carrying an ADMITTED victim key that did not authenticate the message is refused", ok);
        }
        {
            auto re = target.engine(false, NodeId{0}, kCluster);
            const Feed f = feed_responder(*re, craft_msg1(attacker, target.pub, honest_payload(attacker, kCluster, ts)));
            check(f.step == HandshakeEngine::Step::Done && f.out.size() == kPinnedMsg2Len,
                  "H5 CONTROL: the honest payload from the same signer completes", ok);
        }
        {   // expected_peer enforcement (SecureTransport passes the frame's claimed sender)
            auto re = target.engine(false, victim.id, kCluster);
            const Feed f = feed_responder(*re, craft_msg1(attacker, target.pub, honest_payload(attacker, kCluster, ts + 1)));
            check(f.step == HandshakeEngine::Step::Failed && f.out.empty(),
                  "H5: a sender other than the NodeId the caller expected is refused", ok);
        }
    }

    // ---- H6: cross-mode (prologue) ----------------------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        const std::uint64_t ts = 1'800'000'000'000'000'001LL;
        const PinnedIdentity& ai = a.factory->identity();
        {
            auto re = b.engine(false, NodeId{0}, kCluster);
            const Feed f = feed_responder(
                *re, craft_msg1(ai, b.pub, honest_payload(ai, kCluster, ts), pinned_prologue(TrustMode::CaChain)));
            check(f.step == HandshakeEngine::Step::Failed && f.out.empty(),
                  "H6: a Noise peer in CaChain mode (different prologue) never completes with a pinned responder", ok);
        }
        {
            auto re = b.engine(false, NodeId{0}, kCluster);
            const Feed f = feed_responder(
                *re, craft_msg1(ai, b.pub, honest_payload(ai, kCluster, ts), pinned_prologue(TrustMode::PinnedKey)));
            check(f.step == HandshakeEngine::Step::Done, "H6 CONTROL: same mode completes", ok);
        }
        check(pinned_prologue(TrustMode::CaChain) != pinned_prologue(TrustMode::PinnedKey), "H6: prologues differ per mode", ok);
    }

    // ---- H7: payload gates ---------------------------------------------------------------------------------------------
    {
        TestClock clk;
        PinnedOptions opt;
        opt.clock_unix_ns = clk.fn();
        Node a(opt), b(opt);
        pair_up(a, b, kCluster);
        const PinnedIdentity& ai = a.factory->identity();
        const std::uint64_t now = static_cast<std::uint64_t>(clk.now->load());
        auto attempt = [&](adapters::PinnedMsg1Payload p) {
            auto re = b.engine(false, NodeId{0}, kCluster);
            return feed_responder(*re, craft_msg1(ai, b.pub, p));
        };
        adapters::PinnedMsg1Payload p = honest_payload(ai, kCluster, now);
        check(attempt(p).step == HandshakeEngine::Step::Done, "H7 CONTROL: an honest, fresh, in-window Msg1 completes", ok);
        p = honest_payload(ai, kCluster, now + 1);
        p.version = 2;  // within the window of 1
        check(attempt(p).step == HandshakeEngine::Step::Done, "H7: version N+1 is inside the window", ok);
        p = honest_payload(ai, kCluster, now + 2);
        p.version = 3;
        check(attempt(p).step == HandshakeEngine::Step::Failed, "H7: version N+2 is outside the window => silent refusal", ok);
        p = honest_payload(ai, ClusterId{1}, now + 3);
        check(attempt(p).step == HandshakeEngine::Step::Failed, "H7: a Msg1 for another cluster/scope is refused", ok);
        p = honest_payload(ai, kCluster, now + 4 + 121ULL * 1'000'000'000ULL);
        check(attempt(p).step == HandshakeEngine::Step::Failed, "H7: a timestamp 121 s in the future is refused", ok);
        p = honest_payload(ai, kCluster, now - 121ULL * 1'000'000'000ULL);
        check(attempt(p).step == HandshakeEngine::Step::Failed, "H7: a timestamp 121 s in the past is refused", ok);
        p = honest_payload(ai, kCluster, now + 5);
        check(attempt(p).step == HandshakeEngine::Step::Done, "H7 CONTROL: back in the window it completes", ok);
    }
    {   // Monotonic: the engine's own timestamps strictly increase even with a frozen clock.
        Node a, b;
        pair_up(a, b, kCluster);
        const auto o1 = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
        const auto o2 = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
        check(o1.both_done() && o2.both_done(), "H7: two honest handshakes under a frozen clock both complete (timestamps strictly increase)", ok);
    }

    // ---- H8: a NodeId is never self-claimed ---------------------------------------------------------------------------------
    {
        Node a, b;
        pair_up(a, b, kCluster);
        auto bad = a.factory->create(true, NodeId{a.id.value ^ 1}, b.id, kCluster, {}, {}, nullptr);
        Bytes out;
        check(bad->advance({}, out) == HandshakeEngine::Step::Failed && out.empty(),
              "H8: an engine whose self NodeId is not derived from its key emits nothing and fails", ok);
        auto good = a.factory->create(true, a.id, b.id, kCluster, {}, {}, nullptr);
        check(good->advance({}, out) == HandshakeEngine::Step::WantWrite, "H8 CONTROL: the derived id works", ok);
    }

    // ---- H9: forward secrecy and its documented limit --------------------------------------------------------------------
    {
        // Raw Noise with FIXED ephemerals so the comparison is exact. The attacker holds BOTH long-term secrets and the whole
        // capture. Run as the responder with a FRESH ephemeral it derives other keys (the real e_r is gone); with the real e_r
        // (the control: what a compromise of the live session would give) it derives the same keys.
        const noise::KeyPair si = [] { noise::KeyPair k; ::randombytes_buf(k.sk.data(), 32); k.pk = noise::public_from_private(k.sk); return k; }();
        const noise::KeyPair sr = [] { noise::KeyPair k; ::randombytes_buf(k.sk.data(), 32); k.pk = noise::public_from_private(k.sk); return k; }();
        noise::Key32 ei{}, er{}, er_other{};
        ::randombytes_buf(ei.data(), 32);
        ::randombytes_buf(er.data(), 32);
        ::randombytes_buf(er_other.data(), 32);
        const Bytes pro = pinned_prologue();
        const Bytes pl = bytes_of("payload");
        auto fixed = [](noise::Key32 e) { return [e](noise::Key32& sk) { sk = e; }; };
        noise::IkHandshake ini(noise::Role::Initiator, pro, si, &sr.pk, fixed(ei));
        noise::IkHandshake rsp(noise::Role::Responder, pro, sr, nullptr, fixed(er));
        Bytes m1, m2, got;
        check(ini.write_msg1(pl, m1) && rsp.read_msg1(m1, got) && rsp.write_msg2(pl, m2) && ini.read_msg2(m2, got), "H9: raw handshake", ok);
        noise::CipherState real_send, real_recv;
        check(ini.split(real_send, real_recv), "H9: split", ok);

        auto attacker_key = [&](const noise::Key32& e) {
            noise::IkHandshake atk(noise::Role::Responder, pro, sr /*stolen*/, nullptr, fixed(e));
            Bytes g, o;
            const bool good = atk.read_msg1(m1, g) && atk.write_msg2(pl, o);
            noise::CipherState s, r;
            const bool sp = good && atk.split(s, r);
            noise::Key32 k{};
            if (sp) std::memcpy(k.data(), r.key().data(), k.size());  // copy: key() borrows the cipher's storage
            return std::pair{sp, k};  // responder recv == initiator send direction
        };
        const auto same_key = [](const noise::Key32& a, std::span<const std::byte> b) {
            return b.size() == a.size() && std::equal(a.begin(), a.end(), b.begin());
        };
        const auto fresh = attacker_key(er_other);
        check(fresh.first, "H9: with the stolen static key the attacker CAN process the captured Msg1", ok);
        check(!same_key(fresh.second, real_send.key()), "H9: ...but with a fresh ephemeral it derives DIFFERENT session keys (forward secrecy)", ok);
        const auto leaked = attacker_key(er);
        check(leaked.first && same_key(leaked.second, real_send.key()),
              "H9 CONTROL: with the real responder ephemeral (which is erased after the handshake) the same code derives the SAME keys", ok);
    }
    {
        // Documented limit: Msg1 is encrypted to B's STATIC key, so a later compromise of B reveals which key dialled.
        Node a, b;
        pair_up(a, b, kCluster);
        Outcome o = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
        check(o.both_done(), "H9 limit: session established", ok);
        auto stolen_id = PinnedIdentity::from_secret_key(b.factory->identity().secret_key());
        auto open_book = std::make_shared<PinnedKeyBook>();
        (void)open_book->admit(kCluster, a.pub);
        PinnedOptions attacker_opt;
        attacker_opt.max_clock_skew_ns = 0;
        PinnedHandshakeEngineFactory attacker(std::move(*stolen_id), open_book, nullptr, attacker_opt);
        auto re = attacker.create(false, b.id, NodeId{0}, kCluster, {}, {}, nullptr);
        Bytes reply;
        check(re->advance(o.msg1, reply) == HandshakeEngine::Step::Done, "H9 limit: with B's long-term key the captured Msg1 is readable", ok);
        auto stolen = re->take_result();
        check(std::equal(a.pub.begin(), a.pub.end(), stolen.peer_fingerprint.begin()),
              "H9 DOCUMENTED LIMIT: a later compromise of B reveals which key dialled it (not any session traffic)", ok);
    }

    // ---- H10: the red-team's findings, as checks ----------------------------------------------------------------------------
    {   // sign-bit malleability: A and -A share an X25519 image; the book refuses to hold both.
        Node a;
        Ed25519PublicKey neg = a.pub;
        neg[31] ^= std::byte{0x80};
        noise::Key32 xa{}, xn{};
        const bool conv = ed25519_to_x25519(a.pub, xa) && ed25519_to_x25519(neg, xn);
        check(conv && xa == xn, "H10: A and -A convert to the same X25519 key (the malleability exists)", ok);
        PinnedKeyBook book;
        check(book.admit(kCluster, a.pub) && !book.admit(kCluster, neg), "H10: the key book refuses to admit -A beside A", ok);
        PinnedKeyBook other;
        check(other.admit(kCluster, neg), "H10 CONTROL: -A alone is admissible (it is the pair that is refused)", ok);
    }
    {   // timestamp overflow: an absurd skew must not overflow (UBSan would catch it)
        PinnedOptions opt;
        opt.max_clock_skew_ns = INT64_MAX;
        Node a(opt), b(opt);
        pair_up(a, b, kCluster);
        check(run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster)).both_done(), "H10: skew = INT64_MAX is handled without overflow", ok);
    }
    {   // a partial Msg1 is refused at once, never parked
        Node a, b;
        pair_up(a, b, kCluster);
        auto re = b.engine(false, a.id, kCluster);
        const Feed f = feed_responder(*re, Bytes(1, std::byte{7}));
        check(f.step == HandshakeEngine::Step::Failed && f.out.empty(), "H10: a 1-byte 'Msg1' is refused immediately (nothing is parked)", ok);
    }
    {   // DOCUMENTED LIMIT: a responder restart forgets the replay record; a captured Msg1 inside the skew window is accepted once.
        TestClock clk;
        PinnedOptions opt;
        opt.clock_unix_ns = clk.fn();
        Node a(opt), b(opt);
        pair_up(a, b, kCluster);
        Outcome o = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
        auto again = b.engine(false, a.id, kCluster);
        check(feed_responder(*again, o.msg1).step == HandshakeEngine::Step::Failed, "H10: replay to the SAME responder is refused", ok);
        // "restart": a responder with the same identity key but an empty record.
        auto b_id = PinnedIdentity::from_secret_key(b.factory->identity().secret_key());
        PinnedHandshakeEngineFactory restarted(std::move(*b_id), b.book, b.book, opt);
        auto re2 = restarted.create(false, b.id, a.id, kCluster, {}, {}, nullptr);
        check(feed_responder(*re2, o.msg1).step == HandshakeEngine::Step::Done,
              "H10 DOCUMENTED LIMIT: after a responder restart the captured Msg1 is accepted ONCE inside the skew window", ok);
    }
    {   // DOCUMENTED LIMIT: an initiator restart (counter lost) with a clock behind its previous Msg1 is refused until it catches up.
        TestClock clk;
        PinnedOptions opt;
        opt.clock_unix_ns = clk.fn();
        Node a(opt), b(opt);
        pair_up(a, b, kCluster);
        clk.advance_s(100);
        check(run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster)).both_done(), "H10: handshake at t+100 s", ok);
        clk.advance_s(-100);  // the clock steps back (NTP) and the initiator's counter is rebuilt
        auto a_id = PinnedIdentity::from_secret_key(a.factory->identity().secret_key());
        PinnedHandshakeEngineFactory a2(std::move(*a_id), a.book, a.book, opt);
        auto ie = a2.create(true, a.id, b.id, kCluster, {}, {}, nullptr);
        auto re = b.engine(false, a.id, kCluster);
        check(!run_pair(*ie, *re).both_done(),
              "H10 DOCUMENTED LIMIT: an initiator whose clock/counter went backwards is refused until the clock catches up", ok);
    }

    // ---- #78: the factory's read-only posture -----------------------------------------------------------------
    {
        PinnedOptions opt;
        opt.version = 7;
        opt.version_window = 0;
        opt.max_clock_skew_ns = 12'345;
        opt.replay_capacity = 17;
        opt.replay_guard = false;
        opt.check_role_bit = false;
        opt.node_id_derivation = NodeIdDerivation::LegacyRaw;
        opt.test_copy_static_key = true;
        Node a(opt);
        const auto p = a.factory->posture();
        check(p.trust_mode == TrustMode::PinnedKey && a.factory->trust_mode() == TrustMode::PinnedKey,
              "#78: the pinned factory reports PinnedKey", ok);
        check(!a.factory->is_test_double(), "#78: a real factory is not a test double", ok);
        check(p.version == 7 && p.version_window == 0 && p.max_clock_skew_ns == 12'345 &&
                  p.replay_capacity == 17 && !p.replay_guard && !p.check_role_bit &&
                  p.node_id_derivation == NodeIdDerivation::LegacyRaw && p.test_copy_static_key &&
                  p.clock_injected,
              "#78: every PinnedOptions field reads back", ok);
        // CONTROL: a default factory reads back the documented defaults, not the values above.
        Node d{PinnedOptions{}};
        const auto q = d.factory->posture();
        check(q.version == kPinnedVersion && q.version_window == 1 && q.max_clock_skew_ns == 120'000'000'000 &&
                  q.replay_capacity == 4096 && q.replay_guard && q.check_role_bit &&
                  q.node_id_derivation == NodeIdDerivation::RoleBit && !q.test_copy_static_key &&
                  q.clock_injected,
              "#78 CONTROL: a default factory reads back the documented defaults", ok);
        PinnedHandshakeEngineFactory no_clock(PinnedIdentity::generate(PinnedRole::Member, NodeIdDerivation::RoleBit),
                                              nullptr, nullptr, PinnedOptions{});
        check(!no_clock.posture().clock_injected, "#78: an un-injected clock reads back as not injected", ok);
    }

    std::printf(ok ? "pinned_handshake_check: OK\n" : "pinned_handshake_check: FAILED\n");
    return ok ? 0 : 1;
}
