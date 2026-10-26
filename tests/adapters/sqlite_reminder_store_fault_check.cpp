// Fault/edge check for the SQLite reminder adapter (built only when QUARK_WITH_SQLITE=ON): the shared
// properties in reminder_store_fault_checks.hpp. The "broken" store is a garbage file (not a database):
// sqlite3_open succeeds but the schema step fails, so the store must be unhealthy and error everywhere.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "adapters/reminder_store_fault_checks.hpp"
#include "quark/adapters/sqlite_reminder_store.hpp"

using quark::adapters::SqliteReminderStore;

int main() {
    char tmpl[] = "/tmp/quark_sqlite_rem_fault_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string d = dir;
    int n = 0;
    int calls = 0;
    auto make = [&] {
        // make() call order: due round-trip (write, reopen), re-arm, checkpoint (write, reopen), concurrency.
        static const int path_of_call[] = {0, 0, 1, 2, 2, 3};
        const int idx = calls < 6 ? path_of_call[calls] : 4;
        ++calls;
        return SqliteReminderStore(d + "/s" + std::to_string(idx) + ".sqlite");
    };
    const int rc = quark::testkit::run_reminder_store_fault_checks<SqliteReminderStore>(
        "sqlite_reminder_store_fault_check", make, [&] {
            const std::string p = d + "/garbage" + std::to_string(n++) + ".sqlite";
            std::ofstream(p, std::ios::binary) << std::string(4096, 'x');
            return SqliteReminderStore(p);
        });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
