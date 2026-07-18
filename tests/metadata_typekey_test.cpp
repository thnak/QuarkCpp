// Tests 008-Metadata-and-Startup §Type identity — the stable, content-addressed TypeKey.
//   1. STABILITY: the same type yields the same key on every derivation (deterministic).
//   2. UNIQUENESS: a reasonable set of distinct types yields distinct keys (no collision).
//   3. FINGERPRINT UNIFICATION: a Described type's TypeKey IS its 016 fingerprint — the durable
//      record header (016) and the 008 identity are the same number (the promised unification).
//   4. PROTOCOL FOLD: an actor's key folds its protocol, so two actors that differ only by protocol
//      get distinct keys.
#include <cstdio>
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
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};
struct BetaActor : Actor<BetaActor, Sequential> {
    using protocol = Protocol<Pong>;
    void handle(const Pong&) {}
};
// Same *name shape* as AlphaActor but a different protocol — must get a distinct key (fold).
struct GammaActor : Actor<GammaActor, Sequential> {
    using protocol = Protocol<Ping, Pong>;
    void handle(const Ping&) {}
    void handle(const Pong&) {}
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

    std::printf("metadata_typekey_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
