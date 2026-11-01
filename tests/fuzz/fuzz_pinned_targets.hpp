// Implements issue #50 (assessment M3): fuzz targets for the parsers that run on untrusted bytes BEFORE any
// authentication in the pinned-key stack (ADR-068 Noise IK, ADR-072 pinned wire, ADR-070 discovery). Included by
// fuzz_targets.hpp, so BOTH drivers (fuzz_decoders_test, fuzz_decoders) run them on the same inputs.
//
//   1. run_pinned_noise      the real PinnedHandshakeEngine (responder fed arbitrary bytes as Msg1, initiator fed
//                            arbitrary bytes as Msg2) and the noise-layer initiator. Needs libsodium: compiled only
//                            when QUARK_FUZZ_SODIUM is defined (CMake: QUARK_WITH_SODIUM=ON), skipped cleanly otherwise.
//                            Static keys are fixed seeds; the only randomness is an engine's own ephemeral key, which an
//                            input cannot depend on. Input byte 0 selects an extra mode (below).
//   2. run_anonymous_wire    arbitrary bytes arriving on an anonymous accepted connection of the pinned wire:
//                            net::FrameStream + the shared net::anonymous_opener_admissible rule (the very function
//                            TcpTransport's receive loop calls), cut into recv() chunks by the input's first byte.
//   3. run_discovery_decode  base32 instance-label decode, the exact-match accept lists and the sighting fields, through
//                            LanDiscovery over the in-memory fake platform with arbitrary strings.
//   0. control_msg1_parse_copy  a COPY of the Msg1 length/prefix parse path with an out-of-bounds read planted behind
//                            QUARK_FUZZ_PLANT_OOB. It exists to prove the campaign can find a real overread.
//
// Properties asserted (abort on violation: a failed property is a finding, never "flaky"):
//   * a responder that refuses emits NO bytes; one that completes emits exactly 58 bytes, and only for a payload that is
//     honest (right key, derived NodeId, right cluster, version in window, fresh timestamp);
//   * the initiator never completes on a Msg2 that was not produced for its ephemeral key;
//   * the anonymous gate accepts at most one frame, and an accepted frame is an Authenticate with from == to == 0 and at
//     most 1024 payload bytes;
//   * discovery reports only a sighting whose decoded label EQUALS an accepted id (no prefix matching), and the label
//     decoder is the strict inverse of the encoder.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quark/net/lan_discovery.hpp"
#include "quark/net/lan_discovery_fake.hpp"
#include "quark/net/wire_codec.hpp"

#ifdef QUARK_FUZZ_SODIUM
#include "quark/adapters/sodium/pinned_handshake.hpp"
#include "quark/adapters/sodium/pinned_identity.hpp"
#endif

