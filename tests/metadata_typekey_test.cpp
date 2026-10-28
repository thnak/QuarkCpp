// Tests 008-Metadata-and-Startup §Type identity — the stable, content-addressed TypeKey.
//   1. STABILITY: the same type yields the same key on every derivation (deterministic).
//   2. UNIQUENESS: a reasonable set of distinct types yields distinct keys (no collision).
//   3. FINGERPRINT UNIFICATION: a Described type's TypeKey IS its 016 fingerprint — the durable
//      record header (016) and the 008 identity are the same number (the promised unification).
//   4. PROTOCOL FOLD: an actor's key folds its protocol, so two actors that differ only by protocol
//      get distinct keys.
#include <array>
#include <cstdio>
#include <string_view>
#include <unordered_set>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/describe.hpp"
#include "quark/core/metadata.hpp"

using namespace quark;

namespace {

struct Ping {};
struct Pong {};
struct Point {
    int x = 0;
    int y = 0;
};
struct Segment {
    Point a{};
    Point b{};
};

struct AlphaActor : Actor<AlphaActor, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.metadata_typekey_test.AlphaActor";
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};
struct BetaActor : Actor<BetaActor, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.metadata_typekey_test.BetaActor";
    using protocol = Protocol<Pong>;
    void handle(const Pong&) {}
};
// Same *name shape* as AlphaActor but a different protocol — must get a distinct key (fold).
struct GammaActor : Actor<GammaActor, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.metadata_typekey_test.GammaActor";
    using protocol = Protocol<Ping, Pong>;
    void handle(const Ping&) {}
    void handle(const Pong&) {}
};

// ADR-061 (E5): `quark_type_name` pins an actor's identity to a string, so two actors that pin the SAME
// name AND share a protocol have one key — the startup collision scan must reject the second.
struct PinnedOne : Actor<PinnedOne, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.Pinned";
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};
struct PinnedTwo : Actor<PinnedTwo, Sequential> {  // different C++ name, same pinned name + protocol
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.Pinned";
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};
// Same pinned name, wider protocol of NON-Described messages: local-only, never on the wire or in a
// durable record, so it does NOT enter the key (ADR-062) — identity is the declared name.
struct PinnedWide : Actor<PinnedWide, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.Pinned";
    using protocol = Protocol<Ping, Pong>;
    void handle(const Ping&) {}
    void handle(const Pong&) {}
};
struct Wide {  // a DESCRIBED message: its 016 fingerprint folds into the actor key
    int v = 0;
};
QUARK_SERIALIZE(Wide, (1, v))
struct PinnedDescribed : Actor<PinnedDescribed, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.Pinned";
    using protocol = Protocol<Ping, Wide>;
    void handle(const Ping&) {}
    void handle(const Wide&) {}
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// QUARK_SERIALIZE must be invoked in the SAME namespace as the type so the generated
// quark_describe is found by ADL (a type's associated namespace is its enclosing one).
QUARK_SERIALIZE(Point, (1, x), (2, y))
QUARK_SERIALIZE(Segment, (1, a), (2, b))

}  // namespace

