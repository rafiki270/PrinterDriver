#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "printerdriver/platform.hpp"

// Transports (docs/sdk-spec.md §7). The core owns each printer connection
// exclusively and keeps it open after writing, because closing the socket after
// sendall() is what stops status bytes from ever coming back
// (docs/techspec.md §4).

namespace pd {

enum class TransportError {
  None,
  ConnectFailed,
  ConnectTimeout,
  WriteFailed,
  NotConnected,
  Closed,
};

const char* to_string(TransportError) noexcept;

struct TransportResult {
  bool ok = true;
  TransportError error = TransportError::None;
  std::string message;
  // Bytes actually handed to the link before the call failed. The engine needs this
  // and not just ok/failed: zero bytes out is a known failure, one byte out is
  // Unknown (docs/api.md §4).
  size_t bytes_written = 0;

  static TransportResult success(size_t bytes = 0) {
    return TransportResult{true, TransportError::None, {}, bytes};
  }
  static TransportResult failure(TransportError error, std::string message,
                                 size_t bytes = 0) {
    return TransportResult{false, error, std::move(message), bytes};
  }
};

class Transport {
 public:
  // Invoked on the transport's reader thread for every chunk received. Must not
  // block and must not call back into the transport.
  using BytesCallback = std::function<void(const uint8_t*, size_t)>;
  // Invoked at most once per successful connect(), on the reader thread, when the
  // link drops for any reason other than an explicit close().
  using DisconnectCallback = std::function<void(TransportError, const std::string&)>;

  virtual ~Transport() = default;

  // Both must be set before connect(); changing them afterwards races the reader.
  virtual void onBytes(BytesCallback callback) = 0;
  virtual void onDisconnected(DisconnectCallback callback) = 0;

  virtual TransportResult connect() = 0;
  virtual TransportResult write(const uint8_t* data, size_t size) = 0;
  TransportResult write(const std::vector<uint8_t>& data) {
    return write(data.data(), data.size());
  }
  virtual void close() = 0;
  virtual bool isConnected() const = 0;
  virtual std::string describe() const = 0;
};

// A printer's transport is created lazily and re-created after a link drop, so
// configuration is a factory rather than an instance.
using TransportFactory = std::function<std::unique_ptr<Transport>()>;

struct TcpConfig {
  std::string host;
  uint16_t port = 9100;
  uint32_t connect_timeout_ms = 3000;
  uint32_t write_timeout_ms = 5000;
  bool tcp_nodelay = true;
};

class TcpTransport : public Transport {
 public:
  explicit TcpTransport(TcpConfig config);
  ~TcpTransport() override;

  using Transport::write;  // the vector overload, hidden by the override below

  void onBytes(BytesCallback callback) override;
  void onDisconnected(DisconnectCallback callback) override;

  TransportResult connect() override;
  TransportResult write(const uint8_t* data, size_t size) override;
  void close() override;
  bool isConnected() const override;
  std::string describe() const override;

  const TcpConfig& config() const noexcept { return config_; }

 private:
  void readerLoop();
  void teardown(bool notify, TransportError error, const std::string& message);

  TcpConfig config_;
  BytesCallback on_bytes_;
  DisconnectCallback on_disconnected_;

  mutable std::mutex mutex_;
  // Two spellings of the same three handles, because the platforms disagree about what
  // a socket is and what "no socket" looks like. POSIX: file descriptors, invalid is
  // negative, and the reader is woken through a self-pipe. Windows: Winsock SOCKETs
  // (UINT_PTR — they do not fit in an int), invalid is ~0, and the wake mechanism is a
  // connected loopback TCP pair, because select() there can only wait on sockets.
  //
  // Kept as uintptr_t rather than SOCKET so this public header stays free of
  // <winsock2.h>; core/src/transport_win.cpp is the only place they are touched, and
  // core/src/transport.cpp — the POSIX implementation — is unchanged by any of this.
#if PD_PLATFORM_WINDOWS
  std::uintptr_t socket_ = ~static_cast<std::uintptr_t>(0);
  std::uintptr_t wake_read_socket_ = ~static_cast<std::uintptr_t>(0);
  std::uintptr_t wake_write_socket_ = ~static_cast<std::uintptr_t>(0);
#else
  int fd_ = -1;
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;
#endif
  std::thread reader_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<bool> notified_{false};
};

TransportFactory tcp(std::string host, uint16_t port = 9100,
                     uint32_t connect_timeout_ms = 3000);
TransportFactory tcp(TcpConfig config);

}  // namespace pd
