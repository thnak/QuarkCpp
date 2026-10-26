// Tests 012-Persistence FileStore replay hardening (review B8/B9, REVIEW-BACKLOG.md): replay must not
// trust the CONTENTS of a record just because its CRC32 is valid (a CRC is not a MAC, and a buggy or
// future writer can emit a well-framed but malformed record):
//   * a CRC-valid record that is truncated / has an oversized blob length / an unknown kind / is empty
//     marks the log CORRUPT (refused, never truncated) instead of reading past the record (UB/OOB —
//     this test is the ASan gate for that);
//   * batch sequence numbers that go backwards (within a record or across records) are corruption;
//   * a well-formed hand-built log still replays, so the checks are not over-eager;
//   * creating a new per-actor log makes its directory entry durable (B9: PAL dir-sync barrier).
// Rollback / whole-file truncation detection is NOT attempted (needs a design decision, see backlog).
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "quark/core/file_store.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

std::filesystem::path unique_dir() {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("quark_fs_replay_" + std::to_string(static_cast<long long>(n)));
}

std::filesystem::path wal_path(const std::filesystem::path& dir, ActorId id) {
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx-%016llx.qwal",
                  static_cast<unsigned long long>(id.type.value),
                  static_cast<unsigned long long>(id.key));
    return dir / name;
}

std::uint32_t crc32(const std::vector<std::byte>& data) {
    std::uint32_t c = 0xFFFF'FFFFu;
    for (std::byte b : data) {
        c ^= static_cast<std::uint8_t>(b);
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFF'FFFFu;
}

// Little builder for record bodies (host-endian, matching FileStore's byte format).
struct Body {
    std::vector<std::byte> b;
    Body& u8(std::uint8_t v) { b.push_back(std::byte{v}); return *this; }
    Body& u32(std::uint32_t v) { raw(&v, 4); return *this; }
    Body& u64(std::uint64_t v) { raw(&v, 8); return *this; }
    Body& blob(std::size_t n) { u32(static_cast<std::uint32_t>(n)); b.insert(b.end(), n, std::byte{0x42}); return *this; }
    void raw(const void* p, std::size_t n) {
        const auto* s = static_cast<const std::byte*>(p);
        b.insert(b.end(), s, s + n);
    }
};

// Append one {len, crc, body} frame with a VALID crc over exactly `body`.
void frame(std::vector<std::byte>& file, const std::vector<std::byte>& body) {
    const std::uint32_t len = static_cast<std::uint32_t>(body.size());
    const std::uint32_t crc = crc32(body);
    const auto* l = reinterpret_cast<const std::byte*>(&len);
    const auto* c = reinterpret_cast<const std::byte*>(&crc);
    file.insert(file.end(), l, l + 4);
    file.insert(file.end(), c, c + 4);
    file.insert(file.end(), body.begin(), body.end());
}

void write_file(const std::filesystem::path& p, const std::vector<std::byte>& v) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
}

constexpr std::uint8_t kFence = 1, kSnapshot = 2, kBatch = 3;

Body fence_rec(std::uint64_t tok) { return Body{}.u8(kFence).u64(tok); }
// Batch with explicit seqs, each carrying a small blob.
Body batch_rec(std::initializer_list<std::uint64_t> seqs) {
    Body b;
    b.u8(kBatch).u32(static_cast<std::uint32_t>(seqs.size()));
    for (auto s : seqs) b.u64(s).blob(4);
    return b;
}

struct Case { const char* name; std::vector<std::vector<std::byte>> records; bool expect_corrupt; };