namespace quark::fuzz {

[[noreturn]] inline void fuzz_fail(const char* what) {
    std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", what);
    std::abort();
}

// ---- 0. The planted control ---------------------------------------------------------------------------------------
// A copy of the Msg1 prefix parse: it works on an exact-size heap copy, so a one-byte overread is observable by ASan
// whatever buffer the driver hands in (a reused std::vector with spare capacity would hide it). The planted read exists
// only when QUARK_FUZZ_PLANT_OOB is defined; without it this is a plain in-bounds read of the ephemeral key field.
inline void control_msg1_parse_copy(const std::byte* p, std::size_t n) {
    constexpr std::size_t kMsg1 = 154;  // ADR-068: fixed Msg1 length
    if (n != kMsg1) return;
    const std::unique_ptr<std::byte[]> exact(new std::byte[n]);
    std::memcpy(exact.get(), p, n);
    unsigned acc = 0;
    for (std::size_t i = 0; i < 32; ++i) acc += static_cast<unsigned>(exact[i]);  // the ephemeral public key field
#ifdef QUARK_FUZZ_PLANT_OOB
    if ((static_cast<unsigned>(exact[0]) & 0xF0u) == 0xF0u) acc += static_cast<unsigned>(exact[n]);  // PLANTED: one byte past the buffer
#endif
    [[maybe_unused]] static volatile unsigned sink;  // the store keeps `acc` (and the parse) alive
    sink = acc;
}

// ---- 1. Pinned Noise IK ---------------------------------------------------------------------------------------------
#ifdef QUARK_FUZZ_SODIUM
namespace pinned_detail {

using adapters::PinnedHandshakeEngineFactory;
using adapters::PinnedIdentity;
using adapters::PinnedKeyBook;
using adapters::PinnedOptions;
using adapters::noise::Bytes;
using adapters::noise::Key32;

inline constexpr std::int64_t kClockNs = 1'800'000'000'000'000'000LL;

[[nodiscard]] inline PinnedIdentity identity_from_seed(std::uint8_t tag) {
    unsigned char seed[32], pk[32], sk[64];
    std::memset(seed, tag, sizeof seed);
    ::crypto_sign_seed_keypair(pk, sk, seed);
    auto id = PinnedIdentity::from_secret_key(
        std::span<const std::byte, PinnedIdentity::kSecretKeyBytes>(reinterpret_cast<const std::byte*>(sk), 64));
    if (!id) fuzz_fail("fixed test identity rejected");
    return std::move(*id);
}

[[nodiscard]] inline adapters::noise::IkHandshake::EphemeralSource fixed_ephemeral(std::uint8_t tag) {
    return [tag](Key32& sk) { sk.fill(static_cast<std::byte>(tag)); };
}

struct Fixture {
    ClusterId cluster{4242};
    std::shared_ptr<PinnedKeyBook> init_book = std::make_shared<PinnedKeyBook>();
    std::shared_ptr<PinnedKeyBook> resp_book = std::make_shared<PinnedKeyBook>();
    std::unique_ptr<PinnedHandshakeEngineFactory> init_f, resp_f;  // A dials B
    NodeId a_id{}, b_id{};
    Ed25519PublicKey a_pub{}, b_pub{};
    Bytes valid_msg1, valid_msg2;

