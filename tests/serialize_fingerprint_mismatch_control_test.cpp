// CONTROL (ADR-016 control 4) — proves the connect-time fingerprint gate is LOAD-BEARING, not
// decorative. Two peers with DIFFERENT schemas (distinct fingerprints) are FORCED onto the
// unchecked tagless fast path. The negotiation would have rejected this pairing; bypassing it
// must demonstrably CORRUPT. This test PASSES by DETECTING the corruption.
//
//   * Default build         : the reader reads WRONG in-bounds values (offsets shifted by the
//                             mismatched schema). PASS iff a value is corrupted (exit 0).
//   * -DQUARK_CONTROL_OOB   : the reader over-reads a tight, exact-sized heap buffer → ASan
//                             heap-buffer-overflow (the ADR's reproduced effect). Under ASan the
//                             process aborts (non-zero) — registered WILL_FAIL, so aborting =
//                             the control firing.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

// Writer's schema — the ADR's named 24-byte POD.
struct Pod {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};

// Reader's MISMATCHED schema: inserts an extra u64 after id (tag 5), shifting qty/flags/price
// forward by 8 bytes and making the tagless object 8 bytes LONGER than the writer produced.
struct PodShift {
    std::uint64_t id;
    std::uint64_t extra_before;  // added field (tag 5) — shifts everything after it
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};

QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))
QUARK_SERIALIZE(PodShift, (1, id), (5, extra_before), (2, qty), (3, flags), (4, price))

}  // namespace

int main() {
    // The gate's premise: the two schemas hash DISTINCTLY, so negotiate() would refuse tagless
    // and fall back to tagged. If the fingerprints collided the control would be vacuous.
    const std::uint64_t fp_w = fingerprint_v<Pod>;
    const std::uint64_t fp_r = fingerprint_v<PodShift>;
    if (fp_w == fp_r) {
        std::fprintf(stderr, "VACUOUS: fingerprints collide (%016llx) — control cannot fire\n",
                     static_cast<unsigned long long>(fp_w));
        return 1;
    }

    Pod w{0x0102030405060708ULL, 0xaabbccddU, 0x11223344U, 2.5};
    const std::size_t wrote = tagless_size(w);  // 24

#ifdef QUARK_CONTROL_OOB
    // Exact-sized heap buffer: the reader's longer schema reads past the end → ASan fires in
    // TaglessReader::field<double> (price at offset 32 over a 24-byte allocation).
    std::vector<std::byte> tight(wrote);
    encode_tagless(w, tight.data());
    PodShift r{};
    decode_tagless(tight.data(), r);  // <-- heap-buffer-overflow under ASan
    std::fprintf(stderr, "OOB read did not trap (no ASan?) — defect present, id=%llu price=%g\n",
                 static_cast<unsigned long long>(r.id), r.price);
    return 1;  // WILL_FAIL: non-zero == the control fired
#else
    // Value-corruption mode: give the reader a buffer big enough to stay in-bounds, so the
    // corruption shows as WRONG VALUES rather than a trap. The reader's shifted layout reads
    // qty/flags out of the writer's price bytes.
    std::byte buf[64];
    std::memset(buf, 0, sizeof buf);
    encode_tagless(w, buf);
    PodShift r{};
    decode_tagless(buf, r);

    const bool id_ok = (r.id == w.id);                 // id offset unchanged -> still correct
    const bool qty_corrupt = (r.qty != w.qty);         // qty read from shifted offset -> wrong
    const bool flags_corrupt = (r.flags != w.flags);   // likewise

    std::printf("serialize_fingerprint_mismatch_control_test: %s\n",
                (qty_corrupt || flags_corrupt) ? "OK (corruption detected)" : "FAIL (no defect)");
    std::printf("  fp(writer Pod)     = %016llx\n", static_cast<unsigned long long>(fp_w));
    std::printf("  fp(reader PodShift)= %016llx  (distinct -> negotiation would reject)\n",
                static_cast<unsigned long long>(fp_r));
    std::printf("  writer qty = %08x   reader qty = %08x   (wrong: %s)\n", w.qty, r.qty,
                qty_corrupt ? "yes" : "no");
    std::printf("  id preserved by prefix overlap = %s (bytes written=%zu)\n",
                id_ok ? "yes" : "no", wrote);

    // PASS iff forcing tagless past the fingerprint mismatch corrupted a value.
    return (qty_corrupt || flags_corrupt) ? 0 : 1;
#endif
}
