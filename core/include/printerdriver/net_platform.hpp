#pragma once

// The socket edge, in the two spellings the deployed platforms use (docs/platforms.md
// §"Using the SDK from a Windows app", step 1). BSD sockets on POSIX; Winsock2 on
// Windows.
//
// This is an implementation-and-test-support header, not part of the SDK's supported
// surface: an application binds core/include/printerdriver/driver.hpp or the C ABI in
// capi/include/printerdriver/pd.h and never sees a socket. It lives under core/include
// only because core/tests/fake_printer.hpp needs it too, and a test header cannot reach
// into core/src.
//
// -- What is verified, and what is not ----------------------------------------------
//
// The POSIX half is compiled and tested on every run of the suite. The Windows half has
// been *syntax- and type-checked only*, by clang++ -fsyntax-only against the test-only
// stub in core/tests/win32_stub.h — see scripts/check_windows_syntax.sh and
// wrappers/dotnet/README.md. It has never been compiled by MSVC, never linked against
// ws2_32, and never run. Treat every Windows branch below as reviewed source awaiting
// its first real build.
//
// -- Why select() and not WSAPoll() on Windows ---------------------------------------
//
// WSAPoll does not report a failed non-blocking connect() through POLLOUT/POLLERR the
// way ::poll does — a documented, never-fixed defect. Since a failed connect is exactly
// what TcpTransport::connect() must detect, the Windows implementation of poll() below
// is written on select(), whose exceptfds set does report it. Both call sites here wait
// on at most three sockets, so select's FD_SETSIZE limit is irrelevant.

#include "printerdriver/platform.hpp"

