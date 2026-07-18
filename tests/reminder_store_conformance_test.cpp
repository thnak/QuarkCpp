// 027 durable-ReminderStore conformance — the ON-BOX gate. Runs the shared reminder-store conformance
// harness against the std-only crash-durable `FileReminderStore`: put/remove/upsert/checkpoint, then
// REOPEN on the same `.qrem` path (the "crash") and assert every acknowledged write survived, upserts
// replace instead of duplicating, and same-name-different-actor keys stay distinct.
//
// This both verifies FileReminderStore AND exercises the harness that the (opt-in, not-compiled-here)
// SqliteReminderStore / RocksReminderStore checks reuse — so those adapters ship against a harness
// proven correct on this box. Auto-discovered (`*_test.cpp`); the quote-include resolves relative to
// this file's directory (tests/), so no extra include path is needed.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "quark/core/reminder_service.hpp"
#include "adapters/reminder_store_conformance.hpp"

int main() {
    char tmpl[] = "/tmp/quark_file_rem_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) { std::perror("mkdtemp"); return 1; }
    const std::string path = std::string(dir) + "/reminders.qrem";

    const int rc = quark::testkit::run_reminder_store_conformance(
        "file_reminder_store", [&] { return quark::FileReminderStore(path); });

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return rc;
}
