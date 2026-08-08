#include "printerdriver/transport.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace pd {
namespace {

constexpr size_t kReadBufferSize = 4096;

bool setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string errnoMessage(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
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

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(config_.port);
  const int rc = ::getaddrinfo(config_.host.c_str(), port_text.c_str(), &hints, &resolved);
  if (rc != 0 || resolved == nullptr) {
    return TransportResult::failure(
        TransportError::ConnectFailed,
        "cannot resolve " + config_.host + ": " + ::gai_strerror(rc));
  }

  int sock = -1;
  TransportResult last =
      TransportResult::failure(TransportError::ConnectFailed, "no address candidates");
  for (addrinfo* candidate = resolved; candidate != nullptr;
       candidate = candidate->ai_next) {
    sock = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (sock < 0) {
      last = TransportResult::failure(TransportError::ConnectFailed, errnoMessage("socket"));
      continue;
    }
    if (!setNonBlocking(sock)) {
      last = TransportResult::failure(TransportError::ConnectFailed, errnoMessage("fcntl"));
      ::close(sock);
      sock = -1;
      continue;
    }
    int result = ::connect(sock, candidate->ai_addr, candidate->ai_addrlen);
    if (result != 0 && errno == EINPROGRESS) {
      pollfd waiter{};
      waiter.fd = sock;
      waiter.events = POLLOUT;
      const int ready = ::poll(&waiter, 1, static_cast<int>(config_.connect_timeout_ms));
      if (ready == 0) {
        last = TransportResult::failure(TransportError::ConnectTimeout,
                                        "connect to " + describe() + " timed out after " +
                                            std::to_string(config_.connect_timeout_ms) +
                                            " ms");
        ::close(sock);
        sock = -1;
        continue;
      }
      if (ready < 0) {
        last = TransportResult::failure(TransportError::ConnectFailed, errnoMessage("poll"));
        ::close(sock);
        sock = -1;
        continue;
      }
      int so_error = 0;
      socklen_t length = sizeof(so_error);
      if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 ||
          so_error != 0) {
        last = TransportResult::failure(
            TransportError::ConnectFailed,
            "connect to " + describe() + " failed: " + std::strerror(so_error));
        ::close(sock);
        sock = -1;
        continue;
      }
      result = 0;
    }
    if (result != 0) {
      last = TransportResult::failure(TransportError::ConnectFailed,
                                      "connect to " + describe() + " failed: " +
                                          std::strerror(errno));
      ::close(sock);
      sock = -1;
      continue;
    }
    break;
  }
  ::freeaddrinfo(resolved);
  if (sock < 0) {
    return last;
  }

  if (config_.tcp_nodelay) {
    // Receipts are small and latency-sensitive: Nagle would hold the completion
    // marker back behind the payload's last partial segment.
    int enabled = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  }

  int wake[2] = {-1, -1};
  if (::pipe(wake) != 0) {
    ::close(sock);
    return TransportResult::failure(TransportError::ConnectFailed, errnoMessage("pipe"));
  }
  setNonBlocking(wake[0]);
  setNonBlocking(wake[1]);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd_ = sock;
    wake_read_fd_ = wake[0];
    wake_write_fd_ = wake[1];
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
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
  }
  if (fd < 0) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "not connected to " + describe());
  }
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::send(fd, data + offset, size - offset, 0);
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && (errno == EINTR)) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd waiter{};
      waiter.fd = fd;
      waiter.events = POLLOUT;
      const int ready = ::poll(&waiter, 1, static_cast<int>(config_.write_timeout_ms));
      if (ready > 0) {
        continue;
      }
      return TransportResult::failure(
          TransportError::WriteFailed,
          ready == 0 ? "write to " + describe() + " timed out" : errnoMessage("poll"),
          offset);
    }
    return TransportResult::failure(TransportError::WriteFailed, errnoMessage("send"),
                                    offset);
  }
  return TransportResult::success(offset);
}

void TcpTransport::readerLoop() {
  std::vector<uint8_t> buffer(kReadBufferSize);
  for (;;) {
    int fd = -1;
    int wake_fd = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      fd = fd_;
      wake_fd = wake_read_fd_;
    }
    if (fd < 0 || closing_.load()) {
      return;
    }
    pollfd waiters[2]{};
    waiters[0].fd = fd;
    waiters[0].events = POLLIN;
    waiters[1].fd = wake_fd;
    waiters[1].events = POLLIN;
    const int ready = ::poll(waiters, 2, 200);
    if (closing_.load()) {
      return;
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      teardown(true, TransportError::Closed, errnoMessage("poll"));
      return;
    }
    if (ready == 0) {
      continue;
    }
    if ((waiters[1].revents & POLLIN) != 0) {
      return;  // close() woke us.
    }
    if ((waiters[0].revents & (POLLERR | POLLNVAL)) != 0) {
      teardown(true, TransportError::Closed, "socket error on " + describe());
      return;
    }
    if ((waiters[0].revents & (POLLIN | POLLHUP)) == 0) {
      continue;
    }
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (got > 0) {
      if (on_bytes_) {
        on_bytes_(buffer.data(), static_cast<size_t>(got));
      }
      continue;
    }
    if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    teardown(true, TransportError::Closed,
             got == 0 ? "peer closed " + describe() : errnoMessage("recv"));
    return;
  }
}

void TcpTransport::teardown(bool notify, TransportError error,
                            const std::string& message) {
  connected_.store(false);
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
    fd_ = -1;
  }
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  if (notify && on_disconnected_ && !notified_.exchange(true)) {
    on_disconnected_(error, message);
  }
}

void TcpTransport::close() {
  const bool was_connected = connected_.exchange(false);
  closing_.store(true);
  int wake_fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_fd = wake_write_fd_;
  }
  if (wake_fd >= 0) {
    const uint8_t token = 1;
    ssize_t ignored = ::write(wake_fd, &token, 1);
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
  if (wake_read_fd_ >= 0) {
    ::close(wake_read_fd_);
    wake_read_fd_ = -1;
  }
  if (wake_write_fd_ >= 0) {
    ::close(wake_write_fd_);
    wake_write_fd_ = -1;
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
