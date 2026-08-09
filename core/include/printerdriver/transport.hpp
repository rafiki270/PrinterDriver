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

// --- M13b: RS-232 (docs/compatibility-brief.md §1, §13-§15) --------------------------
//
// Serial is not a legacy footnote in this fleet. Bixolon's SRP-350V documents USB,
// Ethernet, serial, dual serial and parallel; the Citizen CMP portables are RS-232 plus
// USB; Rongta and Xprinter desktops ship serial as standard. More to the point, serial is
// the one interface where the ETB fence of docs/wire-protocols.md §2 is unconditionally
// safe — there is no shared port and no broadcast, so the exclusivity that TCP 9100
// cannot promise is a property of the cable.
//
// The parameters below are not decoration. A printer configured for 7E1 at 19 200 that is
// driven at 8N1 at 9600 does not fail: it prints garbage, slowly, and the status
// backchannel returns bytes that parse as nothing. So every frame parameter is explicit
// and none has a "usual" value inferred from another.

enum class SerialParity { None, Even, Odd };

enum class SerialFlowControl {
  None,     // no handshaking at all: only correct with a printer that never fills up
  RtsCts,   // hardware handshaking — what a receipt printer's manual almost always means
  XonXoff,  // software handshaking; unusable for binary raster data on some firmware
};

struct SerialConfig {
  std::string device;  // e.g. "/dev/ttyUSB0", "/dev/cu.usbserial-1420"
  uint32_t baud = 9600;
  uint8_t data_bits = 8;   // 5..8
  SerialParity parity = SerialParity::None;
  uint8_t stop_bits = 1;   // 1 or 2
  SerialFlowControl flow = SerialFlowControl::None;
  uint32_t write_timeout_ms = 5000;
};

// POSIX termios. Declared unconditionally so that a Windows build still sees the
// configuration type — a fleet description that mentions a serial printer must not stop
// compiling — while core/src/transport_serial.cpp implements it only where termios
// exists. connect() on a platform without it fails with a message saying so, rather than
// pretending to open a port.
class SerialTransport : public Transport {
 public:
  explicit SerialTransport(SerialConfig config);
  ~SerialTransport() override;

  using Transport::write;  // the vector overload, hidden by the override below

  void onBytes(BytesCallback callback) override;
  void onDisconnected(DisconnectCallback callback) override;

  TransportResult connect() override;
  TransportResult write(const uint8_t* data, size_t size) override;
  void close() override;
  bool isConnected() const override;
  std::string describe() const override;

  const SerialConfig& config() const noexcept { return config_; }

  // Whether this build has a termios implementation behind it. False on Windows, where
  // the answer is a real one rather than a crash: the port would have to be opened
  // through the Win32 COM API, which this core does not carry.
  static bool supported() noexcept;

 private:
  void readerLoop();
  void teardown(bool notify, TransportError error, const std::string& message);

  SerialConfig config_;
  BytesCallback on_bytes_;
  DisconnectCallback on_disconnected_;

  mutable std::mutex mutex_;
  int fd_ = -1;
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;
  std::thread reader_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<bool> notified_{false};
};

TransportFactory serial(SerialConfig config);
TransportFactory serial(std::string device, uint32_t baud = 9600);

