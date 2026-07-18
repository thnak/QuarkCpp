// Reflection-free proof TU (ADR-016 §"Reflection-free — MEASURED"). This exercises every codec
// path (tagless + tagged encode, tagged decode, fingerprint, negotiation) so that, compiled with
// -fno-rtti, `nm` over the object shows 0 typeid/dynamic_cast (RTTI) symbols from the codec. The
// verification harness builds this TU with -fno-rtti and greps its symbols; the runtime check
// here is a defensive round-trip so the TU is also a live test.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/serialize.hpp"

using namespace quark;

namespace {
struct Pod {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};
struct Line {
    std::uint64_t sku;
    std::uint32_t qty;
};
struct Order {
    std::uint64_t id;
    std::string customer;
    std::vector<Line> lines;
};
QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))
QUARK_SERIALIZE(Line, (1, sku), (2, qty))
QUARK_SERIALIZE(Order, (1, id), (2, customer), (3, lines))

}  // namespace

int main() {
    std::uint64_t fails = 0;

    // Tagless path.
    Pod p{1, 2, 3, 4.5};
    std::byte tl[64];
    encode_tagless(p, tl);
    Pod p2{};
    decode_tagless(tl, p2);
    if (p2.id != 1 || p2.qty != 2 || p2.flags != 3 || p2.price != 4.5) ++fails;

    // Tagged path (nested vector).
    Order o{7, "acme", {{10, 1}, {20, 2}}};
    std::byte tg[256];
    auto n = encode_tagged(o, tg, sizeof tg);
    Order o2{};
    auto rc = decode_tagged(tg, *n, o2);
    if (!n || !rc || o2.customer != "acme" || o2.lines.size() != 2 || o2.lines[1].qty != 2) ++fails;

    // Negotiation + fingerprint.
    const WireMode m = negotiate<Pod>(local_schema<Pod>());
    if (m != WireMode::Tagless) ++fails;

    // Validation seam: a non-described type is a Validation error (008).
    struct Undescribed {
        int x;
    };
    auto v = validate_serializable<Undescribed>();
    if (v || v.error().code != errc::validation) ++fails;
    auto vd = validate_serializable<Pod>();
    if (!vd) ++fails;

    const bool pass = (fails == 0);
    std::printf("serialize_reflection_free_test: %s  (pod_fp=%016llx)\n", pass ? "OK" : "FAIL",
                static_cast<unsigned long long>(fingerprint_v<Pod>));
    return pass ? 0 : 1;
}