#if PD_PLATFORM_WINDOWS
#ifndef PD_WINDOWS_SYNTAX_CHECK
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// core/tests/win32_stub.h is force-included by scripts/check_windows_syntax.sh in place
// of the two headers above; see that stub's header comment. A real Windows build never
// defines PD_WINDOWS_SYNTAX_CHECK, so it always takes the branch above.
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace pd {
namespace net {

#if PD_PLATFORM_WINDOWS
// SOCKET is UINT_PTR: on 64-bit Windows it does not fit in an int, and the invalid
// value is ~0 rather than a negative number. Storing one in an `int` is the classic
// Winsock port bug; this typedef exists so it cannot happen here.
using Socket = std::uintptr_t;
using SockLen = int;
#else
using Socket = int;
using SockLen = socklen_t;
#endif

inline Socket invalidSocket() noexcept {
#if PD_PLATFORM_WINDOWS
  return static_cast<Socket>(~static_cast<std::uintptr_t>(0));  // INVALID_SOCKET
#else
  return -1;
#endif
}

inline bool valid(Socket socket) noexcept {
#if PD_PLATFORM_WINDOWS
  return socket != invalidSocket();
#else
  return socket >= 0;
#endif
}

// Winsock refuses every call until a process has called WSAStartup. Done once, lazily,
// on first use, so no caller has to remember it and no static initializer runs before
// main(). WSACleanup is deliberately never called: the reference is released at process
// exit anyway, and tearing Winsock down while another thread is inside a socket call is
// a far worse bug than one leaked reference. On POSIX this is a no-op returning true.
inline bool startup() noexcept {
#if PD_PLATFORM_WINDOWS
  static const bool ready = [] {
    WSADATA data;
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
#else
  return true;
#endif
}

inline int lastError() noexcept {
#if PD_PLATFORM_WINDOWS
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

inline bool wouldBlock(int error) noexcept {
#if PD_PLATFORM_WINDOWS
  return error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

// A non-blocking connect() that has not finished yet.
inline bool inProgress(int error) noexcept {
#if PD_PLATFORM_WINDOWS
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
  return error == EINPROGRESS;
#endif
}

inline bool interrupted(int error) noexcept {
#if PD_PLATFORM_WINDOWS
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

// Human-readable form of a socket error code, for the message on TransportResult.
inline std::string errorText(int error) {
#if PD_PLATFORM_WINDOWS
  char* buffer = nullptr;
  const unsigned long written = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<unsigned long>(error), 0, reinterpret_cast<char*>(&buffer), 0,
      nullptr);
  if (written == 0 || buffer == nullptr) {
    return "winsock error " + std::to_string(error);
  }
  std::string message(buffer, static_cast<std::size_t>(written));
  ::LocalFree(buffer);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                              message.back() == ' ' || message.back() == '.')) {
    message.pop_back();
  }
  return message;
#else
  return std::strerror(error);
#endif
}

// --- Byte order --------------------------------------------------------------------
//
// Written out rather than delegating to htonl()/ntohs(). Those are function-like macros
// on several hosts, which makes them impossible to stand in for when the Windows edge
// is syntax-checked on a POSIX box (core/tests/win32_stub.h), and the operation is four
// lines. Endian-independent by construction: the byte array IS the network order, so
// this is correct on a big-endian host too, where htonl would be the identity.

inline std::uint32_t toNetwork32(std::uint32_t value) noexcept {
  const unsigned char bytes[4] = {static_cast<unsigned char>((value >> 24) & 0xFFu),
                                  static_cast<unsigned char>((value >> 16) & 0xFFu),
                                  static_cast<unsigned char>((value >> 8) & 0xFFu),
                                  static_cast<unsigned char>(value & 0xFFu)};
  std::uint32_t out = 0;
  std::memcpy(&out, bytes, sizeof(out));
  return out;
}

inline std::uint16_t fromNetwork16(std::uint16_t value) noexcept {
  unsigned char bytes[2];
  std::memcpy(bytes, &value, sizeof(bytes));
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

// 127.0.0.1, in network order, without needing INADDR_LOOPBACK from a platform header.
inline std::uint32_t loopbackAddress() noexcept { return toNetwork32(0x7f000001u); }

inline Socket create(int family, int type, int protocol) noexcept {
  if (!startup()) {
    return invalidSocket();
  }
  return static_cast<Socket>(::socket(family, type, protocol));
}

inline void closeSocket(Socket socket) noexcept {
  if (!valid(socket)) {
    return;
  }
#if PD_PLATFORM_WINDOWS
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

inline void shutdownBoth(Socket socket) noexcept {
  if (!valid(socket)) {
    return;
  }
#if PD_PLATFORM_WINDOWS
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

inline bool setNonBlocking(Socket socket) noexcept {
#if PD_PLATFORM_WINDOWS
  unsigned long enabled = 1;
  return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const int flags = ::fcntl(socket, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// setsockopt's option buffer is `const void*` on POSIX and `const char*` on Winsock;
// every option this SDK sets is a plain int, so one helper covers both.
inline bool setIntOption(Socket socket, int level, int option, int value) noexcept {
#if PD_PLATFORM_WINDOWS
  return ::setsockopt(socket, level, option, reinterpret_cast<const char*>(&value),
                      static_cast<int>(sizeof(value))) == 0;
#else
  return ::setsockopt(socket, level, option, &value, sizeof(value)) == 0;
#endif
}

// SO_ERROR after a non-blocking connect completes. False when the query itself failed.
inline bool pendingError(Socket socket, int* out) noexcept {
  int error = 0;
  SockLen length = static_cast<SockLen>(sizeof(error));
#if PD_PLATFORM_WINDOWS
  if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                   &length) != 0) {
    return false;
  }
#else
  if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
    return false;
  }
#endif
  *out = error;
  return true;
}

// send/recv return ssize_t on POSIX and int on Winsock, and take size_t vs int for the
// length. int64_t covers both without narrowing anywhere.
inline std::int64_t sendSome(Socket socket, const void* data, std::size_t size) noexcept {
#if PD_PLATFORM_WINDOWS
  return ::send(socket, static_cast<const char*>(data), static_cast<int>(size), 0);
#else
  return ::send(socket, data, size, 0);
#endif
}

inline std::int64_t recvSome(Socket socket, void* data, std::size_t size) noexcept {
#if PD_PLATFORM_WINDOWS
  return ::recv(socket, static_cast<char*>(data), static_cast<int>(size), 0);
#else
  return ::recv(socket, data, size, 0);
#endif
}

// --- Readiness ---------------------------------------------------------------------

constexpr short kPollIn = 0x01;
constexpr short kPollOut = 0x02;
constexpr short kPollError = 0x04;
constexpr short kPollHangup = 0x08;
constexpr short kPollInvalid = 0x10;

struct PollFd {
  Socket socket = invalidSocket();
  short events = 0;
  short revents = 0;
};

// Every call site here waits on one, two or three sockets. Keeping the bound explicit
// lets the POSIX path build its pollfd array on the stack and the Windows path skip
// any FD_SETSIZE worry.
constexpr std::size_t kMaxPollFds = 4;

// >0: that many entries have revents set. 0: the timeout expired. <0: the wait itself
// failed, and lastError() says why.
inline int poll(PollFd* fds, std::size_t count, int timeout_ms) noexcept {
  if (fds == nullptr || count == 0 || count > kMaxPollFds) {
    return -1;
  }
#if PD_PLATFORM_WINDOWS
  fd_set readable;
  fd_set writable;
  fd_set failed;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  bool any = false;
  for (std::size_t i = 0; i < count; ++i) {
    fds[i].revents = 0;
    if (!valid(fds[i].socket)) {
      fds[i].revents = kPollInvalid;
      continue;
    }
    if ((fds[i].events & kPollIn) != 0) {
      FD_SET(fds[i].socket, &readable);
    }
    if ((fds[i].events & kPollOut) != 0) {
      FD_SET(fds[i].socket, &writable);
    }
    FD_SET(fds[i].socket, &failed);
    any = true;
  }
  if (!any) {
    // Every socket handed in was already closed: report it as revents rather than
    // blocking forever on three empty sets, which is what select() would do.
    int invalid = 0;
    for (std::size_t i = 0; i < count; ++i) {
      invalid += fds[i].revents != 0 ? 1 : 0;
    }
    return invalid;
  }
  timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  // The first argument is ignored by Winsock (it is the POSIX nfds bound); 0 is the
  // conventional value there.
  const int ready = ::select(0, &readable, &writable, &failed, timeout_ms < 0 ? nullptr
                                                                             : &timeout);
  if (ready < 0) {
    return -1;
  }
  int signalled = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!valid(fds[i].socket)) {
      continue;
    }
    if (FD_ISSET(fds[i].socket, &readable)) {
      fds[i].revents |= kPollIn;
    }
    if (FD_ISSET(fds[i].socket, &writable)) {
      fds[i].revents |= kPollOut;
    }
    if (FD_ISSET(fds[i].socket, &failed)) {
      fds[i].revents |= kPollError;
    }
    signalled += fds[i].revents != 0 ? 1 : 0;
  }
  return signalled;
#else
  pollfd waiters[kMaxPollFds]{};
  for (std::size_t i = 0; i < count; ++i) {
    waiters[i].fd = fds[i].socket;
    waiters[i].events = 0;
    if ((fds[i].events & kPollIn) != 0) {
      waiters[i].events = static_cast<short>(waiters[i].events | POLLIN);
    }
    if ((fds[i].events & kPollOut) != 0) {
      waiters[i].events = static_cast<short>(waiters[i].events | POLLOUT);
    }
  }
  const int ready = ::poll(waiters, static_cast<nfds_t>(count), timeout_ms);
  for (std::size_t i = 0; i < count; ++i) {
    short revents = 0;
    if ((waiters[i].revents & POLLIN) != 0) {
      revents = static_cast<short>(revents | kPollIn);
    }
    if ((waiters[i].revents & POLLOUT) != 0) {
      revents = static_cast<short>(revents | kPollOut);
    }
    if ((waiters[i].revents & POLLERR) != 0) {
      revents = static_cast<short>(revents | kPollError);
    }
    if ((waiters[i].revents & POLLHUP) != 0) {
      revents = static_cast<short>(revents | kPollHangup);
    }
    if ((waiters[i].revents & POLLNVAL) != 0) {
      revents = static_cast<short>(revents | kPollInvalid);
    }
    fds[i].revents = revents;
  }
  return ready;
#endif
}

}  // namespace net
}  // namespace pd
