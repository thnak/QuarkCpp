// Implements 019-Platform-Abstraction-Layer (Windows/ARM64 backend) — ADR-059.
// The Windows PAL code (pal/windows_x86_64/file_io.hpp) calls only Win32/Winsock APIs and contains no
// x86 intrinsics or asm, so it is arch-generic; the directory is named windows_x86_64 for history
// (same situation as pal/linux_x86_64, which also serves arm64). This header is the Windows/ARM64
// arch-selector entry: it exists so the backend is explicit and so ARM64-specific divergence has a
// home. Status: cross-compiled only (see ADR-059); NOT yet run on ARM64 hardware.
#pragma once
#include "pal/windows_x86_64/file_io.hpp"
