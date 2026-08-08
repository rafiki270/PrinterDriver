#ifdef _WIN32
#include "printerdriver/transport.hpp"

// Winsock2 implementation of TcpTransport (docs/platforms.md, "Using the SDK from a
// Windows app", step 1). CMake compiles this file INSTEAD OF core/src/transport.cpp
// when WIN32 — the two are alternatives, never both, so the POSIX implementation is
// untouched by the existence of this one and stays byte-for-byte what it always was.
//
// -- Verification status -------------------------------------------------------------
//
// SYNTAX- AND TYPE-CHECKED ONLY. This file has never been compiled by MSVC or clang-cl,
// never linked against ws2_32.lib, and never run. What has been done to it is
//
//   clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
//     -DPD_FORCE_WINDOWS_PLATFORM -DPD_WINDOWS_SYNTAX_CHECK \
//     -include core/tests/win32_stub.h -I core/include core/src/transport_win.cpp
//
// on a macOS host — see scripts/check_windows_syntax.sh, which also runs a negative
// control proving that check can actually fail. That establishes that every Winsock
// call here has the right name, arity and argument types against a faithful stub of
// <winsock2.h>. It establishes nothing about runtime behaviour. The first real evidence
// will come from .github/workflows/windows.yml, which is manual-dispatch only and has
// not been run.
//
// -- What changes, and what deliberately does not ------------------------------------
//
// The structure, the state machine, the callback contract, the timeout handling and
// every TransportResult message shape are the POSIX file's, transposed one call at a
// time. The three genuine platform differences are:
//
//   1. WSAStartup must run before any socket call (pd::net::startup, done lazily on
//      first socket creation).
//   2. SOCKET is UINT_PTR and its invalid value is ~0, not a negative number, so the
//      handles are uintptr_t and every test is net::valid() rather than `< 0`.
//   3. The self-pipe that wakes the reader out of poll() cannot be a pipe: Windows
//      select() waits on sockets only. It is a connected loopback TCP pair instead —
//      the standard socketpair() emulation, with the accepted peer verified against the
//      client's own local address so that another process racing onto the ephemeral
//      listening port cannot become half of this transport's wake channel.

#include "printerdriver/net_platform.hpp"

#include <cstring>
#include <utility>

namespace pd {
namespace {

constexpr size_t kReadBufferSize = 4096;

std::string socketMessage(const char* what) {
  return std::string(what) + ": " + net::errorText(net::lastError());
}

// socketpair() does not exist on Winsock. This is the conventional stand-in: bind a
// listener to an ephemeral loopback port, connect to it, accept, then throw the
// listener away. The peer check is not paranoia theatre — the listening port is
// reachable by every process on the machine for the microseconds it exists, and
// accepting a stranger's connection here would hand a local process the ability to
// wake (and, worse, to feed bytes to) this transport's reader loop.
bool makeWakePair(net::Socket* read_end, net::Socket* write_end) {
  const net::Socket listener = net::create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!net::valid(listener)) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = net::loopbackAddress();
  address.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
             static_cast<net::SockLen>(sizeof(address))) != 0 ||
      ::listen(listener, 1) != 0) {
    net::closeSocket(listener);
    return false;
  }
  net::SockLen address_length = static_cast<net::SockLen>(sizeof(address));
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) !=
      0) {
    net::closeSocket(listener);
    return false;
  }

  const net::Socket client = net::create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!net::valid(client)) {
    net::closeSocket(listener);
    return false;
  }
  if (::connect(client, reinterpret_cast<sockaddr*>(&address),
                static_cast<net::SockLen>(sizeof(address))) != 0) {
    net::closeSocket(client);
    net::closeSocket(listener);
    return false;
  }

  sockaddr_in peer{};
  net::SockLen peer_length = static_cast<net::SockLen>(sizeof(peer));
  const net::Socket server = static_cast<net::Socket>(
      ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length));
  net::closeSocket(listener);
  if (!net::valid(server)) {
    net::closeSocket(client);
    return false;
  }

  sockaddr_in local{};
  net::SockLen local_length = static_cast<net::SockLen>(sizeof(local));
  if (::getsockname(client, reinterpret_cast<sockaddr*>(&local), &local_length) != 0 ||
      local.sin_port != peer.sin_port || local.sin_addr.s_addr != peer.sin_addr.s_addr) {
    net::closeSocket(server);
    net::closeSocket(client);
    return false;
  }

  net::setNonBlocking(server);
  net::setNonBlocking(client);
  *read_end = server;
  *write_end = client;
  return true;
}

}  // namespace

const char* to_string(TransportError error) noexcept {
  switch (error) {
    case TransportError::None: return "None";
    case TransportError::ConnectFailed: return "ConnectFailed";
    case TransportError::ConnectTimeout: return "ConnectTimeout";
    case TransportError::WriteFailed: return "WriteFailed";
    case TransportError::NotConnected: return "NotConnected";
    case TransportError::Closed: return "Closed";
  }
  return "None";
}

