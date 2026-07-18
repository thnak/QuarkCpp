// Tests 016-Serialization §"Field identity is the tag" — additive schema evolution BOTH
// directions (ADR-016 §"evolution"): an OLD reader skips an unknown new tag; a NEW reader
// defaults a missing tag. Tags are identity, not position — so add/reorder is free.
#include <cstdint>
#include <cstdio>
#include <string>

#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

// V1 of a type: two fields at tags 1, 2.
struct EventV1 {
    std::uint64_t id;
    std::string name;
};

// V2 of the SAME logical type: adds tag 3 (priority) and tag 4 (retries). Tags 1,2 unchanged.
struct EventV2 {
    std::uint64_t id;
    std::string name;
    std::uint32_t priority;
    std::int64_t retries;
};

QUARK_SERIALIZE(EventV1, (1, id), (2, name))
QUARK_SERIALIZE(EventV2, (1, id), (2, name), (3, priority), (4, retries))

}  // namespace

int main() {
    std::uint64_t fails = 0;
    std::byte buf[256];

    // --- Forward compat: NEW reader over OLD bytes defaults the missing tags -----------------
    {
        EventV1 old{42, "spawn"};
        auto n = encode_tagged(old, buf, sizeof buf);
        EventV2 neu{};
        neu.priority = 7;  // pre-set to a sentinel; a missing tag must LEAVE the default...
        neu.retries = 9;   // ...i.e. whatever the caller default-constructed (here overwritten):
        // Decode requires a default-constructed target for "missing => default" semantics; use a
        // fresh value and assert the absent tags stay value-initialised (0).
        EventV2 fresh{};
        auto rc = decode_tagged(buf, *n, fresh);
        if (!rc || fresh.id != 42 || fresh.name != "spawn" || fresh.priority != 0 ||
            fresh.retries != 0) {
            std::fprintf(stderr, "forward-compat FAIL: new reader over old bytes\n");
            ++fails;
        }
        (void)neu;
    }

    // --- Backward compat: OLD reader over NEW bytes skips the unknown tags -------------------
    {
        EventV2 neu{99, "migrate", 5, -3};
        auto n = encode_tagged(neu, buf, sizeof buf);
        EventV1 old{};
        auto rc = decode_tagged(buf, *n, old);
        if (!rc || old.id != 99 || old.name != "migrate") {
            std::fprintf(stderr, "backward-compat FAIL: old reader over new bytes\n");
            ++fails;
        }
    }

    // Fingerprints must differ (adding a field changes the schema — negotiation would reject
    // the tagless fast path across this boundary; the tagged path handles it).
    if (fingerprint_v<EventV1> == fingerprint_v<EventV2>) {
        std::fprintf(stderr, "FAIL: V1 and V2 fingerprints collide\n");
        ++fails;
    }

    const bool pass = (fails == 0);
    std::printf("serialize_evolution_test: %s  (v1_fp=%016llx v2_fp=%016llx)\n",
                pass ? "OK" : "FAIL", static_cast<unsigned long long>(fingerprint_v<EventV1>),
                static_cast<unsigned long long>(fingerprint_v<EventV2>));
    return pass ? 0 : 1;
}
