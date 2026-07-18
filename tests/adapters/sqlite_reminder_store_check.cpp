// ReminderStore-conformance + crash-durability check for the SQLite reminder adapter (built only when
// QUARK_WITH_SQLITE=ON). Reopens a SqliteReminderStore on the same DB file to model a restart.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "quark/adapters/sqlite_reminder_store.hpp"
#include "adapters/reminder_store_conformance.hpp"

int main() {
    char tmpl[] = "/tmp/quark_sqlite_rem_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string dbfile = std::string(dir) + "/reminders.sqlite";

    const int rc = quark::testkit::run_reminder_store_conformance(
        "sqlite_reminder_store_check", [&] { return quark::adapters::SqliteReminderStore(dbfile); });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
