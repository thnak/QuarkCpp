// Implements 019-Platform-Abstraction-Layer §5 (durable file I/O & flush) — the OS-selector entry
// point. quark/core/file_store.hpp (012) and quark/core/reminder_service.hpp (027) include ONLY this
// header, never a per-OS one directly and never `<fcntl.h>`/`<unistd.h>`/`<windows.h>` (019 §"The one
// rule"). See pal/linux_x86_64/file_io.hpp / pal/windows_x86_64/file_io.hpp for the concrete surface
// (file_t, FileOpenMode, file_open/file_pread/file_pwrite/file_read/file_write/file_truncate/
// file_size/durable_flush/file_close/file_rename/make_dir) — identical on both backends.
#pragma once

#if defined(_WIN32)
#include "pal/windows_x86_64/file_io.hpp"
#else
#include "pal/linux_x86_64/file_io.hpp"
#endif
