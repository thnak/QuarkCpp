// Implements 019-Platform-Abstraction-Layer §5 (durable file I/O & flush) — the POSIX backend, used
// by Linux (and any other POSIX target that opts into this header). Factored out of file_store.hpp /
// reminder_service.hpp verbatim: same calls (`open`/`pwrite`/`pread`/`read`/`write`/`ftruncate`/
// `close`/`fdatasync`/`rename`/`mkdir`), same semantics, no behavior change on Linux — only the call
// site moved so quark/core/*.hpp stop touching `<fcntl.h>`/`<unistd.h>` directly (019 §"The one rule").
#pragma once

#if !defined(__linux__) && !defined(__unix__) && !defined(__APPLE__)
#error "pal/linux_x86_64/file_io.hpp is the POSIX backend of the 019 PAL file-I/O seam; \
other OSes need their own backend (see pal/windows_x86_64/file_io.hpp). Do not include it elsewhere."
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>

namespace quark::pal {

using file_t = int;
inline constexpr file_t invalid_file = -1;

enum class FileOpenMode : std::uint8_t {
    kReadWriteCreate,   // O_RDWR|O_CREAT            — open-or-create, keep existing contents
    kWriteCreateTrunc,  // O_WRONLY|O_CREAT|O_TRUNC   — fresh file (compact tmp)
    kWriteExisting,     // O_WRONLY                   — must already exist (truncate-repair reopen)
    kWriteCreateAppend, // O_WRONLY|O_CREAT|O_APPEND  — every write lands at EOF
    kReadOnly,          // O_RDONLY
};

[[nodiscard]] inline file_t file_open(const std::string& path, FileOpenMode mode) noexcept {
    switch (mode) {
        case FileOpenMode::kReadWriteCreate:  return ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        case FileOpenMode::kWriteCreateTrunc: return ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        case FileOpenMode::kWriteExisting:    return ::open(path.c_str(), O_WRONLY, 0644);
        case FileOpenMode::kWriteCreateAppend:
            return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        case FileOpenMode::kReadOnly:          return ::open(path.c_str(), O_RDONLY, 0644);
    }
    return invalid_file;
}

// Positioned I/O — does not disturb (or depend on) the file's current read/write offset.
[[nodiscard]] inline std::int64_t file_pwrite(file_t f, const void* buf, std::size_t n,
                                              std::uint64_t off) noexcept {
    return static_cast<std::int64_t>(::pwrite(f, buf, n, static_cast<off_t>(off)));
}
[[nodiscard]] inline std::int64_t file_pread(file_t f, void* buf, std::size_t n,
                                             std::uint64_t off) noexcept {
    return static_cast<std::int64_t>(::pread(f, buf, n, static_cast<off_t>(off)));
}

// Sequential I/O at the file's current offset (the append-mode fd relies on O_APPEND for atomicity).
[[nodiscard]] inline std::int64_t file_write(file_t f, const void* buf, std::size_t n) noexcept {
    return static_cast<std::int64_t>(::write(f, buf, n));
}
[[nodiscard]] inline std::int64_t file_read(file_t f, void* buf, std::size_t n) noexcept {
    return static_cast<std::int64_t>(::read(f, buf, n));
}

[[nodiscard]] inline bool file_truncate(file_t f, std::uint64_t new_size) noexcept {
    return ::ftruncate(f, static_cast<off_t>(new_size)) == 0;
}

[[nodiscard]] inline std::optional<std::uint64_t> file_size(file_t f) noexcept {
    struct ::stat st{};
    if (::fstat(f, &st) != 0) return std::nullopt;
    return static_cast<std::uint64_t>(st.st_size);
}

// The durable-flush barrier: fdatasync on Linux (skips inode-metadata-only flushes), fsync elsewhere.
[[nodiscard]] inline bool durable_flush(file_t f) noexcept {
#if defined(__linux__)
    return ::fdatasync(f) == 0;
#else
    return ::fsync(f) == 0;
#endif
}

inline void file_close(file_t f) noexcept {
    if (f >= 0) ::close(f);
}

[[nodiscard]] inline bool file_rename(const std::string& from, const std::string& to) noexcept {
    return ::rename(from.c_str(), to.c_str()) == 0;
}

// Idempotent: a pre-existing directory is not an error (mirrors the original `::mkdir` call sites,
// which discarded the return value entirely — a missing/unwritable dir surfaces at file_open()).
inline void make_dir(const std::string& path) noexcept {
    ::mkdir(path.c_str(), 0755);
}

}  // namespace quark::pal
