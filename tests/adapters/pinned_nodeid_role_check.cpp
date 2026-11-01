// Issue #63 / ADR-075: the pinned NodeId's bit 63 is the ROLE the application admitted the key in, not a random bit of the key
// hash. ADR-068 derived all 64 bits from SHA-256, so about half of all pinned keys landed on ADR-074 D3's client bit and were
// treated as clients (never dialled by a member, role rules for SWIM, relay and placement). Each property has a control that
// shows the old behaviour.
//   T1  two MEMBERS whose raw SHA-256 bit 63 differs (>= 50 key pairs, real TCP + SecureTransport + Noise IK on the pinned wire)
//       link and deliver both ways, and neither is a client. CONTROL: the same keys under the legacy derivation
//       (NodeIdDerivation::LegacyRaw): the one with the raw bit set is classed a client, the member never dials it, no link.
//   T2  a NodeId whose bit 63 disagrees with the role the verifier's trust data holds for the (authenticated) key is refused
//       with zero bytes, in both directions (a Member key presenting bit 63 set, a Client key presenting it clear; and an
//       initiator that holds the peer as the other role sends nothing). CONTROL: with the role check off it is accepted.
//       Also the book's role policy: one role per key per cluster, re-admission in the other role refused.
//   T3  a device admitted as CLIENT (the device-attach shape) gets the client role everywhere ADR-074 applies it: the client
//       dials and a member never dials it (real TCP), SWIM refuses it, placement never picks it. CONTROL: the same key
//       admitted as Member is a member in all of those.
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" as amended by ADR-075; ADR-074 D3 (unchanged).
#include <algorithm>
#include <cstdio>

#include "pinned_tcp_util.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/node_authority.hpp"
#include "quark/core/transport.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster = kPinnedTcpCluster;

[[nodiscard]] bool raw_bit63(const Ed25519PublicKey& k) {
    return is_client_node(derive_pinned_node_id(k, PinnedRole::Member, NodeIdDerivation::LegacyRaw));
}

// A fresh key as a (secret key) so the same key can be loaded under different roles / derivations.
struct Key {
    std::array<std::byte, PinnedIdentity::kSecretKeyBytes> sk{};
    Ed25519PublicKey pub{};
    bool raw63 = false;
};
[[nodiscard]] Key fresh_key() {
    const PinnedIdentity id = PinnedIdentity::generate();
    Key k;
    std::copy(id.secret_key().begin(), id.secret_key().end(), k.sk.begin());
    k.pub = id.public_key();
    k.raw63 = raw_bit63(k.pub);
    return k;
}
[[nodiscard]] PinnedIdentity load(const Key& k, PinnedRole role, NodeIdDerivation how = NodeIdDerivation::RoleBit) {
    return std::move(*PinnedIdentity::from_secret_key(std::span<const std::byte, PinnedIdentity::kSecretKeyBytes>(k.sk), role, how));
}
[[nodiscard]] std::unique_ptr<Node> node_of(const Key& k, PinnedRole role, bool legacy = false) {
    PinnedOptions o;
    if (legacy) o.node_id_derivation = NodeIdDerivation::LegacyRaw;
    return std::make_unique<Node>(load(k, role, o.node_id_derivation), o);
}

// Run a (lower id) -> b (higher id) session over real TCP; true iff data flowed both ways and each side holds one session.
struct Link {
    bool a_to_b = false, b_to_a = false;
    std::uint64_t sessions_a = 0, sessions_b = 0;
};
Link link(Node& a, Node& b, int timeout_ms) {
    Link l;
    Stack sb(b, true);
    if (!sb.start()) return l;
    Stack sa(a, true);
    if (!sa.start()) return l;
    sa.dial_via(b.id, sb.tcp.listen_port());
    l.a_to_b = deliver(sa, sb, b.id, 2, "to b", timeout_ms);
    l.b_to_a = l.a_to_b && deliver(sb, sa, a.id, 2, "to a", timeout_ms);
    l.sessions_a = sa.sec.session_count();
    l.sessions_b = sb.sec.session_count();
    return l;
}
// Order two nodes by id: the lower one dials (the repo's convention for the pinned checks).
void order(std::unique_ptr<Node>& x, std::unique_ptr<Node>& y) {
    if (y->id.value < x->id.value) std::swap(x, y);
}

