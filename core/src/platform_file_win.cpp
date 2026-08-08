#include "platform_file.hpp"

// The Win32 half of the durable-file edge (docs/platforms.md, step 2). CMake compiles
// this file INSTEAD OF platform_file_posix.cpp when WIN32; the two are alternatives,
// never both.
//
// -- Verification status -------------------------------------------------------------
//
// SYNTAX- AND TYPE-CHECKED ONLY, by clang++ -fsyntax-only against the test-only stub in
// core/tests/win32_stub.h (scripts/check_windows_syntax.sh, which also runs a negative
// control proving that check can fail). Never compiled by MSVC, never linked, never
// run. The first behavioural evidence will come from .github/workflows/windows.yml,
// which is manual-dispatch only and has not been run.
//
// -- Three places where Win32 is not just POSIX with different spelling ---------------
//
// 1. APPEND. CreateFileA with FILE_APPEND_DATA *and without* FILE_WRITE_DATA is the
//    real equivalent of O_APPEND: every WriteFile then lands at end-of-file atomically
//    with respect to other writers. Asking for GENERIC_WRITE and seeking instead would
//    reintroduce exactly the interleaving the journal's append-only design rules out.
//
// 2. REPLACE. rename() semantics do not exist here — MoveFile fails when the target
//    exists. ReplaceFileA is the call that both replaces atomically and preserves the
//    original file's attributes/ACLs, which matters for a journal that outlives many
//    process lifetimes. It fails with ERROR_FILE_NOT_FOUND on the very first compaction
//    (there is no journal to replace yet), so that one case falls back to MoveFileExA
//    with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
//
// 3. DIRECTORY DURABILITY. There is no FlushFileBuffers for a directory: Win32 gives no
//    handle to a directory's metadata stream that can be flushed. syncDirectory is
//    therefore a documented no-op here. The ordering the journal needs is still
//    obtained, from MOVEFILE_WRITE_THROUGH / ReplaceFile's own write-through behaviour
//    rather than from a separate directory flush. Anyone porting this further should
//    read that as "the guarantee is narrower on Windows and this is where it is
//    narrower", not as a TODO that was forgotten.

#ifndef PD_WINDOWS_SYNTAX_CHECK
#include <windows.h>
#else
// core/tests/win32_stub.h is force-included by scripts/check_windows_syntax.sh in place
// of <windows.h>; a real Windows build never defines PD_WINDOWS_SYNTAX_CHECK and always
// takes the branch above.
#endif

namespace pd {
namespace platform_file {
namespace {

// WriteFile takes a DWORD count. Journal writes are kilobytes, but clamping rather than
// truncating is the difference between a loop that terminates and one that corrupts.
constexpr size_t kMaxSingleWrite = 1u << 30;

std::string describe(unsigned long code) {
  char* buffer = nullptr;
  const unsigned long written = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  if (written == 0 || buffer == nullptr) {
    return "windows error " + std::to_string(code);
  }
  std::string message(buffer, static_cast<size_t>(written));
  ::LocalFree(buffer);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                              message.back() == ' ' || message.back() == '.')) {
    message.pop_back();
  }
  return message;
}

void setDetail(std::string* detail) {
  if (detail != nullptr) {
    *detail = describe(::GetLastError());
  }
}

}  // namespace

bool createDirectory(const std::string& path, std::string* detail) {
  if (::CreateDirectoryA(path.c_str(), nullptr) != 0) {
    return true;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    return true;
  }
  setDetail(detail);
  return false;
}

NativeFile openTruncate(const std::string& path, std::string* detail) {
  const HANDLE handle =
      ::CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    setDetail(detail);
    return invalidNativeFile();
  }
  return handle;
}

NativeFile openAppend(const std::string& path, std::string* detail) {
  const HANDLE handle =
      ::CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    setDetail(detail);
    return invalidNativeFile();
  }
  return handle;
}

int64_t writeSome(NativeFile file, const void* data, size_t size, std::string* detail) {
  const DWORD chunk = static_cast<DWORD>(size < kMaxSingleWrite ? size : kMaxSingleWrite);
  DWORD written = 0;
  if (::WriteFile(file, data, chunk, &written, nullptr) == 0) {
    setDetail(detail);
    return -1;
  }
  return static_cast<int64_t>(written);
}

bool writeAll(NativeFile file, const void* data, size_t size, std::string* detail) {
  const char* cursor = static_cast<const char*>(data);
  size_t remaining = size;
  while (remaining > 0) {
    const int64_t written = writeSome(file, cursor, remaining, detail);
    if (written < 0) {
      return false;
    }
    if (written == 0) {
      if (detail != nullptr) {
        *detail = "short write";
      }
      return false;
    }
    cursor += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

bool sync(NativeFile file, std::string* detail) {
  if (::FlushFileBuffers(file) == 0) {
    setDetail(detail);
    return false;
  }
  return true;
}

void closeFile(NativeFile file) noexcept {
  if (nativeFileValid(file)) {
    ::CloseHandle(file);
  }
}

bool replaceFile(const std::string& from, const std::string& to, std::string* detail) {
  if (::ReplaceFileA(to.c_str(), from.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                     nullptr, nullptr) != 0) {
    return true;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND) {
    // Nothing to replace: this is the first journal this directory has ever held.
    if (::MoveFileExA(from.c_str(), to.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return true;
    }
    setDetail(detail);
    return false;
  }
  if (detail != nullptr) {
    *detail = describe(error);
  }
  return false;
}

void removeFile(const std::string& path) noexcept { ::DeleteFileA(path.c_str()); }

void syncDirectory(const std::string& path) noexcept {
  // See this file's header comment: Win32 exposes no flushable handle to a directory.
  (void)path;
}

bool isSeparator(char c) noexcept { return c == '/' || c == '\\'; }

}  // namespace platform_file
}  // namespace pd
