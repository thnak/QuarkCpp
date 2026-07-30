// Tests 008-Metadata-and-Startup §Type identity — cross-toolchain `type_key` CONFORMANCE.
// Companion to `metadata_typekey_test.cpp`, which asserts only self-consistency (same key on every
// derivation, within one compile) and therefore cannot see drift: it would stay green even if a
// compiler upgrade or a name-slicer edit silently changed a durable key. This test pins GOLDEN
// constants so drift turns the build red instead of silently invalidating already-written durable
// records (OpenQuestions.md item 1).
//
// Tiered because `type_key_of<T>()` (metadata.hpp) derives three different ways, and only one is
// toolchain-portable by construction:
//   Tier 1 — Described types: `type_key_of<T>() == fingerprint_v<T>` (016), folds only
//            `{(tag, wire_type)}` — no name enters it, so it is portable BY CONSTRUCTION. Golden
//            on every toolchain.
//   Tier 2 — non-Described, name-derived keys (`fnv1a(canonical_type_name<T>())`): the GCC/Clang
//            `__PRETTY_FUNCTION__` slice agrees, but MSVC's `__FUNCSIG__` slice keeps the
//            `struct`/`class` elaborator (`struct ns::Point` vs `ns::Point`) — a different string
//            folds to a different key. Golden on GCC/Clang; MSVC is excluded from the golden
//            comparison (recorded, not asserted-equal) below.
//   Tier 3 — templates over compiler-divergent builtin spellings (e.g. `size_t` = `long unsigned
//            int` on GCC vs `unsigned long` on Clang): divergent TODAY on every toolchain pair.
//            Determinism-only (same key on repeated derivation), never asserted equal across
//            toolchains — a cross-toolchain golden here would hard-fail on principle.
//
// This binary prints every tier's key so a human/CI can diff between toolchain runs; the checks
// below pin the Tier-1 golden (all toolchains) and the Tier-2 golden under `#if !defined(_MSC_VER)`
// (GCC/Clang only — MSVC's divergence is the expected finding, not a bug to golden away).
#include <cstdio>
#include <cstdint>

#include "quark/core/describe.hpp"
#include "quark/core/metadata.hpp"

using namespace quark;

