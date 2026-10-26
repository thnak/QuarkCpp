// Fault-handling check for the SQLite adapter (built only when QUARK_WITH_SQLITE=ON): see
// store_fault_checks.hpp for the shared properties, plus SQLite-specific injected faults:
//   * a garbage file (not a database) leaves the store unhealthy — it never looks like an empty store;
//   * another connection holding the write lock makes BEGIN fail: acquire_fence and append must return
//     an ERROR (the old code ignored a failed BEGIN) and leave the log unchanged.
#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "adapters/store_fault_checks.hpp"
#include "quark/adapters/sqlite_store.hpp"

using quark::adapters::SqliteStore;

int main() {
    char tmpl[] = "/tmp/quark_sqlite_fault_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string d = dir;
    int n = 0;
    auto fresh = [&] { return d + "/s" + std::to_string(n++) + ".sqlite"; };

    int rc = quark::testkit::run_store_fault_checks<SqliteStore>(
        "sqlite_store_fault_check", [&] { return SqliteStore(fresh()); },
        [&] {
            // A regular file full of garbage: sqlite3_open succeeds but the schema PRAGMA fails.
            const std::string p = fresh();
            std::ofstream(p, std::ios::binary) << std::string(4096, 'x');
            return SqliteStore(p);
        });

    // ---- a held write lock makes BEGIN fail: errors, no partial writes --------------------------
    bool ok = true;
    auto check = [&](bool c, const char* what) {
        if (!c) { std::fprintf(stderr, "  [sqlite busy] CHECK FAILED: %s\n", what); ok = false; }
    };
    {
        const std::string p = fresh();
        const quark::ActorId id{quark::TypeKey{0xB0B}, 1};
        SqliteStore store(p);
        const auto f = store.acquire_fence(id);
        check(f.has_value(), "fence acquired on a healthy store");
        const std::vector<std::byte> payload(4, std::byte{1});
        check(f.has_value() && store.append(id, *f, 1, payload).has_value(), "baseline append");

        sqlite3* other = nullptr;
        check(sqlite3_open(p.c_str(), &other) == SQLITE_OK, "second connection opens");
        check(sqlite3_exec(other, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK,
              "the second connection takes the write lock");

        check(!store.acquire_fence(id).has_value(), "acquire_fence under a held write lock is an ERROR");
        check(f.has_value() && !store.append(id, *f, 2, payload).has_value(),
              "append under a held write lock is an ERROR (a failed BEGIN is not ignored)");
        check(f.has_value() &&
                  !store.save_snapshot(id, *f, quark::SnapshotRecord{*f, 1, payload}).has_value(),
              "save_snapshot under a held write lock is an ERROR");

        sqlite3_exec(other, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(other);
        check(store.last_seq(id) == 1, "nothing was written while the lock was held");
        check(f.has_value() && store.append(id, *f, 2, payload).has_value(),
              "the store recovers once the lock is released");
        check(store.last_seq(id) == 2, "the post-recovery append committed");
    }
    std::printf("sqlite_store_fault_check (busy): %s\n", ok ? "OK" : "FAIL");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return (rc == 0 && ok) ? 0 : 1;
}
