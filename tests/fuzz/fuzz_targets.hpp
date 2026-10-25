// Shared P5 fuzz targets. Every untrusted decode path is exercised with the SAME input:
//   wire tagged + bounds-checked tagless, the F17 mixed-schema pair + negotiation gate,
//   SWIM control, the schema advert, durable snapshot/event records + migration, frame-body
//   decoding, and TCP frame reassembly. `fuzz_decoders_test` drives this deterministically on
//   every platform; `fuzz_decoders` drives it under libFuzzer + ASan/UBSan in CI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"
#include "quark/core/wire.hpp"
#include "quark/net/wire_codec.hpp"

namespace quark::fuzz {

struct Msg {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int64_t c = 0;
    std::string s;
};
QUARK_SERIALIZE(Msg, (1, a), (2, b), (3, c), (4, s))

// A reused-tag layout change: same tags, int32/int64 swapped. The F17 fingerprint must differ.
struct TransferV1 {
    std::int32_t from = 0;
    std::int32_t to = 0;
    std::int64_t amount = 0;
    std::string memo;
};
QUARK_SERIALIZE(TransferV1, (1, from), (2, to), (3, amount), (4, memo))

struct TransferV2 {
    std::int64_t amount = 0;
    std::int32_t from = 0;
    std::int32_t to = 0;
    std::string memo;
};
QUARK_SERIALIZE(TransferV2, (1, amount), (2, from), (3, to), (4, memo))

struct EventV1 {
    std::uint64_t id = 0;
    std::string name;
};
QUARK_SERIALIZE(EventV1, (1, id), (2, name))

struct EventV2 {
    std::uint64_t id = 0;
    std::string name;
    std::uint32_t priority = 0;
    std::int64_t retries = 0;
};
QUARK_SERIALIZE(EventV2, (1, id), (2, name), (3, priority), (4, retries))

struct DurableSnap {
    std::uint64_t account = 0;
    std::uint64_t balance = 0;
    std::uint32_t version = 0;
};
QUARK_SERIALIZE(DurableSnap, (1, account), (2, balance), (3, version))

inline void run_one(const std::byte* p, std::size_t size) {

    // 016 wire: canonical tagged and the bounds-checked tagless reader.
    Msg m;
    (void)decode_tagged(p, size, m);
    Msg t;
    (void)decode_tagless_checked(p, size, t);

    // F17: the same bytes as two reused-tag schemas, plus the negotiation gate (must never choose
    // tagless for the mismatch).
    TransferV1 v1;
    TransferV2 v2;
    (void)decode_tagless_checked(p, size, v1);
    (void)decode_tagless_checked(p, size, v2);
    (void)decode_tagged(p, size, v1);
    (void)decode_tagged(p, size, v2);
    (void)negotiate<TransferV1>(PeerSchema{fingerprint_v<TransferV2>, pal::platform_abi_tag});
    (void)negotiate<TransferV2>(PeerSchema{fingerprint_v<TransferV1>, pal::platform_abi_tag});

    // 021 SWIM control and the F17 schema advertisement.
    ControlMsg cm;
    (void)detail::decode_control(p, size, cm);
    SchemaAdvertisement adv;
    (void)decode_schema_advert(p, size, adv);

    // 012/016 durable payloads + the migration chain.
    (void)decode_record<DurableSnap>(p, size);
    (void)decode_record<EventV2>(p, size);
    (void)read_migrated<EventV2, EventV1>(p, size);

    // 010 TCP frame body + stream reassembly (partial/coalesced frames, oversized prefixes).
    (void)net::decode_frame_body(std::span<const std::byte>(p, size));
    net::FrameStream fs(1024);
    (void)fs.feed(p, size, [](MessageFrame&&) {});
}

inline void run_one(const std::uint8_t* data, std::size_t size) {
    run_one(reinterpret_cast<const std::byte*>(data), size);
}

inline std::vector<std::vector<std::byte>> seed_corpus() {
    std::vector<std::vector<std::byte>> out;
    const auto add_tagged = [&out](const auto& v) {
        std::vector<std::byte> b(2048);
        if (const auto n = encode_tagged(v, b.data(), b.size()); n) {
            b.resize(*n);
            out.push_back(std::move(b));
        }
    };
    const auto add_tagless = [&out](const auto& v) {
        std::vector<std::byte> b(tagless_size(v));
        (void)encode_tagless(v, b.data());
        out.push_back(std::move(b));
    };
    const auto add_durable = [&out](const auto& v) {
        if (auto b = encode_durable(v)) out.push_back(std::move(*b));
    };

    const Msg m{1, 2, 3, "seed"};
    add_tagged(m);
    add_tagless(m);

    const TransferV1 v1{1, 2, 1000, "p"};
    const TransferV2 v2{1000, 1, 2, "p"};
    add_tagged(v1);
    add_tagged(v2);
    add_tagless(v1);
    add_tagless(v2);

    const EventV1 e1{42, "spawn"};
    const EventV2 e2{99, "migrate", 5, -3};
    add_tagged(e1);
    add_tagged(e2);
    add_durable(e1);
    add_durable(e2);

    const DurableSnap snap{0xA11CE, 4200, 3};
    add_durable(snap);

    ControlMsg cm{};
    cm.kind = ControlKind::Gossip;
    cm.cluster = ClusterId{1};
    cm.from = NodeId{1};
    cm.seq = 1;
    out.push_back(detail::encode_control(cm));

    SchemaAdvertisement adv{pal::platform_abi_tag, {fingerprint_v<Msg>, fingerprint_v<TransferV1>}};
    out.push_back(encode_schema_advert(adv));

    MessageFrame f{};
    f.from = NodeId{1};
    f.to = NodeId{2};
    f.target = ActorId{TypeKey{0xAA}, 1};
    f.msg_type = TypeKey{fingerprint_v<Msg>};
    f.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    out.push_back(net::encode_frame(f));

    // Truncated variants exercise the partial-frame/bounds paths with a plausible prefix.
    const std::size_t n = out.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& b = out[i];
        const auto half = static_cast<std::ptrdiff_t>(b.size() / 2);
        out.push_back(std::vector<std::byte>(b.begin(), b.begin() + half));
    }
    return out;
}

}  // namespace quark::fuzz

// Durable schema versions + an additive V1->V2 upcaster (the migration decoder path).
QUARK_SCHEMA_VERSION(quark::fuzz::EventV1, 1);
QUARK_SCHEMA_VERSION(quark::fuzz::EventV2, 2);
QUARK_MIGRATE(quark::fuzz::EventV1, quark::fuzz::EventV2,
              [](const quark::fuzz::EventV1& o) {
                  return quark::fuzz::EventV2{o.id, o.name, 0, 0};
              });