namespace {

// Tier 1 fixture — a Described type. Its key must be name-free: renaming this struct must NOT
// change fingerprint_v (checked structurally below, not by literally renaming — the derivation
// never reads a name for a Described type, see describe.hpp's FingerprintFolder).
struct ConformancePoint {
    int x = 0;
    int y = 0;
};
QUARK_SERIALIZE(ConformancePoint, (1, x), (2, y))

// Reordered-field sibling — 016's field-order invariant (describe.hpp:236 / OpenQuestions item 2)
// says swapping which FIELD carries which TAG must move the fingerprint (it changes the folded
// (tag, wire_type) sequence), even though both types serialize the "same" two ints.
struct ConformancePointFieldsSwapped {
    int x = 0;
    int y = 0;
};
QUARK_SERIALIZE(ConformancePointFieldsSwapped, (2, x), (1, y))

// Tier 2 fixture — plain (non-Described) struct, so its key is name-derived
// (`fnv1a(canonical_type_name<T>())`). This is exactly where MSVC's elaborator diverges.
struct ConformanceTag {
    int unused = 0;
};

// Tier 3 fixture — a template instantiated over a builtin whose SPELLING differs across compilers
// (size_t: "long unsigned int" GCC vs "unsigned long" Clang vs "unsigned __int64" MSVC).
template <class T>
struct ConformanceWrap {
    T value{};
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

// --- Golden constants ---------------------------------------------------------------------
// Tier 1 is portable BY CONSTRUCTION (no name folded in), so one golden covers every toolchain.
// Measured directly on this box: Clang 22.1.5 (Windows, MSVC-ABI-compatible frontend) AND real
// MSVC 19.51.36252 (cl.exe) independently compute the SAME value — a genuine cross-toolchain
// measurement, not carried forward from the (unverified) numbers OpenQuestions.md previously
// quoted. GCC is not independently confirmed on this box (no g++ installed here); Tier 1's
// portability is asserted unconditionally because it holds by construction (FingerprintFolder
// never reads a name — see describe.hpp), not merely by two toolchains agreeing.
inline constexpr std::uint64_t kTier1Golden = 0x60eb68b9763e6f4cULL;
inline constexpr std::uint64_t kTier1SwappedGolden = 0x54f52ae0703db766ULL;

// Tier 2 golden, measured on Clang (this box) — the branch metadata.hpp's canonical_type_name()
// takes whenever __clang__ is defined, INCLUDING Windows Clang, which also defines _MSC_VER for
// MSVC-ABI compatibility. `#if !defined(_MSC_VER)` (as OpenQuestions.md originally proposed) is
// therefore the WRONG guard here — it would wrongly exclude a real Clang build on Windows. The
// guard below mirrors metadata.hpp's actual dispatch order (__clang__ / __GNUC__ checked BEFORE
// _MSC_VER). Not independently confirmed against real GCC on this box.
inline constexpr std::uint64_t kTier2GoldenClangGcc = 0xba4a66eb2b3e6ff3ULL;
// Real MSVC (cl.exe, __clang__ NOT defined) golden — measured directly, confirms the predicted
// elaborator divergence (`struct ns::Point` vs `ns::Point`) is real, not merely plausible.
inline constexpr std::uint64_t kTier2GoldenRealMsvc = 0x2d4c989423ddf201ULL;

int main() {
    bool ok = true;

    const std::uint64_t tier1_key = fingerprint_v<ConformancePoint>;
    const std::uint64_t tier1_swapped_key = fingerprint_v<ConformancePointFieldsSwapped>;
    const std::uint64_t tier2_key = type_key_of<ConformanceTag>().value;
    const std::uint64_t tier3_key = type_key_of<ConformanceWrap<std::size_t>>().value;

    // --- Tier 1: portable by construction — golden on every toolchain. ----------------------
    check(type_key_of<ConformancePoint>().value == fingerprint_v<ConformancePoint>,
          "Tier 1: type_key_of == fingerprint_v for a Described type", ok);
    check(tier1_key == kTier1Golden,
          "Tier 1 golden: Described fingerprint drifted — a durable wire/record identity change",
          ok);
    check(tier1_swapped_key == kTier1SwappedGolden,
          "Tier 1 golden (fields-swapped sibling) drifted", ok);
    check(tier1_key != tier1_swapped_key,
          "Tier 1 negative control: swapping which field carries which tag MUST move the "
          "fingerprint (016 field-order invariant) — proves the golden above is non-vacuous", ok);

    // --- Tier 2: golden on the __clang__/__GNUC__ branch; real MSVC diverges (confirmed). ----
    // This mirrors metadata.hpp's OWN dispatch priority, not a naive _MSC_VER check.
#if defined(__clang__) || defined(__GNUC__)
    check(tier2_key == kTier2GoldenClangGcc,
          "Tier 2 golden (Clang/GCC name-derived key) drifted", ok);
#elif defined(_MSC_VER)
    check(tier2_key == kTier2GoldenRealMsvc,
          "Tier 2 golden (real MSVC name-derived key) drifted", ok);
    check(tier2_key != kTier2GoldenClangGcc,
          "Tier 2 CONFIRMED divergence: real MSVC's __FUNCSIG__ elaborator (`struct ns::T` vs "
          "`ns::T`) yields a different name-derived key than Clang/GCC for the identical type — "
          "if this ever matches, MSVC's canonical_type_name() slicing changed and Tier 2 may be "
          "closeable",
          ok);
#endif

    // --- Tier 3: determinism only — never asserted equal across toolchains. -----------------
    check(tier3_key == type_key_of<ConformanceWrap<std::size_t>>().value,
          "Tier 3: self-consistent within one toolchain (cross-toolchain equality NOT asserted)",
          ok);

    // Order matters and must MIRROR metadata.hpp's canonical_type_name() dispatch exactly: on
    // Windows, clang-cl-style Clang defines BOTH __clang__ and _MSC_VER (MSVC-ABI compatibility),
    // so checking _MSC_VER first would mislabel a real Clang run as MSVC.
#if defined(__clang__)
    const char* compiler = "Clang";
#elif defined(__GNUC__)
    const char* compiler = "GCC";
#elif defined(_MSC_VER)
    const char* compiler = "MSVC (cl.exe)";
#else
    const char* compiler = "unknown";
#endif

    std::printf("metadata_typekey_conformance_test [%s]:\n", compiler);
    std::printf("  Tier 1 (Described fingerprint, ConformancePoint)         = 0x%016llx\n",
                 static_cast<unsigned long long>(tier1_key));
    std::printf("  Tier 1 (fields-swapped sibling, negative control)       = 0x%016llx\n",
                 static_cast<unsigned long long>(tier1_swapped_key));
    std::printf("  Tier 2 (name-derived, ConformanceTag)                    = 0x%016llx\n",
                 static_cast<unsigned long long>(tier2_key));
    std::printf("  Tier 3 (builtin-spelling, Wrap<size_t>)                  = 0x%016llx\n",
                 static_cast<unsigned long long>(tier3_key));

    std::printf("metadata_typekey_conformance_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
