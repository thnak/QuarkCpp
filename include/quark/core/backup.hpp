// Implements 012-Persistence §Backup/restore + ADR-055 (#23): the portable BYTE form of a
// `BackupImage` (persistence.hpp) and file helpers over any `BackupableStore`.
//
// Format (host-endian, like the FileStore WAL; a portable build byte-swaps — same noted seam):
//   "QBAK0001" | u64 epoch | u32 actor_count | actor* | u32 crc32(everything before)
//   actor = u64 type | u64 key | u64 owner | u64 last_seq | u8 has_snapshot
//           [u64 fence | u64 through_seq | blob] | u32 n_events | (u64 seq | blob)*
//   blob  = u32 len | bytes
// The trailing CRC catches truncation/bit-rot of the image file; it is NOT a MAC (an attacker who can
// rewrite the file can forge an image — protect the file like the store itself).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "pal/file_io.hpp"
#include "quark/core/error.hpp"
#include "quark/core/persistence.hpp"

namespace quark {

namespace backup_detail {
inline std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFF'FFFFu;
    for (std::byte b : data) c = table[(c ^ static_cast<std::uint8_t>(b)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFF'FFFFu;
}
inline void put(std::vector<std::byte>& b, const void* p, std::size_t n) {
    const auto* s = static_cast<const std::byte*>(p);
    b.insert(b.end(), s, s + n);
}
inline void u8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(std::byte{v}); }
inline void u32(std::vector<std::byte>& b, std::uint32_t v) { put(b, &v, 4); }
inline void u64(std::vector<std::byte>& b, std::uint64_t v) { put(b, &v, 8); }
inline void blob(std::vector<std::byte>& b, const std::vector<std::byte>& v) {
    u32(b, static_cast<std::uint32_t>(v.size()));
    b.insert(b.end(), v.begin(), v.end());
}
struct Rd {
    std::span<const std::byte> b;
    std::size_t p = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (!ok || n > b.size() - p) return ok = false;
        return true;
    }
    std::uint8_t r8() { return need(1) ? static_cast<std::uint8_t>(b[p++]) : 0; }
    std::uint32_t r32() {
        std::uint32_t v = 0;
        if (!need(4)) return 0;
        std::memcpy(&v, b.data() + p, 4);
        p += 4;
        return v;
    }
    std::uint64_t r64() {
        std::uint64_t v = 0;
        if (!need(8)) return 0;
        std::memcpy(&v, b.data() + p, 8);
        p += 8;
        return v;
    }
    std::vector<std::byte> rblob() {
        const std::uint32_t n = r32();
        if (!need(n)) return {};
        std::vector<std::byte> v(b.begin() + static_cast<std::ptrdiff_t>(p),
                                 b.begin() + static_cast<std::ptrdiff_t>(p) + n);
        p += n;
        return v;
    }
};
inline constexpr char kMagic[8] = {'Q', 'B', 'A', 'K', '0', '0', '0', '1'};
}  // namespace backup_detail

[[nodiscard]] inline std::vector<std::byte> encode_backup(const BackupImage& img) {
    using namespace backup_detail;
    std::vector<std::byte> out;
    put(out, kMagic, sizeof(kMagic));
    u64(out, img.epoch.value);
    u32(out, static_cast<std::uint32_t>(img.actors.size()));
    for (const ActorBackup& a : img.actors) {
        u64(out, a.id.type.value);
        u64(out, a.id.key);
        u64(out, a.owner.value);
        u64(out, a.last_seq);
        u8(out, a.snapshot ? 1 : 0);
        if (a.snapshot) {
            u64(out, a.snapshot->fence.value);
            u64(out, a.snapshot->through_seq);
            blob(out, a.snapshot->record);
        }
        u32(out, static_cast<std::uint32_t>(a.log.size()));
        for (const EventRecord& e : a.log) {
            u64(out, e.seq);
            blob(out, e.record);
        }
    }
    u32(out, crc32(std::span<const std::byte>(out)));
    return out;
}

[[nodiscard]] inline result<BackupImage> decode_backup(std::span<const std::byte> bytes) {
    using namespace backup_detail;
    const auto bad = [](const char* m) { return std::unexpected(error{errc::serialization, m}); };
    if (bytes.size() < sizeof(kMagic) + 4 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return bad("backup: bad magic/short image");
    std::uint32_t want = 0;
    std::memcpy(&want, bytes.data() + bytes.size() - 4, 4);
    if (crc32(bytes.first(bytes.size() - 4)) != want) return bad("backup: CRC mismatch");
    Rd r{bytes.first(bytes.size() - 4), sizeof(kMagic)};
    BackupImage img;
    img.epoch.value = r.r64();
    const std::uint32_t n = r.r32();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        ActorBackup a;
        a.id.type.value = r.r64();
        a.id.key = r.r64();
        a.owner.value = r.r64();
        a.last_seq = r.r64();
        if (r.r8() == 1) {
            SnapshotRecord s;
            s.fence.value = r.r64();
            s.through_seq = r.r64();
            s.record = r.rblob();
            a.snapshot = std::move(s);
        }
        const std::uint32_t ne = r.r32();
        for (std::uint32_t k = 0; k < ne && r.ok; ++k) {
            EventRecord e;
            e.seq = r.r64();
            e.record = r.rblob();
            a.log.push_back(std::move(e));
        }
        img.actors.push_back(std::move(a));
    }
    if (!r.ok || r.p != r.b.size()) return bad("backup: malformed image");
    if (auto v = validate_backup(img); !v) return std::unexpected(v.error());
    return img;
}

// Take an online backup of `store` into `path` (written to `<path>.tmp`, flushed, atomically renamed,
// so a reader never sees a half-written image). Writers to `store` continue; see persistence.hpp for
// the consistency model.
template <BackupableStore S>
[[nodiscard]] result<void> snapshot_to_file(S& store, const std::string& path) {
    BackupImage img;
    if (auto rc = store.snapshot_to(img); !rc) return rc;
    const std::vector<std::byte> bytes = encode_backup(img);
    const std::string tmp = path + ".tmp";
    pal::file_t f = pal::file_open(tmp, pal::FileOpenMode::kWriteCreateTrunc);
    if (f == pal::invalid_file) return fail(errc::internal, "backup: cannot create image file");
    const bool ok = pal::file_pwrite(f, bytes.data(), bytes.size(), 0) ==
                        static_cast<std::int64_t>(bytes.size()) &&
                    pal::durable_flush(f);
    pal::file_close(f);
    if (!ok || !pal::durable_rename(tmp, path)) {
        (void)std::remove(tmp.c_str());
        return fail(errc::internal, "backup: writing the image file failed");
    }
    return {};
}

// Load the image at `path` into the FRESH `store`, installing a fencing epoch above every token in the
// image and `min_epoch`. Returns the installed epoch.
template <BackupableStore S>
[[nodiscard]] result<FencingEpoch> restore_from_file(S& store, const std::string& path,
                                                     FencingEpoch min_epoch = {}) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(errc::not_found, "restore: cannot open image file");
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto img = decode_backup(std::as_bytes(std::span<const char>(raw)));
    if (!img) return std::unexpected(img.error());
    return store.restore_from(*img, min_epoch);
}

}  // namespace quark