[[nodiscard]] std::shared_ptr<const std::vector<NodeId>> sorted_nodes(NodeId x, NodeId y) {
    std::vector<NodeId> v{x, y};
    std::sort(v.begin(), v.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
    return std::make_shared<const std::vector<NodeId>>(std::move(v));
}

PinnedMsg1Payload payload_for(const PinnedIdentity& id, std::uint64_t node, std::uint64_t ts) {
    PinnedMsg1Payload p = honest_payload(id, kCluster, ts);
    p.node = NodeId{node};
    return p;
}
std::uint64_t g_ts = 1'800'000'000'000'000'000ULL;
// Feed a crafted Msg1 from `signer` (claiming `claimed`) to a fresh responder engine of `r`.
Feed craft_to(Node& r, const PinnedIdentity& signer, std::uint64_t claimed) {
    const Bytes m1 = craft_msg1(signer, r.pub, payload_for(signer, claimed, ++g_ts));
    auto eng = r.engine(false, NodeId{0}, kCluster);
    return feed_responder(*eng, m1);
}
}  // namespace

int main() {
    bool ok = true;
    if (!adapters::noise::init()) return 1;

    // ---------------- T1 ----------------
    {
        constexpr int kPairs = 50;
        int linked = 0, member_ids = 0, mixed_seen = 0;
        std::vector<std::pair<Key, Key>> pairs;
        for (int i = 0; i < kPairs; ++i) {
            Key x = fresh_key(), y = fresh_key();
            while (y.raw63 == x.raw63) y = fresh_key();  // mixed pair: the raw bit 63 differs
            ++mixed_seen;
            pairs.emplace_back(x, y);
            auto a = node_of(x, PinnedRole::Member), b = node_of(y, PinnedRole::Member);
            order(a, b);
            member_ids += (!is_client_node(a->id) && !is_client_node(b->id)) ? 1 : 0;
            pair_up(*a, *b, kCluster);
            const Link l = link(*a, *b, 5000);
            if (l.a_to_b && l.b_to_a && l.sessions_a == 1 && l.sessions_b == 1) ++linked;
        }
        std::printf("T1: %d mixed-raw-bit pairs, %d linked both ways, %d with both ids member-class\n", mixed_seen, linked, member_ids);
        check(linked == kPairs, "T1: every mixed-bit member pair links and delivers both ways (63-bit derivation)", ok);
        check(member_ids == kPairs, "T1: no member is treated as a client", ok);

        // CONTROL: the very same keys under the legacy derivation.
        int control_linked = 0, control_client_classed = 0;
        constexpr int kControl = 4;
        for (int i = 0; i < kControl; ++i) {
            auto a = node_of(pairs[static_cast<std::size_t>(i)].first, PinnedRole::Member, true);
            auto b = node_of(pairs[static_cast<std::size_t>(i)].second, PinnedRole::Member, true);
            order(a, b);
            control_client_classed += (is_client_node(a->id) || is_client_node(b->id)) ? 1 : 0;
            pair_up(*a, *b, kCluster);
            const Link l = link(*a, *b, 1000);
            if (l.a_to_b && l.b_to_a) ++control_linked;
        }
        std::printf("T1 CONTROL (legacy derivation): %d pairs, %d classed a client, %d linked\n", kControl, control_client_classed, control_linked);
        check(control_client_classed == kControl, "T1 CONTROL: under the legacy derivation one key of every mixed pair is classed a client", ok);
        check(control_linked == 0, "T1 CONTROL: under the legacy derivation the lower (member) side never dials the client-classed one: no link", ok);
    }

    // ---------------- T2 ----------------
    {
        Node resp;  // a Member node; its book will hold a Member key and a Client key
        PinnedIdentity m = PinnedIdentity::generate(PinnedRole::Member);
        PinnedIdentity c = PinnedIdentity::generate(PinnedRole::Client);
        check(resp.book->admit(kCluster, m.public_key(), PinnedRole::Member), "T2: admit member key", ok);
        check(resp.book->admit(kCluster, c.public_key(), PinnedRole::Client), "T2: admit client key", ok);
        check(!is_client_node(m.node_id()) && is_client_node(c.node_id()), "T2: identities carry the role in bit 63", ok);
        check(pinned_key_bits(m.node_id()) == pinned_key_bits(derive_pinned_node_id(m.public_key(), PinnedRole::Client)),
              "T2: the 63 key bits are the same in either role", ok);

        // Honest controls first: the right bit is accepted.
        check(craft_to(resp, m, m.node_id().value).step == HandshakeEngine::Step::Done, "T2: member key + member id is accepted", ok);
        check(craft_to(resp, c, c.node_id().value).step == HandshakeEngine::Step::Done, "T2: client key + client id is accepted", ok);
        // The attacks: the right 63 bits, the wrong role bit.
        const Feed fm = craft_to(resp, m, m.node_id().value | kClientNodeBit);
        check(fm.step == HandshakeEngine::Step::Failed && fm.out.empty(), "T2: a Member key presenting bit 63 set is refused with ZERO bytes", ok);
        const Feed fc = craft_to(resp, c, c.node_id().value & ~kClientNodeBit);
        check(fc.step == HandshakeEngine::Step::Failed && fc.out.empty(), "T2: a Client key presenting bit 63 clear is refused with ZERO bytes", ok);
        check(fm.reason.find("role bit") != std::string::npos && fc.reason.find("role bit") != std::string::npos,
              "T2: both refusals are the role check (not some other check)", ok);
        // The low 63 bits still have to match the key.
        check(craft_to(resp, m, m.node_id().value ^ 1).step == HandshakeEngine::Step::Failed, "T2: wrong key bits are still refused", ok);

        // CONTROL: role check off (test knob): the same two messages are accepted.
        PinnedOptions off;
        off.check_role_bit = false;
        Node lax(off);
        (void)lax.book->admit(kCluster, m.public_key(), PinnedRole::Member);
        (void)lax.book->admit(kCluster, c.public_key(), PinnedRole::Client);
        check(craft_to(lax, m, m.node_id().value | kClientNodeBit).step == HandshakeEngine::Step::Done,
              "T2 CONTROL: with the role check off a Member key presenting bit 63 set is accepted", ok);
        check(craft_to(lax, c, c.node_id().value & ~kClientNodeBit).step == HandshakeEngine::Step::Done,
              "T2 CONTROL: with the role check off a Client key presenting bit 63 clear is accepted", ok);

        // Initiator side: it holds P as a Member but P's id says Client: the directory finds no key and NOTHING is sent.
        Node peer(PinnedOptions{}, nullptr, PinnedRole::Client);
        Node ini;
        (void)ini.book->admit(kCluster, peer.pub, PinnedRole::Member);  // the application's trust data says Member
        {
            auto e = ini.engine(true, peer.id, kCluster);
            Bytes out;
            check(e->advance({}, out) == HandshakeEngine::Step::Failed && out.empty(),
                  "T2: an initiator that holds the peer as Member refuses to dial a Client-bit id, sending nothing", ok);
        }
        Node ini2;
        (void)ini2.book->admit(kCluster, peer.pub, PinnedRole::Client);
        {
            auto e = ini2.engine(true, peer.id, kCluster);
            Bytes out;
            check(e->advance({}, out) == HandshakeEngine::Step::WantWrite && out.size() == kPinnedMsg1Len,
                  "T2 CONTROL: holding the peer as Client, the same initiator sends Msg1", ok);
        }
        // Whole handshake, honest, both roles, in memory.
        {
            Node a, d(PinnedOptions{}, nullptr, PinnedRole::Client);
            pair_up(a, d, kCluster);
            auto ie = a.engine(true, d.id, kCluster);
            auto re = d.engine(false, NodeId{0}, kCluster);
            const Outcome o = run_pair(*ie, *re);
            check(o.both_done() && is_client_node(o.init_result.peer_node_id) && !is_client_node(o.resp_result.peer_node_id),
                  "T2: an honest member<->client handshake completes and each side learns the other's role bit", ok);
        }

        // The book's role policy.
        {
            PinnedKeyBook b;
            const PinnedIdentity k = PinnedIdentity::generate();
            check(b.admit(kCluster, k.public_key(), PinnedRole::Member), "book: admit as Member", ok);
            check(b.admit(kCluster, k.public_key(), PinnedRole::Member), "book: re-admitting in the same role is a no-op success", ok);
            check(!b.admit(kCluster, k.public_key(), PinnedRole::Client), "book: admitting the same key in the OTHER role is refused", ok);
            check(b.role_of(kCluster, k.public_key()) == PinnedRole::Member, "book: ... and the original role is unchanged", ok);
            const NodeId mid = derive_pinned_node_id(k.public_key(), PinnedRole::Member);
            const NodeId cid = derive_pinned_node_id(k.public_key(), PinnedRole::Client);
            check(b.key_of(kCluster, mid).has_value() && !b.key_of(kCluster, cid).has_value(),
                  "book: key_of answers only for the id whose bit 63 is the admitted role", ok);
            check(b.revoke(kCluster, k.public_key()) && b.admit(kCluster, k.public_key(), PinnedRole::Client),
                  "book: a role change is revoke() then admit()", ok);
            check(b.key_of(kCluster, cid).has_value() && !b.key_of(kCluster, mid).has_value() &&
                      b.role_of(kCluster, k.public_key()) == PinnedRole::Client,
                  "book: after the change the Client id resolves and the Member id does not", ok);
            check(b.admitted(kCluster, k.public_key()) && !b.admitted(ClusterId{1}, k.public_key()), "book: admitted() ignores the role, not the cluster", ok);
            // Roles are per cluster.
            check(b.admit(ClusterId{2}, k.public_key(), PinnedRole::Member) && b.role_of(ClusterId{2}, k.public_key()) == PinnedRole::Member,
                  "book: the same key may hold another role in another cluster", ok);
            // 63-bit key slots: the Member id and the Client id of one key address one slot (so a colliding second key is
            // refused whatever role either holds); the only difference between them is the role bit.
            check(pinned_key_bits(mid) == pinned_key_bits(cid) && mid.value != cid.value, "book: one key, one 63-bit slot", ok);
        }
    }

    // ---------------- T3 ----------------
    {
        const Key kd = fresh_key();  // the device key
        const Key km = fresh_key();  // a member
        auto member = node_of(km, PinnedRole::Member);
        auto device = node_of(kd, PinnedRole::Client);
        check(is_client_node(device->id) && !is_client_node(member->id), "T3: the device is client-class, the member is not", ok);
        pair_up(*member, *device, kCluster);
        check(keep_local_dial(device->id, member->id) && !keep_local_dial(member->id, device->id),
              "T3: the dial rule keeps the client's dial and never the member's", ok);

        // (a) the client dials: works whatever the numeric ids; the member's session is under the client-class id.
        {
            Stack sm(*member, true);
            (void)sm.start();
            Stack sd(*device, true);
            (void)sd.start();
            sd.dial_via(member->id, sm.tcp.listen_port());
            check(deliver(sd, sm, member->id, 2, "device to member"), "T3: the client dials the member and delivers", ok);
            check(deliver(sm, sd, device->id, 2, "member to device"), "T3: the member answers on the client's connection", ok);
            check(sm.sec.session_count() == 1 && sd.sec.session_count() == 1, "T3: one session each side", ok);
        }
        // (b) a member never dials a client: nothing forms even though the client's listener is up and the key is admitted.
        {
            Stack sd(*device, true);
            (void)sd.start();
            Stack sm(*member, true);
            (void)sm.start();
            sm.dial_via(device->id, sd.tcp.listen_port());
            check(!deliver(sm, sd, device->id, 1, "member dials client", 1200), "T3: a member never dials a client: no delivery", ok);
            check(sm.sec.session_count() == 0 && sd.sec.session_count() == 0, "T3: ... and no session on either side", ok);
        }
        // (c) SWIM and placement.
        {
            LoopbackFabric fabric;
            SwimMembership::Config cfg;
            cfg.cluster_id = kCluster;
            LoopbackTransport t1(fabric, member->id);
            SwimMembership s1(member->id, t1, cfg);
            check(!s1.admit(device->id, kCluster) && !s1.view().contains(device->id), "T3: SWIM refuses the client-admitted device", ok);
            const MembershipView forged(sorted_nodes(member->id, device->id), 1);
            AdmitAllNodeAuthority all;
            std::vector<NodeId> eligible;
            admitted_nodes(forged, all, eligible);
            check(eligible.size() == 1 && eligible[0] == member->id, "T3: placement never makes the client-admitted device eligible", ok);
        }
        // CONTROL: the very same device key admitted as a MEMBER is a member in all of it.
        {
            auto dm = node_of(kd, PinnedRole::Member);
            auto mm = node_of(km, PinnedRole::Member);
            check(!is_client_node(dm->id), "T3 CONTROL: the same key admitted as Member has a member id", ok);
            order(mm, dm);
            pair_up(*mm, *dm, kCluster);
            const Link l = link(*mm, *dm, 5000);
            check(l.a_to_b && l.b_to_a, "T3 CONTROL: as a Member it links by the ordinary member rule (lower id dials)", ok);
            LoopbackFabric fabric;
            SwimMembership::Config cfg;
            cfg.cluster_id = kCluster;
            LoopbackTransport t1(fabric, mm->id);
            SwimMembership s1(mm->id, t1, cfg);
            check(s1.admit(dm->id, kCluster) && s1.view().contains(dm->id), "T3 CONTROL: SWIM admits it as a member", ok);
            const MembershipView v(sorted_nodes(mm->id, dm->id), 1);
            AdmitAllNodeAuthority all;
            std::vector<NodeId> eligible;
            admitted_nodes(v, all, eligible);
            check(eligible.size() == 2, "T3 CONTROL: placement counts it as eligible", ok);
        }
    }

    std::printf(ok ? "pinned_nodeid_role_check: OK\n" : "pinned_nodeid_role_check: FAILED\n");
    return ok ? 0 : 1;
}