    Fixture() {
        if (!adapters::noise::init()) fuzz_fail("libsodium unavailable");
        PinnedOptions o;
        o.clock_unix_ns = [] { return kClockNs; };
        PinnedOptions ro = o;
        ro.replay_guard = false;  // one static responder serves every input; replay is not what is fuzzed here
        PinnedIdentity a = identity_from_seed(0xA1), b = identity_from_seed(0xB2);
        a_id = a.node_id();
        b_id = b.node_id();
        a_pub = a.public_key();
        b_pub = b.public_key();
        (void)init_book->admit(cluster, b_pub);
        (void)resp_book->admit(cluster, a_pub);
        init_f = std::make_unique<PinnedHandshakeEngineFactory>(std::move(a), init_book, init_book, o);
        resp_f = std::make_unique<PinnedHandshakeEngineFactory>(std::move(b), resp_book, resp_book, ro);

        // A valid Msg1 and Msg2 from deterministic ephemerals (the same construction the engine uses).
        Key32 rs{};
        if (!adapters::ed25519_to_x25519(b_pub, rs)) fuzz_fail("responder key conversion");
        adapters::noise::IkHandshake ini(adapters::noise::Role::Initiator, adapters::pinned_prologue(),
                                         init_f->identity().noise_static(), &rs, fixed_ephemeral(0x77));
        adapters::PinnedMsg1Payload p1;
        p1.cluster = cluster;
        p1.node = a_id;
        p1.timestamp_ns = static_cast<std::uint64_t>(kClockNs);
        p1.ed25519 = a_pub;
        if (!ini.write_msg1(adapters::encode_pinned_msg1_payload(p1), valid_msg1)) fuzz_fail("seed Msg1");
        adapters::noise::IkHandshake rsp(adapters::noise::Role::Responder, adapters::pinned_prologue(),
                                         resp_f->identity().noise_static(), nullptr, fixed_ephemeral(0x88));
        Bytes pl;
        if (!rsp.read_msg1(valid_msg1, pl)) fuzz_fail("seed Msg1 does not authenticate");
        if (!rsp.write_msg2(adapters::encode_pinned_msg2_payload({adapters::kPinnedVersion, b_id}), valid_msg2))
            fuzz_fail("seed Msg2");
    }
};

[[nodiscard]] inline Fixture& fx() {
    static Fixture f;
    return f;
}

// Feed `in` to a fresh responder engine and check the silent-refusal / honest-completion properties.
// `payload` is the decoded Msg1 payload when the caller knows it (hostile-payload mode), else nullptr.
inline void feed_responder(std::span<const std::byte> in, const adapters::PinnedMsg1Payload* payload) {
    Fixture& f = fx();
    auto e = f.resp_f->create(false, f.b_id, NodeId{0}, f.cluster, {}, {}, nullptr);
    std::vector<std::byte> out;
    const auto st = e->advance(in, out);
    if (st == HandshakeEngine::Step::Failed && !out.empty()) fuzz_fail("responder emitted bytes on a refusal");
    if (st == HandshakeEngine::Step::Done) {
        if (out.size() != adapters::kPinnedMsg2Len) fuzz_fail("responder completed without a 58-byte Msg2");
        if (payload) {
            const std::uint16_t v = payload->version;
            const bool honest = payload->ed25519 == f.a_pub && payload->node == f.a_id && payload->cluster == f.cluster &&
                                payload->timestamp_ns == static_cast<std::uint64_t>(kClockNs) && v <= 2u;
            if (!honest) fuzz_fail("responder completed a hostile payload");
        }
    }
}

// The noise-layer initiator (deterministic ephemeral) reading `msg2`, then the Msg2 payload decoder.
inline void feed_initiator_noise(std::span<const std::byte> msg2) {
    Fixture& f = fx();
    Key32 rs{};
    if (!adapters::ed25519_to_x25519(f.b_pub, rs)) fuzz_fail("responder key conversion");
    adapters::noise::IkHandshake ini(adapters::noise::Role::Initiator, adapters::pinned_prologue(),
                                     f.init_f->identity().noise_static(), &rs, fixed_ephemeral(0x77));
    Bytes m1;
    if (!ini.write_msg1(adapters::encode_pinned_msg1_payload({}), m1)) fuzz_fail("initiator Msg1");
    Bytes pl;
    if (ini.read_msg2(msg2, pl)) {
        const auto d = adapters::decode_pinned_msg2_payload(pl);
        [[maybe_unused]] static volatile std::uint64_t sink;  // the store keeps the decode alive
        sink = d ? d->node.value : 0;
    }
}

// The initiator ENGINE fed `msg2` after it has emitted its own Msg1: it must never complete (the Msg2 was not made for
// its random ephemeral key).
inline void feed_initiator_engine(std::span<const std::byte> msg2) {
    Fixture& f = fx();
    auto e = f.init_f->create(true, f.a_id, f.b_id, f.cluster, {}, {}, nullptr);
    std::vector<std::byte> out1, out2;
    if (e->advance({}, out1) != HandshakeEngine::Step::WantWrite) fuzz_fail("initiator did not emit Msg1");
    if (out1.size() != adapters::kPinnedMsg1Len) fuzz_fail("initiator Msg1 is not 154 bytes");
    if (e->advance(msg2, out2) == HandshakeEngine::Step::Done) fuzz_fail("initiator accepted a forged Msg2");
}

[[nodiscard]] inline std::vector<std::byte> padded(std::span<const std::byte> in, std::size_t len) {
    std::vector<std::byte> v(len, std::byte{0});
    if (!in.empty()) std::memcpy(v.data(), in.data(), in.size() < len ? in.size() : len);
    return v;
}

}  // namespace pinned_detail

// Input byte 0, low 2 bits, picks the extra mode (the whole input is ALWAYS also fed raw as a Msg1 and as a Msg2):
//   0 raw only              1 valid Msg1/Msg2 with byte patches from the rest of the input
//   2 hostile payload: a Msg1 the real initiator key authenticates, payload bytes = the input (bits 4-7 repair the
//     key / node / cluster / timestamp fields so the later checks are reached)
//   3 input padded or cut to exactly 154 / 58 bytes (forces the DH path on every input)
inline void run_pinned_noise(const std::byte* p, std::size_t n) {
    using namespace pinned_detail;
    Fixture& f = fx();
    const std::span<const std::byte> in(p, n);
    feed_responder(in, nullptr);
    if (n >= adapters::noise::kMsg2Overhead) feed_initiator_noise(in);
    if (n == adapters::kPinnedMsg2Len) feed_initiator_engine(in);
    if (n == 0) return;
    const std::uint8_t sel = static_cast<std::uint8_t>(p[0]);
    const auto rest = in.subspan(1);
    switch (sel & 3u) {
        case 1: {
            Bytes m1 = f.valid_msg1, m2 = f.valid_msg2;
            for (std::size_t i = 0; i + 2 < rest.size(); i += 3) {
                const std::size_t idx = (static_cast<std::size_t>(rest[i]) | (static_cast<std::size_t>(rest[i + 1]) << 8));
                Bytes& m = (sel & 4u) ? m2 : m1;
                m[idx % m.size()] ^= rest[i + 2];
            }
            feed_responder(m1, nullptr);
            feed_initiator_noise(m2);
            break;
        }
        case 2: {
            Bytes pl = padded(rest, adapters::kPinnedMsg1PayloadLen);
            if (sel & 0x10u) std::memcpy(pl.data() + 26, f.a_pub.data(), 32);
            const auto put64 = [&pl](std::size_t off, std::uint64_t v) {
                for (int i = 0; i < 8; ++i) pl[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
            };
            if (sel & 0x20u) put64(10, f.a_id.value);
            if (sel & 0x40u) put64(2, f.cluster.value);
            if (sel & 0x80u) put64(18, static_cast<std::uint64_t>(kClockNs));
            Key32 rs{};
            if (!adapters::ed25519_to_x25519(f.b_pub, rs)) fuzz_fail("responder key conversion");
            adapters::noise::IkHandshake ini(adapters::noise::Role::Initiator, adapters::pinned_prologue(),
                                             f.init_f->identity().noise_static(), &rs, fixed_ephemeral(0x66));
            Bytes m1;
            if (!ini.write_msg1(pl, m1)) fuzz_fail("hostile Msg1 could not be built");
            const auto decoded = adapters::decode_pinned_msg1_payload(pl);
            feed_responder(m1, decoded ? &*decoded : nullptr);
            break;
        }
        case 3: {
            feed_responder(padded(in, adapters::kPinnedMsg1Len), nullptr);
            const auto m2 = padded(in, adapters::kPinnedMsg2Len);
            feed_initiator_noise(m2);
            feed_initiator_engine(m2);
            break;
        }
        default: break;
    }
}
#else
inline void run_pinned_noise(const std::byte*, std::size_t) {}  // libsodium not built: skipped cleanly
#endif

// ---- 2. The anonymous inbound path of the pinned wire --------------------------------------------------------------
// Input byte 0 = recv() chunk size (0 = deliver everything in one recv); the rest is the connection's byte stream. The
// loop is TcpTransport::do_read's anonymous branch: reassemble with FrameStream, apply the shared admissibility rule to
// each whole frame, close (stop) on the first refusal or stream error.
inline void run_anonymous_wire(const std::byte* p, std::size_t n) {
    if (n == 0) return;
    const std::size_t chunk = static_cast<std::size_t>(p[0]) == 0 ? n : static_cast<std::size_t>(p[0]);
    net::FrameStream fs;
    bool opener_seen = false, kill = false;
    int accepted = 0;
    for (std::size_t off = 1; off < n && !kill; off += chunk) {
        const std::size_t len = (n - off) < chunk ? (n - off) : chunk;
        const bool ok = fs.feed(p + off, len, [&](MessageFrame&& f) {
            if (kill || !net::anonymous_opener_admissible(f, opener_seen)) {
                kill = true;
                return;
            }
            opener_seen = true;
            ++accepted;
            if (f.kind != FrameKind::Authenticate || f.from.value != 0 || f.to.value != 0 ||
                f.payload.size() > net::kAnonOpenerMax)
                fuzz_fail("anonymous gate accepted a frame that is not a plain opener");
        });
        if (!ok) break;  // protocol error: the transport drops the connection
    }
    if (accepted > 1) fuzz_fail("anonymous gate accepted more than one frame");
}

// ---- 3. Discovery decode ---------------------------------------------------------------------------------------------
// Header (9 bytes): flags, a, b, port(2), ipv4(4); then `rest`. instance label = rest[0:a%..], host label = next b bytes,
// service type = the remainder (flag 0x01) or a fixed one. flags: 0x02 accept the label's decoded id as CURRENT, 0x04 as
// PREVIOUS, 0x08 advertise an id taken from the input, 0x10 send a Lost after the Found.
inline void run_discovery_decode(const std::byte* p, std::size_t n) {
    using namespace net;
    const std::string_view all(reinterpret_cast<const char*>(p), n);
    // The label decoder is the strict inverse of the encoder.
    if (const auto d = decode_instance_label(all)) {
        if (d->size() > kMaxInstanceIdBytes) fuzz_fail("decoded label longer than the id cap");
        if (encode_instance_label(*d) != all) fuzz_fail("label decode is not canonical");
    }
    if (n > 0 && n <= kMaxInstanceIdBytes) {
        const std::span<const std::byte> raw(p, n);
        const auto d = decode_instance_label(encode_instance_label(raw));
        if (!d || !std::equal(d->begin(), d->end(), raw.begin(), raw.end())) fuzz_fail("label round trip");
    }
    if (n < 9) return;

    const std::uint8_t flags = static_cast<std::uint8_t>(p[0]);
    const std::size_t rest_n = n - 9;
    const char* rest = reinterpret_cast<const char*>(p + 9);
    const std::size_t a = static_cast<std::size_t>(p[1]) % (rest_n + 1);
    const std::size_t b = a + static_cast<std::size_t>(p[2]) % (rest_n - a + 1);
    const std::uint16_t port = static_cast<std::uint16_t>(static_cast<unsigned>(p[3]) | (static_cast<unsigned>(p[4]) << 8));
    std::uint32_t ipv4 = 0;
    for (int i = 0; i < 4; ++i) ipv4 |= static_cast<std::uint32_t>(p[5 + i]) << (8 * i);
    const std::string label(rest, a), host(rest + a, b - a);
    const std::string type = (flags & 0x01u) ? std::string(rest + b, rest_n - b) : std::string("_fz._tcp");
    if (type.empty()) return;

    std::set<std::string> accepted_now, accepted_prev;
    DiscoveryWindow w;
    w.host_label = "fuzz-host";
    w.advertise_id = (flags & 0x08u) ? DnsSdBytes(p + 9, p + 9 + (rest_n < 40 ? rest_n : 40)) : DnsSdBytes{std::byte{1}};
    w.accept_ids.push_back(DnsSdBytes{std::byte{9}, std::byte{9}});
    if (const auto id = decode_instance_label(label)) {
        if (flags & 0x02u) {
            w.accept_ids.push_back(*id);
            accepted_now.insert(std::string(reinterpret_cast<const char*>(id->data()), id->size()));
        }
        if (flags & 0x04u) {
            w.accept_previous_ids.push_back(*id);
            accepted_prev.insert(std::string(reinterpret_cast<const char*>(id->data()), id->size()));
        }
    }

    FakeDnsSdNetwork netw;
    FakeDnsSdPlatform browser_side(netw), other(netw);
    int found = 0;
    LanDiscovery d(browser_side, [&](const DiscoveredPeer& peer) {
        ++found;
        const std::string key(reinterpret_cast<const char*>(peer.instance_id.data()), peer.instance_id.size());
        const bool now = accepted_now.contains(key), prev = accepted_prev.contains(key);
        if (!now && !prev && key != std::string("\x09\x09", 2)) fuzz_fail("discovery reported an id that is not accepted");
        if (peer.host_label != host || peer.port != port || peer.ipv4 != ipv4) fuzz_fail("sighting fields altered");
    });
    DiscoveryScopeConfig cfg;
    cfg.service_type = type;
    cfg.port = 1;
    cfg.window = [w] { return w; };
    auto scope = d.open_scope(std::move(cfg));
    if (!scope) return;
    (void)d.start_advertising(*scope);  // may refuse (id too long / empty): fine
    (void)d.start_browsing(*scope);
    DnsSdAdvert adv{type, label, host, ipv4, port, {}};
    auto h = other.register_service(adv);
    (void)d.window_boundary(*scope);
    if (h && (flags & 0x10u)) other.deregister_service(*h);
    d.close_scope(*scope);
}

// ---- seeds ---------------------------------------------------------------------------------------------------------
inline void add_pinned_seeds(std::vector<std::vector<std::byte>>& out) {
    using Buf = std::vector<std::byte>;
    const auto B = [](std::initializer_list<unsigned> l) {
        Buf b;
        for (unsigned v : l) b.push_back(static_cast<std::byte>(v));
        return b;
    };
    const auto cat = [](Buf a, const Buf& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };

#ifdef QUARK_FUZZ_SODIUM
    {
        using namespace pinned_detail;
        Fixture& f = fx();
        out.push_back(f.valid_msg1);  // raw valid Msg1: the responder completes
        out.push_back(f.valid_msg2);  // raw valid Msg2 (noise-layer initiator reaches the payload decoder)
        Buf flipped = f.valid_msg1;
        flipped[flipped.size() - 1] ^= std::byte{1};
        out.push_back(flipped);
        out.push_back(B({0x01, 0x05, 0x00, 0x01}));              // patch mode, a one-byte change to Msg1
        out.push_back(B({0x05, 0x10, 0x00, 0x80}));              // patch mode on Msg2
        Buf honest(adapters::kPinnedMsg1PayloadLen, std::byte{0});
        out.push_back(cat(B({0xF2}), honest));                   // hostile payload, every field repaired
        out.push_back(cat(B({0x02}), honest));                   // hostile payload, nothing repaired
        out.push_back(cat(B({0x03}), f.valid_msg1));             // padded/cut mode
    }
#endif

    // Pinned wire, anonymous inbound: a frame is [u32 len][body]; chunk byte first.
    const auto frame = [](FrameKind k, std::uint64_t from, std::uint64_t to, std::size_t payload) {
        MessageFrame m{};
        m.kind = k;
        m.from = NodeId{from};
        m.to = NodeId{to};
        m.payload.assign(payload, std::byte{0x5A});
        return net::encode_frame(m);
    };
    const Buf opener = frame(FrameKind::Authenticate, 0, 0, 154);
    out.push_back(cat(B({0}), opener));                                        // the one legal opener, one recv
    out.push_back(cat(B({7}), opener));                                        // the same, 7-byte recv chunks
    out.push_back(cat(cat(B({0}), opener), opener));                           // two openers coalesced: must close
    out.push_back(cat(B({0}), frame(FrameKind::Authenticate, 0, 0, 1024)));    // exactly at the cap
    out.push_back(cat(B({0}), frame(FrameKind::Authenticate, 0, 0, 1025)));    // one over the cap
    out.push_back(cat(B({0}), frame(FrameKind::Data, 0, 0, 154)));             // wrong kind
    out.push_back(cat(B({0}), frame(FrameKind::Authenticate, 1, 0, 154)));     // from != 0
    out.push_back(cat(B({0}), frame(FrameKind::Authenticate, 0, 2, 154)));     // to != 0

    // Discovery: header + label + host. A label that decodes and is accepted (current / previous), and bad ones.
    const std::vector<std::byte> id16(16, std::byte{0x42});
    const std::string good = net::encode_instance_label(id16);
    const auto disc = [&](unsigned flags, const std::string& lbl, const std::string& host) {
        Buf b = B({flags, static_cast<unsigned>(lbl.size()), static_cast<unsigned>(host.size()), 0x39, 0x30, 192, 168, 1, 7});
        for (char c : lbl) b.push_back(static_cast<std::byte>(c));
        for (char c : host) b.push_back(static_cast<std::byte>(c));
        return b;
    };
    out.push_back(disc(0x02, good, "host-1"));
    out.push_back(disc(0x04, good, "host-2"));
    out.push_back(disc(0x12, good, "host-3"));        // found then lost
    out.push_back(disc(0x0A, good, "host-4"));        // also advertises an id taken from the input
    out.push_back(disc(0x02, "ABCDEFGH", "upper"));   // uppercase: strict decoder refuses
    out.push_back(disc(0x02, good + "a", "padded"));  // non-canonical trailing bits
    out.push_back(disc(0x02, "aaa", "alias"));        // REGRESSION (#50): a spare zero group is a second spelling
    out.push_back(disc(0x02, "a", "empty"));          // REGRESSION (#50): five zero bits decoded as the empty id
    out.push_back(Buf(good.size(), std::byte{0}));
    {
        Buf g;
        for (char c : good) g.push_back(static_cast<std::byte>(c));
        out.push_back(g);  // a bare label: exercises the decode/round-trip properties
    }
}

}  // namespace quark::fuzz
