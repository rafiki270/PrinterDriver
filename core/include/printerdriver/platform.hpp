#pragma once

#include <cstdint>

// Which platform edge the core is being compiled for (docs/platforms.md, "Using the SDK
// from a Windows app").
//
// The core is portable C++17 with three platform edges — sockets, durable file writes
// and the temp-directory helper in the test harness. Everything else (encoder, parser,
// state machine, profiles, queue addon) is standard C++ and needs none of this.
//
// -- Why a macro of our own rather than plain `#ifdef _WIN32` ------------------------
//
// PD_FORCE_WINDOWS_PLATFORM lets a *host* compiler parse and type-check the Windows
// edge without pretending to be MSVC. `-D_WIN32` on clang against libc++ would change
// how the standard library itself is configured (<thread>, <mutex>, filesystem paths)
// and produce failures that have nothing to do with this SDK's code. Defining our own
// selector instead switches only our own `#if PD_PLATFORM_WINDOWS` sites, which is
// exactly what scripts/check_windows_syntax.sh needs and nothing more.
//
// A real Windows build (MSVC or clang-cl) defines _WIN32 and never defines
// PD_FORCE_WINDOWS_PLATFORM, so the two spellings cannot disagree there.
#if defined(_WIN32) || defined(PD_FORCE_WINDOWS_PLATFORM)
#define PD_PLATFORM_WINDOWS 1
#else
#define PD_PLATFORM_WINDOWS 0
#endif

namespace pd {

// A file the journal is written through. A POSIX file descriptor, or a Win32 HANDLE
// kept as void* so that no public header of this SDK has to include <windows.h>.
#if PD_PLATFORM_WINDOWS
using NativeFile = void*;
#else
using NativeFile = int;
#endif

// The value that means "no file". Matches INVALID_HANDLE_VALUE ((HANDLE)-1) on Windows,
// which is what CreateFile returns on failure — not nullptr.
inline NativeFile invalidNativeFile() noexcept {
#if PD_PLATFORM_WINDOWS
  return reinterpret_cast<NativeFile>(static_cast<intptr_t>(-1));
#else
  return -1;
#endif
}

inline bool nativeFileValid(NativeFile file) noexcept {
#if PD_PLATFORM_WINDOWS
  return file != invalidNativeFile() && file != nullptr;
#else
  return file >= 0;
#endif
}

}  // namespace pd
