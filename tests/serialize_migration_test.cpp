// Tests 016-Serialization §"Migrations" — a registered QUARK_MIGRATE chain applied v→v+1→…→
// current on read (ADR-016 §"Migration chain v1→v2→v3"). A durable record persisted at an older
// schema_version is decoded as its own version, then upcast through the chain to the current
// type before it reaches a handler. Event logs depend on this: old shapes stay readable forever.
#include <cstdint>
#include <cstdio>
#include <string>

#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

// A durable "Account" type that evolved across three breaking versions.
struct AccountV1 {
    std::uint64_t id;
    std::uint32_t balance_cents;  // v1 stored an unsigned cent count
};

struct AccountV2 {
    std::uint64_t id;
    std::int64_t balance_cents;  // v2 widened to signed (overdrafts) — breaking type change
    std::string currency;        // v2 added a currency
};

struct AccountV3 {
    std::uint64_t id;
    std::int64_t balance_millis;  // v3 changed units cents->millis — breaking meaning change
    std::string currency;
    bool frozen;  // v3 added a flag
};

QUARK_SERIALIZE(AccountV1, (1, id), (2, balance_cents))
QUARK_SERIALIZE(AccountV2, (1, id), (2, balance_cents), (3, currency))
QUARK_SERIALIZE(AccountV3, (1, id), (2, balance_millis), (3, currency), (4, frozen))

}  // namespace

// Version + migration registration open namespace quark:: so they live at global scope; they
// may reference the anonymous-namespace types above.
QUARK_SCHEMA_VERSION(AccountV1, 1);
QUARK_SCHEMA_VERSION(AccountV2, 2);
QUARK_SCHEMA_VERSION(AccountV3, 3);

// v1 -> v2: widen to signed, default currency to USD.
QUARK_MIGRATE(AccountV1, AccountV2, [](const AccountV1& o) {
    return AccountV2{o.id, static_cast<std::int64_t>(o.balance_cents), "USD"};
});
// v2 -> v3: rescale cents -> millis (x10), default frozen=false.
QUARK_MIGRATE(AccountV2, AccountV3, [](const AccountV2& o) {
    return AccountV3{o.id, o.balance_cents * 10, o.currency, false};
});

int main() {
    std::uint64_t fails = 0;
    std::byte buf[256];
    // Durable header type_key (008 seam): each schema version carries the durable key of ITS OWN
    // type — its 016 fingerprint — so read_migrated can reject a record whose key does not match the
    // type at the claimed schema version (defense-in-depth against decoding the wrong type).
    const TypeKey v1_key{fingerprint_v<AccountV1>};
    const TypeKey v2_key{fingerprint_v<AccountV2>};
    const TypeKey v3_key{fingerprint_v<AccountV3>};

    // --- A v1 record read forward to v3 (two-hop chain) --------------------------------------
    {
        AccountV1 v1{7, 1234};
        auto n = encode_record(v1, v1_key, buf, sizeof buf);
        auto hdr = peek_record_header(buf, *n);
        auto acct = read_migrated<AccountV3, AccountV1>(buf, *n);
        if (!n || !hdr || hdr->schema_version != 1 || !acct || acct->id != 7 ||
            acct->balance_millis != 12340 /* 1234c -> 12340m */ || acct->currency != "USD" ||
            acct->frozen) {
            std::fprintf(stderr, "v1->v3 migration FAIL\n");
            ++fails;
        }
    }

    // --- A v2 record read forward to v3 (one-hop chain) --------------------------------------
    {
        AccountV2 v2{8, -500, "EUR"};
        auto n = encode_record(v2, v2_key, buf, sizeof buf);
        auto acct = read_migrated<AccountV3, AccountV1>(buf, *n);
        if (!n || !acct || acct->id != 8 || acct->balance_millis != -5000 ||
            acct->currency != "EUR" || acct->frozen) {
            std::fprintf(stderr, "v2->v3 migration FAIL\n");
            ++fails;
        }
    }

    // --- A current (v3) record needs no migration (identity hop) -----------------------------
    {
        AccountV3 v3{9, 777, "GBP", true};
        auto n = encode_record(v3, v3_key, buf, sizeof buf);
        auto acct = read_migrated<AccountV3, AccountV1>(buf, *n);
        if (!n || !acct || acct->id != 9 || acct->balance_millis != 777 ||
            acct->currency != "GBP" || !acct->frozen) {
            std::fprintf(stderr, "v3 identity FAIL\n");
            ++fails;
        }
    }

    // --- An unknown future version is a serialization error (008 drift detection) ------------
    {
        // Hand-forge a header with schema_version = 99 (not in the chain).
        std::byte bad[64];
        detail::ByteWriter w(bad, sizeof bad);
        (void)detail::put_fixed_le<std::uint64_t>(w, v1_key.value);
        (void)detail::put_fixed_le<std::uint32_t>(w, 99u);
        auto acct = read_migrated<AccountV3, AccountV1>(bad, w.written());
        if (acct || acct.error().code != errc::serialization) {
            std::fprintf(stderr, "unknown-version drift NOT detected\n");
            ++fails;
        }
    }

    const bool pass = (fails == 0);
    std::printf("serialize_migration_test: %s  (v1=%u v2=%u v3=%u)\n", pass ? "OK" : "FAIL",
                schema_version_v<AccountV1>, schema_version_v<AccountV2>,
                schema_version_v<AccountV3>);
    return pass ? 0 : 1;
}
