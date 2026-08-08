#include "platform_file.hpp"

// The POSIX half of the durable-file edge. Every call here is the one core/src/
// job_store.cpp and core/src/capability_probe.cpp made inline before this file existed,
// with the same flags, the same mode bits and the same errno handling — the extraction
// moved code, it did not change it. This is the half that is compiled and exercised by
// all twelve test suites on every run.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace pd {
namespace platform_file {
namespace {

void setDetail(std::string* detail) {
  if (detail != nullptr) {
    *detail = std::strerror(errno);
  }
}

}  // namespace

bool createDirectory(const std::string& path, std::string* detail) {
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    setDetail(detail);
    return false;
  }
  return true;
}

NativeFile openTruncate(const std::string& path, std::string* detail) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    setDetail(detail);
    return invalidNativeFile();
  }
  return fd;
}

NativeFile openAppend(const std::string& path, std::string* detail) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    setDetail(detail);
    return invalidNativeFile();
  }
  return fd;
}

int64_t writeSome(NativeFile file, const void* data, size_t size, std::string* detail) {
  const ssize_t written = ::write(file, data, size);
  if (written < 0) {
    setDetail(detail);
    return -1;
  }
  return written;
}

bool writeAll(NativeFile file, const void* data, size_t size, std::string* detail) {
  const char* cursor = static_cast<const char*>(data);
  size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = ::write(file, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      setDetail(detail);
      return false;
    }
    if (written == 0) {
      // Cannot happen for a regular file with a non-empty buffer. Reported rather than
      // retried, because the alternative is a loop that never ends.
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
  if (::fsync(file) != 0) {
    setDetail(detail);
    return false;
  }
  return true;
}

void closeFile(NativeFile file) noexcept {
  if (nativeFileValid(file)) {
    ::close(file);
  }
}

bool replaceFile(const std::string& from, const std::string& to, std::string* detail) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    setDetail(detail);
    return false;
  }
  return true;
}

void removeFile(const std::string& path) noexcept { ::unlink(path.c_str()); }

void syncDirectory(const std::string& path) noexcept {
  const int dir_fd = ::open(path.c_str(), O_RDONLY);
  if (dir_fd < 0) {
    return;
  }
  ::fsync(dir_fd);
  ::close(dir_fd);
}

bool isSeparator(char c) noexcept { return c == '/'; }

}  // namespace platform_file
}  // namespace pd
