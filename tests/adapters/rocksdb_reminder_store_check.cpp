// ReminderStore-conformance + crash-durability check for the RocksDB reminder adapter (built only when
// QUARK_WITH_ROCKSDB=ON). Reopens a RocksReminderStore on the same DB directory to model a restart.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "quark/adapters/rocksdb_reminder_store.hpp"
#include "adapters/reminder_store_conformance.hpp"

int main() {
    char tmpl[] = "/tmp/quark_rocksdb_rem_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string dbdir = std::string(dir) + "/reminders.rocksdb";

    const int rc = quark::testkit::run_reminder_store_conformance(
        "rocksdb_reminder_store_check", [&] { return quark::adapters::RocksReminderStore(dbdir); });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