TcpTransport::TcpTransport(TcpConfig config) : config_(std::move(config)) {}

TcpTransport::~TcpTransport() { close(); }

void TcpTransport::onBytes(BytesCallback callback) { on_bytes_ = std::move(callback); }

void TcpTransport::onDisconnected(DisconnectCallback callback) {
  on_disconnected_ = std::move(callback);
}

std::string TcpTransport::describe() const {
  return "tcp://" + config_.host + ":" + std::to_string(config_.port);
}

bool TcpTransport::isConnected() const { return connected_.load(); }

TransportResult TcpTransport::connect() {
  if (connected_.load()) {
    return TransportResult::success();
  }
  closing_.store(false);
  notified_.store(false);

  if (!net::startup()) {
    return TransportResult::failure(TransportError::ConnectFailed,
                                    "WSAStartup failed: " +
                                        net::errorText(net::lastError()));
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(config_.port);
  // On Winsock getaddrinfo's return code IS a WSA error code, so errorText() renders it
  // the same way every other failure in this file is rendered; there is no
  // gai_strerror to reach for.
  const int rc = ::getaddrinfo(config_.host.c_str(), port_text.c_str(), &hints, &resolved);
  if (rc != 0 || resolved == nullptr) {
    return TransportResult::failure(
        TransportError::ConnectFailed,
        "cannot resolve " + config_.host + ": " + net::errorText(rc));
  }

  net::Socket sock = net::invalidSocket();
  TransportResult last =
      TransportResult::failure(TransportError::ConnectFailed, "no address candidates");
  for (addrinfo* candidate = resolved; candidate != nullptr;
       candidate = candidate->ai_next) {
    sock = net::create(candidate->ai_family, candidate->ai_socktype,
                       candidate->ai_protocol);
    if (!net::valid(sock)) {
      last = TransportResult::failure(TransportError::ConnectFailed,
                                      socketMessage("socket"));
      continue;
    }
    if (!net::setNonBlocking(sock)) {
      last = TransportResult::failure(TransportError::ConnectFailed,
                                      socketMessage("ioctlsocket"));
      net::closeSocket(sock);
      sock = net::invalidSocket();
      continue;
    }
    int result = ::connect(sock, candidate->ai_addr,
                           static_cast<net::SockLen>(candidate->ai_addrlen));
    if (result != 0 && net::inProgress(net::lastError())) {
      net::PollFd waiter;
      waiter.socket = sock;
      waiter.events = net::kPollOut;
      const int ready =
          net::poll(&waiter, 1, static_cast<int>(config_.connect_timeout_ms));
      if (ready == 0) {
        last = TransportResult::failure(TransportError::ConnectTimeout,
                                        "connect to " + describe() + " timed out after " +
                                            std::to_string(config_.connect_timeout_ms) +
                                            " ms");
        net::closeSocket(sock);
        sock = net::invalidSocket();
        continue;
      }
      if (ready < 0) {
        last = TransportResult::failure(TransportError::ConnectFailed,
                                        socketMessage("select"));
        net::closeSocket(sock);
        sock = net::invalidSocket();
        continue;
      }
      // select() reports a failed non-blocking connect through exceptfds, which
      // net::poll surfaces as kPollError; SO_ERROR then says why. This is the reason
      // net::poll is written on select() and not on WSAPoll — see net_platform.hpp.
      int so_error = 0;
      if (!net::pendingError(sock, &so_error) || so_error != 0) {
        last = TransportResult::failure(
            TransportError::ConnectFailed,
            "connect to " + describe() + " failed: " + net::errorText(so_error));
        net::closeSocket(sock);
        sock = net::invalidSocket();
        continue;
      }
      result = 0;
    }
    if (result != 0) {
      last = TransportResult::failure(TransportError::ConnectFailed,
                                      "connect to " + describe() + " failed: " +
                                          net::errorText(net::lastError()));
      net::closeSocket(sock);
      sock = net::invalidSocket();
      continue;
    }
    break;
  }
  ::freeaddrinfo(resolved);
  if (!net::valid(sock)) {
    return last;
  }

  if (config_.tcp_nodelay) {
    // Receipts are small and latency-sensitive: Nagle would hold the completion
    // marker back behind the payload's last partial segment.
    net::setIntOption(sock, IPPROTO_TCP, TCP_NODELAY, 1);
  }

  net::Socket wake_read = net::invalidSocket();
  net::Socket wake_write = net::invalidSocket();
  if (!makeWakePair(&wake_read, &wake_write)) {
    net::closeSocket(sock);
    return TransportResult::failure(TransportError::ConnectFailed,
                                    socketMessage("loopback wake pair"));
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    socket_ = sock;
    wake_read_socket_ = wake_read;
    wake_write_socket_ = wake_write;
  }
  connected_.store(true);
  reader_ = std::thread([this] { readerLoop(); });
  return TransportResult::success();
}

TransportResult TcpTransport::write(const uint8_t* data, size_t size) {
  if (!connected_.load()) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "not connected to " + describe());
  }
  net::Socket sock = net::invalidSocket();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sock = socket_;
  }
  if (!net::valid(sock)) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "not connected to " + describe());
  }
  size_t offset = 0;
  while (offset < size) {
    const int64_t written = net::sendSome(sock, data + offset, size - offset);
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    const int error = net::lastError();
    if (written < 0 && net::interrupted(error)) {
      continue;
    }
    if (written < 0 && net::wouldBlock(error)) {
      net::PollFd waiter;
      waiter.socket = sock;
      waiter.events = net::kPollOut;
      const int ready = net::poll(&waiter, 1, static_cast<int>(config_.write_timeout_ms));
      if (ready > 0) {
        continue;
      }
      return TransportResult::failure(
          TransportError::WriteFailed,
          ready == 0 ? "write to " + describe() + " timed out" : socketMessage("select"),
          offset);
    }
    return TransportResult::failure(TransportError::WriteFailed, socketMessage("send"),
                                    offset);
  }
  return TransportResult::success(offset);
}

