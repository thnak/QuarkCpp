// Fault/edge check for the RocksDB reminder adapter (built only when QUARK_WITH_ROCKSDB=ON): the shared
// properties in reminder_store_fault_checks.hpp. The "broken" store's database path lies UNDER a regular
// file, so DB::Open fails and the store is unhealthy.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "adapters/reminder_store_fault_checks.hpp"
#include "quark/adapters/rocksdb_reminder_store.hpp"

using quark::adapters::RocksReminderStore;

int main() {
    char tmpl[] = "/tmp/quark_rocks_rem_fault_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string d = dir;
    int n = 0;
    // `make` is called several times per scenario; scenarios that reopen must hit the SAME path, so the
    // factory counts in groups: a fresh path per scenario is chosen by the call index below.
    int calls = 0;
    auto make = [&] {
        // make() call order in the harness: due round-trip (write, reopen) -> re-arm -> checkpoint
        // (write, reopen) -> concurrency. Reopen pairs must share a path; map call index -> path.
        static const int path_of_call[] = {0, 0, 1, 2, 2, 3};
        const int idx = calls < 6 ? path_of_call[calls] : 4;
        ++calls;
        return RocksReminderStore(d + "/db" + std::to_string(idx));
    };
    const int rc = quark::testkit::run_reminder_store_fault_checks<RocksReminderStore>(
        "rocksdb_reminder_store_fault_check", make, [&] {
            const std::string file = d + "/not_a_dir" + std::to_string(n++);
            std::ofstream(file) << "x";
            return RocksReminderStore(file + "/db");
        });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
