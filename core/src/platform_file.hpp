#pragma once

// The durable-file edge (docs/platforms.md, "Using the SDK from a Windows app", step 2).
//
// core/src/job_store.cpp's entire reason to exist is one ordering rule: a state
// transition is on disk before the action it authorizes happens. That rule is made of
// four primitives — create a directory, append and flush, write a temp file, replace the
// old one atomically — and those four are the only things about it that differ between
// POSIX and Win32:
//
//   POSIX                       Win32
//   mkdir(0700)                 CreateDirectoryA
//   open(O_APPEND, 0600)        CreateFileA(FILE_APPEND_DATA, OPEN_ALWAYS)
//   write                       WriteFile
//   fsync                       FlushFileBuffers
//   rename                      ReplaceFileA (rename fails if the target exists)
//   fsync(directory fd)         — not expressible; see platform_file_win.cpp
//
// Everything else in job_store.cpp — the journal format, escaping, parsing, recovery
// classification — is standard C++ and is compiled unchanged on both. Exactly one of
// platform_file_posix.cpp / platform_file_win.cpp is compiled into the library.
//
// Every fallible call takes an optional `detail` out-parameter which receives the
// platform's own error text (strerror / FormatMessage) on failure, so that the caller
// can compose the message shapes docs/api.md expects without knowing which platform it
// is on. Passing nullptr is allowed.

#include <cstddef>
#include <cstdint>
#include <string>

#include "printerdriver/platform.hpp"

namespace pd {
namespace platform_file {

// True when the directory exists afterwards — including when it already did, which is
// not an error for a storage directory that survives restarts.
bool createDirectory(const std::string& path, std::string* detail);

// Write-only, create-or-truncate, owner-only permissions where the platform has them.
NativeFile openTruncate(const std::string& path, std::string* detail);

// Write-only, create-if-missing, every write appended atomically at end of file.
NativeFile openAppend(const std::string& path, std::string* detail);

// One write attempt: the number of bytes accepted, or -1 with *detail set.
int64_t writeSome(NativeFile file, const void* data, size_t size, std::string* detail);

// Writes the whole buffer, retrying a call interrupted by a signal. A zero-byte return
// for a non-empty buffer is reported as a failure rather than retried forever.
bool writeAll(NativeFile file, const void* data, size_t size, std::string* detail);

// Forces this file's data out to the device. This is the call the durability rule
// rests on; a caller that skips it (StorageConfig::fsync_enabled == false) is opting
// out of the guarantee, not speeding it up.
bool sync(NativeFile file, std::string* detail);

void closeFile(NativeFile file) noexcept;

// Makes `to` be what `from` contains, atomically, replacing whatever `to` was. A crash
// during this leaves either the old file or the new one, never a blend.
bool replaceFile(const std::string& from, const std::string& to, std::string* detail);

void removeFile(const std::string& path) noexcept;

// Best-effort: makes a directory entry created by replaceFile durable. A no-op where
// the platform has no way to express it.
void syncDirectory(const std::string& path) noexcept;

// True when `c` separates path components on this platform.
bool isSeparator(char c) noexcept;

}  // namespace platform_file
}  // namespace pd