void TcpTransport::readerLoop() {
  std::vector<uint8_t> buffer(kReadBufferSize);
  for (;;) {
    net::Socket sock = net::invalidSocket();
    net::Socket wake = net::invalidSocket();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sock = socket_;
      wake = wake_read_socket_;
    }
    if (!net::valid(sock) || closing_.load()) {
      return;
    }
    net::PollFd waiters[2];
    waiters[0].socket = sock;
    waiters[0].events = net::kPollIn;
    waiters[1].socket = wake;
    waiters[1].events = net::kPollIn;
    const int ready = net::poll(waiters, 2, 200);
    if (closing_.load()) {
      return;
    }
    if (ready < 0) {
      if (net::interrupted(net::lastError())) {
        continue;
      }
      teardown(true, TransportError::Closed, socketMessage("select"));
      return;
    }
    if (ready == 0) {
      continue;
    }
    if ((waiters[1].revents & net::kPollIn) != 0) {
      return;  // close() woke us.
    }
    if ((waiters[0].revents & (net::kPollError | net::kPollInvalid)) != 0) {
      teardown(true, TransportError::Closed, "socket error on " + describe());
      return;
    }
    if ((waiters[0].revents & (net::kPollIn | net::kPollHangup)) == 0) {
      continue;
    }
    const int64_t got = net::recvSome(sock, buffer.data(), buffer.size());
    if (got > 0) {
      if (on_bytes_) {
        on_bytes_(buffer.data(), static_cast<size_t>(got));
      }
      continue;
    }
    const int error = net::lastError();
    if (got < 0 && (net::interrupted(error) || net::wouldBlock(error))) {
      continue;
    }
    teardown(true, TransportError::Closed,
             got == 0 ? "peer closed " + describe() : socketMessage("recv"));
    return;
  }
}

void TcpTransport::teardown(bool notify, TransportError error,
                            const std::string& message) {
  connected_.store(false);
  net::Socket sock = net::invalidSocket();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sock = socket_;
    socket_ = net::invalidSocket();
  }
  if (net::valid(sock)) {
    net::shutdownBoth(sock);
    net::closeSocket(sock);
  }
  if (notify && on_disconnected_ && !notified_.exchange(true)) {
    on_disconnected_(error, message);
  }
}

void TcpTransport::close() {
  const bool was_connected = connected_.exchange(false);
  closing_.store(true);
  net::Socket wake = net::invalidSocket();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake = wake_write_socket_;
  }
  if (net::valid(wake)) {
    const uint8_t token = 1;
    const int64_t ignored = net::sendSome(wake, &token, 1);
    (void)ignored;
  }
  if (reader_.joinable()) {
    // Joining from inside the reader's own callback would deadlock; a transport
    // closed from a byte callback detaches instead and lets the loop unwind.
    if (reader_.get_id() == std::this_thread::get_id()) {
      reader_.detach();
    } else {
      reader_.join();
    }
  }
  teardown(false, TransportError::Closed, {});
  std::lock_guard<std::mutex> lock(mutex_);
  if (net::valid(wake_read_socket_)) {
    net::closeSocket(wake_read_socket_);
    wake_read_socket_ = net::invalidSocket();
  }
  if (net::valid(wake_write_socket_)) {
    net::closeSocket(wake_write_socket_);
    wake_write_socket_ = net::invalidSocket();
  }
  (void)was_connected;
}

TransportFactory tcp(std::string host, uint16_t port, uint32_t connect_timeout_ms) {
  TcpConfig config;
  config.host = std::move(host);
  config.port = port;
  config.connect_timeout_ms = connect_timeout_ms;
  return tcp(config);
}

TransportFactory tcp(TcpConfig config) {
  return [config]() -> std::unique_ptr<Transport> {
    return std::unique_ptr<Transport>(new TcpTransport(config));
  };
}

}  // namespace pd

#endif  // _WIN32
