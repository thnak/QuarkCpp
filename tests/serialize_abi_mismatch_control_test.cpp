// CONTROL (ADR-016 control 5) — proves the platform/ABI/endianness tag in the negotiation is
// LOAD-BEARING. The tagless fast path bulk-copies RAW native layout, so it is only safe between
// peers of the same endianness/ABI. Here a byte-swapped (big-endian) peer is FORCED onto tagless
// despite a mismatched ABI tag; the reader interprets the swapped bytes natively and reads a
// demonstrably WRONG value. This test PASSES by DETECTING the corruption.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

struct Pod {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};

QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))

}  // namespace

int main() {
    // A foreign peer with a DIFFERENT ABI/endianness tag. negotiate() compares this against
    // pal::platform_abi_tag; a mismatch forces the canonical tagged (LE, layout-independent)
    // fallback. We bypass that gate to show why it exists.
    constexpr std::uint32_t foreign_be_abi_tag = 0x78'86'36'42;  // 'x','86','6','B' — big-endian
    const PeerSchema foreign{fingerprint_v<Pod>, foreign_be_abi_tag};

    // Sanity: with a matching fingerprint but a MISMATCHED ABI tag, negotiate() must NOT choose
    // tagless (it must fall back to tagged). If it chose tagless, the gate would be broken.
    const WireMode mode = negotiate<Pod>(foreign);
    if (mode != WireMode::Tagged) {
        std::fprintf(stderr, "GATE BROKEN: negotiate() chose tagless across an ABI mismatch\n");
        return 1;
    }

    // Now FORCE tagless anyway (the thing the gate forbids). Simulate the big-endian peer by
    // byte-swapping the id field's 8 bytes on the wire, as its native store would produce.
    Pod w{0x0102030405060708ULL, 0xaabbccddU, 0x11223344U, 6.0};
    std::byte buf[64];
    encode_tagless(w, buf);

    // The foreign peer's native (big-endian) representation of id = the LE bytes reversed.
    std::uint64_t id_le;
    std::memcpy(&id_le, buf, sizeof id_le);
    const std::uint64_t id_be = std::byteswap(id_le);
    std::memcpy(buf, &id_be, sizeof id_be);  // buf now holds the foreign peer's id bytes

    // Our reader decodes natively (little-endian) — reading the byte-swapped id as-is.
    Pod r{};
    decode_tagless(buf, r);

    const bool id_corrupt = (r.id != w.id);
    const bool id_is_swapped = (r.id == std::byteswap(w.id));

    std::printf("serialize_abi_mismatch_control_test: %s\n",
                id_corrupt ? "OK (corruption detected)" : "FAIL (no defect)");
    std::printf("  local ABI tag   = %08x\n", pal::platform_abi_tag);
    std::printf("  foreign ABI tag = %08x  (mismatch -> negotiate() = Tagged, gate holds)\n",
                foreign_be_abi_tag);
    std::printf("  writer id = %016llx   reader id = %016llx  (byte-swapped: %s)\n",
                static_cast<unsigned long long>(w.id), static_cast<unsigned long long>(r.id),
                id_is_swapped ? "yes" : "no");

    // PASS iff the ABI mismatch, when the gate is bypassed, corrupted the value.
    return id_corrupt ? 0 : 1;
}
