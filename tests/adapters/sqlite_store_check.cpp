// Store-conformance + crash-durability check for the SQLite adapter (built only when
// QUARK_WITH_SQLITE=ON). Reopens a SqliteStore on the same DB file to model a restart.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "quark/adapters/sqlite_store.hpp"
#include "adapters/store_conformance.hpp"

int main() {
    char tmpl[] = "/tmp/quark_sqlite_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string dbfile = std::string(dir) + "/store.sqlite";

    const int rc = quark::testkit::run_store_conformance(
        "sqlite_store_check", [&] { return quark::adapters::SqliteStore(dbfile); });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
