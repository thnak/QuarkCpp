// Implements 019-Platform-Abstraction-Layer §5 (durable file I/O & flush) — the Windows/x86-64
// backend, mirroring pal/linux_x86_64/file_io.hpp's surface exactly (same FileOpenMode enum, same
// function set) so quark/core/file_store.hpp and reminder_service.hpp are unchanged above this seam.
//
// PATHS ARE NARROW (`CreateFileA`), DELIBERATELY: every caller in this codebase builds paths from
// ASCII actor-id hex or a caller-supplied root string (file_store.hpp's `path_for`, .qwal/.qrem
// names) — no Unicode path support is needed today. Wide-path (`CreateFileW`) support is a documented
// future extension, not a gap in the paths this seam is actually asked to open.
//
// POSITIONED I/O (pread/pwrite): implemented via ReadFile/WriteFile with an OVERLAPPED carrying an
// explicit offset on an otherwise-synchronous handle (no FILE_FLAG_OVERLAPPED) — this performs the
// I/O at that offset without disturbing the handle's current file pointer, the same "does not depend
// on or move the cursor" contract POSIX pread/pwrite give file_store.hpp's random-access WAL writes.
//
// APPEND MODE: opened with FILE_APPEND_DATA (not GENERIC_WRITE) instead of GENERIC_WRITE, which is
// the Win32 analogue of O_APPEND — every WriteFile lands atomically at EOF regardless of position,
// matching reminder_service.hpp's single-writer append assumption.
#pragma once

#if !defined(_WIN32)
#error "pal/windows_x86_64/file_io.hpp is the Windows backend of the 019 PAL file-I/O seam; \
other OSes need their own backend (see pal/linux_x86_64/file_io.hpp). Do not include it elsewhere."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace quark::pal {

using file_t = HANDLE;
inline const file_t invalid_file = INVALID_HANDLE_VALUE;

enum class FileOpenMode : std::uint8_t {
    kReadWriteCreate,   // OPEN_ALWAYS,   GENERIC_READ|GENERIC_WRITE — keep existing contents
    kWriteCreateTrunc,  // CREATE_ALWAYS, GENERIC_WRITE               — fresh file (compact tmp)
    kWriteExisting,     // OPEN_EXISTING, GENERIC_WRITE               — truncate-repair reopen
    kWriteCreateAppend, // OPEN_ALWAYS,   FILE_APPEND_DATA            — every write lands at EOF
    kReadOnly,          // OPEN_EXISTING, GENERIC_READ
};

// Broad sharing (read+write+delete) mirrors POSIX's much looser default file-locking semantics —
// Windows' default (exclusive) would spuriously fail opens/renames the POSIX backend never had to
// worry about (e.g. compact()'s rename-over-path while another handle briefly overlaps).
inline constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