// --- Custom transports: the link belongs to the embedder ----------------------------
//
// docs/compatibility-brief.md §25. Bluetooth cannot live in this core and should not:
// on Apple it is CoreBluetooth or ExternalAccessory (MFi), on Android a
// BluetoothSocket over RFCOMM, on Linux a BlueZ AF_BLUETOOTH socket, and each of those
// is a platform framework with its own permissions model, its own pairing UI and its
// own threading. Linking any of them into a portable C++17 core would make the core
// unportable to buy one transport.
//
// So the split is: **the platform owns the socket, the core owns the protocol.** The
// embedder implements three operations — connect, write, close — and pushes received
// bytes in. Everything that makes this SDK worth using (the ordered fence, the
// process-ID correlation, preflight, the journal, the confidence grading, the refusal
// to overclaim) happens on this side of the boundary and is identical over Bluetooth,
// TCP or a test double. A wrapper cannot accidentally weaken a completion guarantee,
// because a wrapper never sees one.
//
// -- Thread contract -----------------------------------------------------------------
//
//   * connect/write/close are invoked on the core's worker thread for this printer,
//     one at a time, never concurrently with each other. write() may be called
//     repeatedly; it must transfer the whole buffer or report how much it managed.
//   * feedBytes() may be called from ANY thread — typically the embedder's own RX
//     thread or delegate queue — including while write() is in flight. That is the
//     normal case: a status answer arrives while the next chunk is going out.
//   * feedBytes() must NOT be called from inside connect/write/close. Those run on the
//     thread that would have to service the delivery.
//   * A callback must not call back into any pd_* function or Printer method on the
//     same driver, for the reason documented in pd.h: the worker thread it is running
//     on is the thread that would have to service that call.
//   * The link outlives individual transport instances. The core creates a fresh
//     transport after a link drop, so the embedder keeps feeding the same link and
//     never has to learn about the swap.

class CustomTransport;

class CustomTransportLink {
 public:
  struct Callbacks {
    // True when the platform link is open and ready to carry bytes.
    std::function<bool()> connect;
    // Bytes actually handed to the link, or a negative value for a hard failure. A
    // short write is reported honestly rather than rounded up: zero bytes out is a
    // known failure and one byte out is Unknown (docs/api.md §4), and that distinction
    // is what decides whether an operator should reprint.
    std::function<int64_t(const uint8_t*, size_t)> write;
    std::function<void()> close;
    // What `pdctl` and the printer id derive from, e.g. "bt-spp:00:11:22:33:44:55".
    std::string description = "custom";
  };

  explicit CustomTransportLink(Callbacks callbacks);
  ~CustomTransportLink();

  CustomTransportLink(const CustomTransportLink&) = delete;
  CustomTransportLink& operator=(const CustomTransportLink&) = delete;

  // Delivers received bytes to whichever transport instance is currently bound.
  // False when none is — the core has not connected yet, or has already closed — which
  // is information, not an error: bytes that arrive with nobody listening are dropped
  // rather than queued, exactly as they would be on a socket nobody is reading.
  bool feedBytes(const uint8_t* data, size_t size);

  // The link dropped for a reason other than an explicit close. Surfaces as
  // ConnectionLost on the device event stream and fails any job waiting on a fence.
  bool linkDropped(const std::string& message);

  bool bound() const;
  const std::string& describe() const noexcept { return description_; }

 private:
  friend class CustomTransport;
  void bind(CustomTransport* transport);
  void unbind(CustomTransport* transport);

  Callbacks callbacks_;
  std::string description_;
  mutable std::mutex mutex_;
  CustomTransport* current_ = nullptr;
};

class CustomTransport : public Transport {
 public:
  explicit CustomTransport(std::shared_ptr<CustomTransportLink> link);
  ~CustomTransport() override;

  using Transport::write;  // the vector overload, hidden by the override below

  void onBytes(BytesCallback callback) override;
  void onDisconnected(DisconnectCallback callback) override;

  TransportResult connect() override;
  TransportResult write(const uint8_t* data, size_t size) override;
  void close() override;
  bool isConnected() const override;
  std::string describe() const override;

 private:
  friend class CustomTransportLink;
  // Called by the link, on the embedder's thread, with the link's mutex held.
  void deliver(const uint8_t* data, size_t size);
  void dropped(const std::string& message);

  std::shared_ptr<CustomTransportLink> link_;
  CustomTransportLink::Callbacks callbacks_;
  BytesCallback on_bytes_;
  DisconnectCallback on_disconnected_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> notified_{false};
};

// One link, many transport instances: the factory binds each new instance to the same
// embedder-owned link, so a reconnect after a dropped Bluetooth session needs nothing
// from the embedder but a working connect().
TransportFactory customTransport(std::shared_ptr<CustomTransportLink> link);

}  // namespace pd
