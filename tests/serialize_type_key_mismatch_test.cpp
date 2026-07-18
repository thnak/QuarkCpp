// Tests 016-Serialization / 012 durable-record header — the type_key GUARD. A durable record carries
// {type_key, schema_version}; decode_record<T> (and read_migrated) must REFUSE bytes whose stored
// type_key is not the durable key of T, even when the tagged body would otherwise decode (a wrong
// type with a layout-compatible prefix). Defense-in-depth against silently reinterpreting a record
// as the wrong type. Before the fix the stored type_key was ignored and the wrong-type bytes decoded
// to garbage.
#include <cstdint>
#include <cstdio>

#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"  // encode_durable

using namespace quark;

namespace {

// Xrec has two fields; Yrec is its tag-1 PREFIX — so a raw tagged decode of Yrec over Xrec's bytes
// would SUCCEED (reads tag 1, ignores tag 2) and yield garbage. Their described shapes differ, so
// their 016 fingerprints differ — which is exactly what the type_key guard keys on.
struct Xrec {
    std::int64_t a = 0;
    std::int64_t b = 0;
};
QUARK_SERIALIZE(Xrec, (1, a), (2, b))

struct Yrec {
    std::int64_t a = 0;
};
QUARK_SERIALIZE(Yrec, (1, a))

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    // Distinct fingerprints ⇒ distinct durable type_keys (the guard has something to catch).
    static_assert(fingerprint_v<Xrec> != fingerprint_v<Yrec>,
                  "test premise: X and Y must have different durable keys");

    // A record genuinely of type X (durable key = fingerprint_v<Xrec>).
    auto bytes = encode_durable(Xrec{111, 222});
    check(bytes.has_value(), "encode X", ok);

    // Decoding as X succeeds (positive control).
    auto asX = decode_record<Xrec>(bytes->data(), bytes->size());
    check(asX.has_value() && asX->a == 111 && asX->b == 222, "decode as the correct type X", ok);

    // Decoding as Y (layout-compatible prefix) is REFUSED on the type_key mismatch — NOT silently
    // decoded to Yrec{111}.
    auto asY = decode_record<Yrec>(bytes->data(), bytes->size());
    check(!asY && asY.error().code == errc::serialization,
          "decode as the wrong type Y is a serialization error, not garbage", ok);

    // read_migrated (no evolution, chain head == Y) rejects the mismatch too.
    auto migY = read_migrated<Yrec, Yrec>(bytes->data(), bytes->size());
    check(!migY && migY.error().code == errc::serialization,
          "read_migrated also refuses the wrong type_key", ok);

    // read_migrated of the correct type still works (positive control).
    auto migX = read_migrated<Xrec, Xrec>(bytes->data(), bytes->size());
    check(migX.has_value() && migX->a == 111 && migX->b == 222, "read_migrated decodes the right type", ok);

    std::printf("serialize_type_key_mismatch_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
