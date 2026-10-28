// Tests 008-Metadata-and-Startup §Type identity — cross-toolchain `type_key` CONFORMANCE.
// Companion to `metadata_typekey_test.cpp`, which asserts only self-consistency (same key on every
// derivation, within one compile) and therefore cannot see drift: it would stay green even if a
// compiler upgrade or a name-slicer edit silently changed a durable key. This test pins GOLDEN
// constants so drift turns the build red instead of silently invalidating already-written durable
// records (OpenQuestions.md item 1).
//
// Tiered because `type_key_of<T>()` (metadata.hpp) derives three different ways, and only one is
// toolchain-portable by construction:
//   Tier 1 — Described types: `type_key_of<T>() == fingerprint_v<T>` (016). F17 folds the member
//            category/size/signedness plus the nested schema fingerprint; measured GCC 14, Clang 20
//            and MSVC 14.44 all agree on the golden below.
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
#include <string>
#include <string_view>

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

// ADR-061 fixtures live OUTSIDE the anonymous namespace so the named-namespace case is distinct from
// the anonymous `ConformanceTag` above (GCC `{anonymous}` / Clang `(anonymous namespace)` / MSVC
// `anonymous namespace'` spell the latter three ways).
namespace conformance_named {
struct NamedTag {
    int unused = 0;
};
template <class A, class B>
struct NamedPair {
    A a{};
    B b{};
};
struct Pinned {  // quark_type_name: hashed verbatim, identical on every toolchain, rename-stable
    static constexpr std::string_view quark_type_name = "quark.conformance.Pinned";
    int unused = 0;
};
struct PinnedRenamed {  // a DIFFERENT C++ name with the SAME pinned identity => same key
    static constexpr std::string_view quark_type_name = "quark.conformance.Pinned";
    int unused = 0;
};
}  // namespace conformance_named

// ADR-062 Tier 4 fixtures — ACTORS. Identity = the explicit quark_type_name + the Described protocol
// messages; NO compiler spelling enters the key. The anonymous-namespace actor (GCC/Clang/MSVC spell that
// three ways) and the actor whose protocol names `std::size_t` (a builtin spelled three ways) are the two
// cases that used to diverge per toolchain.
namespace {
struct AnonActor : Actor<AnonActor, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.conformance.AnonActor";
    using protocol = Protocol<ConformancePoint, std::size_t>;
    void handle(const ConformancePoint&) {}
    void handle(const std::size_t&) {}
};
}  // namespace
namespace conformance_named {
struct NamedActor : Actor<NamedActor, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.conformance.NamedActor";
    using protocol = Protocol<ConformancePoint>;
    void handle(const ConformancePoint&) {}
};
struct NoProtocolActor : Actor<NoProtocolActor, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.conformance.NoProtocolActor";
};
}  // namespace conformance_named

namespace {
// Normalise a literal spelling to a std::string (compiler-independent: exercises the three spellings
// directly, so this runs the same on every toolchain, not only on the one that prints that spelling).
std::string norm(std::string_view s) {
    std::string out;
    detail::normalize_type_name(s, [&out](char c) { out.push_back(c); });
    return out;
}
}  // namespace

// --- Golden constants ---------------------------------------------------------------------
// F17 changed the layout-aware fold; the old 0x60eb/0x54f5 values predate it. Re-measured on the
// pinned toolchains: GCC 14, Clang 20 and real MSVC 14.44 all compute the same Tier-1 values.
inline constexpr std::uint64_t kTier1Golden = 0x27756b6f126e43f2ULL;
inline constexpr std::uint64_t kTier1SwappedGolden = 0xec12c83ceaf59f3cULL;

// ADR-061 Tier 2 goldens: ONE value for ALL toolchains (the point of the ADR). `kTier2Golden` is the
// anonymous-namespace fixture and is byte-for-byte the pre-ADR-061 CLANG value (0xba4a66eb2b3e6ff3):
// normalisation targets Clang/GCC's spelling, so a Clang key does not move (E2). GCC's and MSVC's
// pre-ADR-061 values (0xc1a54de107d9d89f / 0x2d4c989423ddf201) are retired.
inline constexpr std::uint64_t kTier2Golden = 0xba4a66eb2b3e6ff3ULL;
inline constexpr std::uint64_t kTier2NamedGolden = 0x4202adf0189cbfa1ULL;
inline constexpr std::uint64_t kTier2PairGolden = 0x28f38acdf8c467cbULL;
inline constexpr std::uint64_t kTier2PinnedGolden = 0x1a4e0103acebb260ULL;
// ADR-062 Tier 4 goldens (actors): one value per fixture on EVERY toolchain.
inline constexpr std::uint64_t kTier4AnonActorGolden = 0xb3d61babaac77327ULL;
inline constexpr std::uint64_t kTier4NamedActorGolden = 0x32464213a5ce9f09ULL;
inline constexpr std::uint64_t kTier4NoProtocolGolden = 0x92462dbdb69011eaULL;