int main() {
    bool ok = true;

    // 1) STABILITY — same type → same key, every time; constexpr-usable.
    static_assert(type_key_of<Point>() == type_key_of<Point>(), "TypeKey must be stable (constexpr)");
    check(type_key_of<AlphaActor>() == type_key_of<AlphaActor>(), "actor key stable", ok);
    check(actor_id_of<AlphaActor>(7).type == type_key_of<AlphaActor>(), "actor_id_of uses type key", ok);

    // 2) UNIQUENESS — a reasonable set of distinct types has no key collision.
    std::unordered_set<std::uint64_t> keys;
    keys.insert(type_key_of<AlphaActor>().value);
    keys.insert(type_key_of<BetaActor>().value);
    keys.insert(type_key_of<GammaActor>().value);
    keys.insert(type_key_of<Point>().value);
    keys.insert(type_key_of<Segment>().value);
    keys.insert(type_key_of<Ping>().value);
    keys.insert(type_key_of<Pong>().value);
    check(keys.size() == 7, "7 distinct types → 7 distinct keys (no collision)", ok);

    // 3) FINGERPRINT UNIFICATION — a Described type's key equals its 016 fingerprint exactly.
    static_assert(type_key_of<Point>().value == fingerprint_v<Point>,
                  "008 TypeKey must equal the 016 fingerprint for a Described type");
    check(type_key_of<Segment>().value == fingerprint_v<Segment>,
          "described TypeKey == 016 fingerprint (durable header unification)", ok);

    // 4) PROTOCOL FOLD — AlphaActor(<Ping>) and GammaActor(<Ping,Pong>) differ by protocol only.
    check(type_key_of<AlphaActor>() != type_key_of<GammaActor>(),
          "actors differing by protocol get distinct keys (fold)", ok);

    // 5) ADR-061 quark_type_name override: key is the pinned name folded with the protocol — C++ name
    // independent — and the startup collision scan still trips on two actors that pin the same identity.
    check(type_key_of<PinnedOne>() == type_key_of<PinnedTwo>(),
          "quark_type_name: same pinned name + protocol => same key regardless of C++ name", ok);
    check(type_key_of<PinnedOne>() == type_key_of<PinnedWide>(),
          "quark_type_name: non-Described (local-only) messages do not enter the key", ok);
    check(type_key_of<PinnedOne>() != type_key_of<PinnedDescribed>(),
          "quark_type_name: a Described message in the protocol DOES fold into the key", ok);
    check(type_key_of<PinnedOne>() != type_key_of<AlphaActor>(),
          "different pinned names => different keys even with an identical protocol", ok);
    {
        TypeRegistry reg(Validation::Strict);
        result<std::uint16_t> a = reg.register_metadata(compile_actor_metadata<PinnedOne>(), "PinnedOne",
                                                        [] { return result<void>{}; });
        result<std::uint16_t> b = reg.register_metadata(compile_actor_metadata<PinnedTwo>(), "PinnedTwo",
                                                        [] { return result<void>{}; });
        check(a.has_value(), "quark_type_name: first pinned actor registers", ok);
        check(!b.has_value() && b.error().code == errc::validation,
              "quark_type_name: second actor pinning the same identity is rejected at startup (Strict)", ok);
    }

    std::printf("metadata_typekey_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// ADR-062 actor type-name rules (compile-time): 1..128 chars of [A-Za-z0-9._-], no leading/trailing '.', no "..".
static_assert(detail::valid_actor_type_name("app.billing.Ledger"), "ADR-062: dotted name is valid");
static_assert(detail::valid_actor_type_name("quark.test.engine_send_pool_partitions_test.Counter"),
              "ADR-062: in-tree test name is valid");
static_assert(detail::valid_actor_type_name("quark.sample.01_hello-counter.Counter"), "ADR-062: '-' and '_' are valid");
static_assert(detail::valid_actor_type_name("A"), "ADR-062: single character is valid");
// Exact-length boundary inputs (128 valid, 129 invalid), generated at compile time.
template <std::size_t N>
constexpr std::array<char, N> repeat_a() {
    std::array<char, N> a{};
    for (auto& c : a) c = 'a';
    return a;
}
constexpr auto kLen128 = repeat_a<128>();
constexpr auto kLen129 = repeat_a<129>();
static_assert(detail::valid_actor_type_name(std::string_view{kLen128.data(), kLen128.size()}),
              "ADR-062: 128 characters is valid (boundary)");
static_assert(!detail::valid_actor_type_name(""), "ADR-062: empty is rejected");
static_assert(!detail::valid_actor_type_name(std::string_view{kLen129.data(), kLen129.size()}),
              "ADR-062: 129 characters is rejected (boundary)");
static_assert(!detail::valid_actor_type_name("app billing.Ledger"), "ADR-062: space is rejected");
static_assert(!detail::valid_actor_type_name("app/billing/Ledger"), "ADR-062: '/' is rejected");
static_assert(!detail::valid_actor_type_name("app!"), "ADR-062: '!' is rejected");
static_assert(!detail::valid_actor_type_name(".app.Ledger"), "ADR-062: leading '.' is rejected");
static_assert(!detail::valid_actor_type_name("app.Ledger."), "ADR-062: trailing '.' is rejected");
static_assert(!detail::valid_actor_type_name("a..b"), "ADR-062: \"..\" is rejected");
static_assert(!detail::valid_actor_type_name("caf\xC3\xA9.Ledger"), "ADR-062: non-ASCII byte is rejected");
static_assert(!detail::valid_actor_type_name("app.\xFF"), "ADR-062: high-bit byte is rejected");