[[nodiscard]] inline file_t file_open(const std::string& path, FileOpenMode mode) noexcept {
    DWORD access = 0, disposition = 0;
    switch (mode) {
        case FileOpenMode::kReadWriteCreate:
            access = GENERIC_READ | GENERIC_WRITE; disposition = OPEN_ALWAYS; break;
        case FileOpenMode::kWriteCreateTrunc:
            access = GENERIC_WRITE; disposition = CREATE_ALWAYS; break;
        case FileOpenMode::kWriteExisting:
            access = GENERIC_WRITE; disposition = OPEN_EXISTING; break;
        case FileOpenMode::kWriteCreateAppend:
            access = FILE_APPEND_DATA; disposition = OPEN_ALWAYS; break;
        case FileOpenMode::kReadOnly:
            access = GENERIC_READ; disposition = OPEN_EXISTING; break;
    }
    return ::CreateFileA(path.c_str(), access, kShareAll, nullptr, disposition,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
}

[[nodiscard]] inline std::int64_t file_pwrite(file_t f, const void* buf, std::size_t n,
                                              std::uint64_t off) noexcept {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFF'FFFFu);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD written = 0;
    if (!::WriteFile(f, buf, static_cast<DWORD>(n), &written, &ov)) return -1;
    return static_cast<std::int64_t>(written);
}
[[nodiscard]] inline std::int64_t file_pread(file_t f, void* buf, std::size_t n,
                                             std::uint64_t off) noexcept {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFF'FFFFu);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD read = 0;
    if (!::ReadFile(f, buf, static_cast<DWORD>(n), &read, &ov)) {
        return ::GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return static_cast<std::int64_t>(read);
}

// Sequential I/O at the handle's current position (append-mode handles ignore it — see banner).
[[nodiscard]] inline std::int64_t file_write(file_t f, const void* buf, std::size_t n) noexcept {
    DWORD written = 0;
    if (!::WriteFile(f, buf, static_cast<DWORD>(n), &written, nullptr)) return -1;
    return static_cast<std::int64_t>(written);
}
[[nodiscard]] inline std::int64_t file_read(file_t f, void* buf, std::size_t n) noexcept {
    DWORD read = 0;
    if (!::ReadFile(f, buf, static_cast<DWORD>(n), &read, nullptr)) {
        return ::GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return static_cast<std::int64_t>(read);
}

// Unlike ftruncate, SetEndOfFile truncates at the CURRENT file pointer — seek first. Every caller of
// this (file_store.hpp replay repair, reminder_service.hpp torn-tail repair) either never issues
// further sequential I/O on that handle or only issues positioned I/O afterward, so the pointer move
// this leaves behind is harmless.
[[nodiscard]] inline bool file_truncate(file_t f, std::uint64_t new_size) noexcept {
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(new_size);
    if (!::SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    return ::SetEndOfFile(f) != 0;
}

[[nodiscard]] inline std::optional<std::uint64_t> file_size(file_t f) noexcept {
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(f, &sz)) return std::nullopt;
    return static_cast<std::uint64_t>(sz.QuadPart);
}

// The durable-flush barrier: FlushFileBuffers (019 §5 — "the sharp one"), the Windows analogue of
// fdatasync/fsync — the only Windows call that reaches the platter, not just the OS cache.
[[nodiscard]] inline bool durable_flush(file_t f) noexcept { return ::FlushFileBuffers(f) != 0; }

inline void file_close(file_t f) noexcept {
    if (f != invalid_file) ::CloseHandle(f);
}

[[nodiscard]] inline bool file_rename(const std::string& from, const std::string& to) noexcept {
    return ::MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Windows has no portable "fsync a directory" (a directory handle cannot be flushed with
// FlushFileBuffers without backup semantics + write access); NTFS journals directory metadata, and the
// rename below uses MOVEFILE_WRITE_THROUGH instead. So this is a successful no-op (review B9) — kept
// so portable callers can request a directory barrier unconditionally.
[[nodiscard]] inline bool durable_sync_dir(const std::string& dir) noexcept {
    (void)dir;
    return true;
}

// The directory that contains `path` (either separator; "." for a bare filename).
[[nodiscard]] inline std::string parent_dir(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

// MoveFileEx with MOVEFILE_WRITE_THROUGH: does not return until the move is flushed to disk.
//
// FIRST choice is a POSIX-semantics rename (FileRenameInfoEx, Win10 1809+/NTFS): like rename(2) it
// replaces `to` even while another handle (compact()'s live append fd) has it open. Plain MoveFileEx
// REPLACE_EXISTING fails with ERROR_ACCESS_DENIED in that case regardless of FILE_SHARE_DELETE, which
// is exactly the compact() shape (review B9). Falls back to MoveFileEx where that is unsupported.
[[nodiscard]] inline bool durable_rename(const std::string& from, const std::string& to) noexcept {
    wchar_t wto[MAX_PATH * 2];
    const int n = ::MultiByteToWideChar(CP_ACP, 0, to.c_str(), -1, wto, MAX_PATH * 2);
    wchar_t full[MAX_PATH * 2];
    const DWORD fl = n > 0 ? ::GetFullPathNameW(wto, MAX_PATH * 2, full, nullptr) : 0;
    if (fl > 0 && fl < MAX_PATH * 2) {
        const HANDLE h = ::CreateFileA(from.c_str(), DELETE | GENERIC_WRITE | SYNCHRONIZE, kShareAll,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            alignas(FILE_RENAME_INFO) unsigned char buf[sizeof(FILE_RENAME_INFO) + MAX_PATH * 2 * sizeof(wchar_t)]{};
            auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf);
            ri->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            ri->RootDirectory = nullptr;
            ri->FileNameLength = static_cast<DWORD>(fl * sizeof(wchar_t));
            for (DWORD i = 0; i < fl; ++i) ri->FileName[i] = full[i];
            const bool ok = ::SetFileInformationByHandle(h, FileRenameInfoEx, ri,
                                                         static_cast<DWORD>(sizeof(buf))) != 0;
            if (ok) (void)::FlushFileBuffers(h);  // the moved file's data+metadata are on disk
            ::CloseHandle(h);
            if (ok) return true;
        }
    }
    return ::MoveFileExA(from.c_str(), to.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// Idempotent: a pre-existing directory is not an error (mirrors ::mkdir's EEXIST-ignoring call
// sites — a missing/unwritable parent surfaces at file_open() instead).
inline void make_dir(const std::string& path) noexcept {
    if (!::CreateDirectoryA(path.c_str(), nullptr)) {
        (void)::GetLastError();  // ERROR_ALREADY_EXISTS is the expected idempotent case; ignore all
    }
}

}  // namespace quark::pal
