// Fault-handling check for the RocksDB adapter (built only when QUARK_WITH_ROCKSDB=ON): the shared
// properties in store_fault_checks.hpp. The "broken" store is one whose database path lies UNDER a
// regular file, so DB::Open fails and the store is unhealthy — every op must then be an error (the old
// adapter returned "no snapshot" / an empty log / a plausible token).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "adapters/store_fault_checks.hpp"
#include "quark/adapters/rocksdb_store.hpp"

using quark::adapters::RocksStore;

int main() {
    char tmpl[] = "/tmp/quark_rocks_fault_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string d = dir;
    int n = 0;

    const int rc = quark::testkit::run_store_fault_checks<RocksStore>(
        "rocksdb_store_fault_check", [&] { return RocksStore(d + "/db" + std::to_string(n++)); },
        [&] {
            const std::string file = d + "/not_a_dir" + std::to_string(n++);
            std::ofstream(file) << "x";
            return RocksStore(file + "/db");  // parent is a regular file => Open fails
        });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