bool run_case(const Case& c, int idx, bool& ok) {
    const auto dir = unique_dir();
    std::filesystem::create_directories(dir);
    const ActorId id{TypeKey{0x5E}, static_cast<std::uint64_t>(idx + 1)};
    std::vector<std::byte> file;
    for (const auto& r : c.records) frame(file, r);
    write_file(wal_path(dir, id), file);
    const auto before = std::filesystem::file_size(wal_path(dir, id));
    {
        FileStore store(dir.string());
        const bool corrupt = store.corrupted(id);
        check(corrupt == c.expect_corrupt, (std::string(c.name) + ": corrupt flag").c_str(), ok);
        if (c.expect_corrupt) {
            check(!store.read_log(id, 0).has_value(), (std::string(c.name) + ": read_log refused").c_str(), ok);
            check(!store.load_snapshot(id).has_value(), (std::string(c.name) + ": load_snapshot refused").c_str(), ok);
            check(store.acquire_fence(id).value == 0, (std::string(c.name) + ": no fence handed out").c_str(), ok);
        } else {
            auto cur = store.read_log(id, 0);
            check(cur.has_value() && cur->entries.size() == 3 && store.last_seq(id) == 3,
                  (std::string(c.name) + ": well-formed log replays all 3 events").c_str(), ok);
        }
    }
    check(std::filesystem::file_size(wal_path(dir, id)) == before,
          (std::string(c.name) + ": the file is never truncated by replay").c_str(), ok);
    std::filesystem::remove_all(dir);
    return ok;
}

bool test_dir_entry_made_durable(bool& ok) {
    // Behavioural floor for B9: opening an actor creates its log and the store still works end to end;
    // the barrier itself (pal::durable_sync_dir) is exercised in persistence_reminder_store_hardening.
    const auto dir = unique_dir();
    {
        FileStore store(dir.string());
        const ActorId id{TypeKey{0x5E}, 99};
        const FenceToken f = store.acquire_fence(id);
        check(f.value >= 1 && std::filesystem::exists(wal_path(dir, id)), "new actor log created + fenced", ok);
        check(pal::durable_sync_dir(dir.string()), "PAL dir sync works on the store root", ok);
    }
    std::filesystem::remove_all(dir);
    return ok;
}
}  // namespace

int main() {
    bool ok = true;
    const std::vector<Case> cases = {
        {"well_formed", {fence_rec(1).b, batch_rec({1, 2}).b, batch_rec({3}).b}, false},
        // batch says 5 events, body holds 1 (CRC covers exactly the short body)
        {"batch_count_exceeds_body",
         {fence_rec(1).b, Body{}.u8(kBatch).u32(5).u64(1).blob(4).b}, true},
        // blob length far larger than what is left of the record
        {"blob_len_oversized",
         {fence_rec(1).b, Body{}.u8(kBatch).u32(1).u64(1).u32(0xFFFF'FFF0u).b}, true},
        {"snapshot_blob_len_oversized",
         {fence_rec(1).b, Body{}.u8(kSnapshot).u64(1).u64(0).u32(0x7FFF'FFFFu).b}, true},
        // fence record cut short (only 3 of 8 token bytes)
        {"fence_truncated", {Body{}.u8(kFence).u8(1).u8(2).u8(3).b}, true},
        // zero-length body: no kind byte at all
        {"empty_record", {std::vector<std::byte>{}}, true},
        {"unknown_kind", {fence_rec(1).b, Body{}.u8(9).u64(0).b}, true},
        // seqs go backwards inside one batch
        {"batch_seq_backwards_within", {fence_rec(1).b, batch_rec({5, 3}).b}, true},
        // ... and across two records (second batch restarts at 2 after last_seq 4)
        {"batch_seq_backwards_across", {fence_rec(1).b, batch_rec({3, 4}).b, batch_rec({2}).b}, true},
        // equal seq is also not strictly increasing
        {"batch_seq_repeat", {fence_rec(1).b, batch_rec({1}).b, batch_rec({1}).b}, true},
    };
    int idx = 0;
    for (const auto& c : cases) {
        bool cok = true;
        run_case(c, idx++, cok);
        std::printf("%s: %s\n", c.name, cok ? "ok" : "FAILED");
        ok = ok && cok;
    }
    bool dok = true;
    test_dir_entry_made_durable(dok);
    std::printf("dir_entry_made_durable: %s\n", dok ? "ok" : "FAILED");
    ok = ok && dok;
    std::printf(ok ? "persistence_filestore_replay_hardening: PASS\n"
                   : "persistence_filestore_replay_hardening: FAIL\n");
    return ok ? 0 : 1;
}
