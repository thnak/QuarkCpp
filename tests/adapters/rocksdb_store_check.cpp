// Store-conformance + crash-durability check for the RocksDB adapter (built only when
// QUARK_WITH_ROCKSDB=ON). Reopens a RocksStore on the same DB directory to model a restart.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "quark/adapters/rocksdb_store.hpp"
#include "adapters/store_conformance.hpp"

int main() {
    char tmpl[] = "/tmp/quark_rocksdb_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string dbdir = std::string(dir) + "/rocks";

    const int rc = quark::testkit::run_store_conformance(
        "rocksdb_store_check", [&] { return quark::adapters::RocksStore(dbdir); });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