int main() {
    bool ok = true;

    const std::uint64_t tier1_key = fingerprint_v<ConformancePoint>;
    const std::uint64_t tier1_swapped_key = fingerprint_v<ConformancePointFieldsSwapped>;
    const std::uint64_t tier2_key = type_key_of<ConformanceTag>().value;
    const std::uint64_t named_key = type_key_of<conformance_named::NamedTag>().value;
    const std::uint64_t pair_key = type_key_of<conformance_named::NamedPair<int, double>>().value;
    const std::uint64_t pinned_key = type_key_of<conformance_named::Pinned>().value;
    const std::uint64_t tier3_key = type_key_of<ConformanceWrap<std::size_t>>().value;


    // --- Tier 1: F17 layout-aware golden (GCC/Clang/MSVC agree). ------------------------------
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

    // --- Tier 2 (ADR-061): ONE golden on every toolchain. -----------------------------------------
    check(tier2_key == kTier2Golden, "Tier 2 golden (anonymous-namespace fixture) drifted or differs on this toolchain", ok);
    check(named_key == kTier2NamedGolden, "Tier 2 golden (named-namespace fixture) drifted or differs on this toolchain", ok);
    check(pair_key == kTier2PairGolden, "Tier 2 golden (two-argument template) drifted or differs on this toolchain", ok);
    check(pinned_key == kTier2PinnedGolden, "Tier 2 golden (quark_type_name) drifted or differs on this toolchain", ok);
    check(pinned_key == type_key_of<conformance_named::PinnedRenamed>().value,
          "quark_type_name: same pinned name => same key regardless of the C++ name (rename-stable)", ok);
    check(pinned_key != named_key, "quark_type_name: negative control, pinned key differs from a name-derived one", ok);

    // --- Tier 4 (ADR-062): actor keys are the same on every toolchain, by construction. -----------
    const std::uint64_t anon_actor = type_key_of<AnonActor>().value;
    const std::uint64_t named_actor = type_key_of<conformance_named::NamedActor>().value;
    const std::uint64_t noproto_actor = type_key_of<conformance_named::NoProtocolActor>().value;
    check(anon_actor == kTier4AnonActorGolden, "Tier 4 golden (anonymous-namespace actor, size_t in protocol) differs on this toolchain", ok);
    check(named_actor == kTier4NamedActorGolden, "Tier 4 golden (named actor) differs on this toolchain", ok);
    check(noproto_actor == kTier4NoProtocolGolden, "Tier 4 golden (actor without a protocol) differs on this toolchain", ok);
    check(noproto_actor == detail::fnv1a(conformance_named::NoProtocolActor::quark_type_name),
          "Tier 4: an actor with no protocol is exactly the hash of its pinned name", ok);
    check(type_key_of<AnonActor>() != type_key_of<conformance_named::NamedActor>(),
          "Tier 4 negative control: different pinned names => different keys", ok);

    // --- Normaliser, compiler-independent: every compiler's spelling of one type => one string. ----
    check(norm("ns::Foo") == "ns::Foo", "normalise: GCC/Clang names are untouched (their keys do not move)", ok);
    check(norm("struct ns::Foo") == "ns::Foo", "normalise: MSVC `struct ` elaborator dropped", ok);
    check(norm("class ns::Foo") == "ns::Foo" && norm("enum ns::E") == "ns::E" && norm("union ns::U") == "ns::U",
          "normalise: class/enum/union elaborators dropped", ok);
    check(norm("ns::W<struct ns::A,class ns::B>") == "ns::W<ns::A, ns::B>",
          "normalise: elaborators inside template args dropped and `,` gets one space (MSVC spelling)", ok);
    check(norm("ns::W<ns::A, ns::B>") == "ns::W<ns::A, ns::B>", "normalise: GCC/Clang template spelling untouched", ok);
    check(norm("{anonymous}::X") == "(anonymous namespace)::X", "normalise: GCC anonymous namespace", ok);
    check(norm("(anonymous namespace)::X") == "(anonymous namespace)::X", "normalise: Clang anonymous namespace untouched", ok);
    check(norm("`anonymous namespace'::X") == "(anonymous namespace)::X", "normalise: MSVC anonymous namespace", ok);
    check(norm("`anonymous-namespace'::X") == "(anonymous namespace)::X", "normalise: MSVC anonymous-namespace variant", ok);
    check(norm("mystruct ns::Foo") == "mystruct ns::Foo" && norm("ns::classy") == "ns::classy",
          "normalise: a keyword is only dropped at a token start, never inside an identifier", ok);
    check(norm("struct `anonymous namespace'::X") == "(anonymous namespace)::X",
          "normalise: MSVC spelling of an anonymous-namespace type collapses to Clang's", ok);
    static_assert(detail::type_name_hash<conformance_named::NamedTag>() == type_key_of<conformance_named::NamedTag>().value,
                  "the normalised name hash is a constant expression and is the key of a non-actor, non-Described type");

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
    std::printf("  Tier 2 (named, NamedTag)                                = 0x%016llx\n",
                 static_cast<unsigned long long>(named_key));
    std::printf("  Tier 2 (named, NamedPair<int,double>)                   = 0x%016llx\n",
                 static_cast<unsigned long long>(pair_key));
    std::printf("  Tier 2 (quark_type_name, Pinned)                        = 0x%016llx\n",
                 static_cast<unsigned long long>(pinned_key));
    std::printf("  Tier 4 (actor, anon ns, size_t in protocol)             = 0x%016llx\n",
                static_cast<unsigned long long>(type_key_of<AnonActor>().value));
    std::printf("  Tier 4 (actor, named)                                   = 0x%016llx\n",
                static_cast<unsigned long long>(type_key_of<conformance_named::NamedActor>().value));
    std::printf("  Tier 4 (actor, no protocol)                             = 0x%016llx\n",
                static_cast<unsigned long long>(type_key_of<conformance_named::NoProtocolActor>().value));
    std::printf("  Tier 3 (builtin-spelling, Wrap<size_t>)                  = 0x%016llx\n",
                 static_cast<unsigned long long>(tier3_key));

    std::printf("metadata_typekey_conformance_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
