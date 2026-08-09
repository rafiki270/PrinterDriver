#include "printerdriver/platform.hpp"

// M13b. RS-232 over POSIX termios (docs/compatibility-brief.md §1).
//
// The whole file is one platform branch. On Windows a serial port is opened through the
// Win32 COM API with a DCB rather than a termios struct, and that is a different
// implementation rather than an #ifdef inside this one — the same split the socket edge
// already uses (core/src/transport.cpp vs core/src/transport_win.cpp). Until it exists,
// SerialTransport::supported() answers false there and connect() says why, which is a
// real answer instead of a link error or a silent no-op.
//
// The reader is a thread on poll(), woken through a self-pipe, exactly like the TCP
// transport: closing a port out from under a blocking read is how a driver ends up with a
// thread that outlives its printer.

#if PD_PLATFORM_WINDOWS

#include "printerdriver/transport.hpp"

namespace pd {

SerialTransport::SerialTransport(SerialConfig config) : config_(std::move(config)) {}
SerialTransport::~SerialTransport() = default;

void SerialTransport::onBytes(BytesCallback callback) { on_bytes_ = std::move(callback); }
void SerialTransport::onDisconnected(DisconnectCallback callback) {
  on_disconnected_ = std::move(callback);
}

TransportResult SerialTransport::connect() {
  return TransportResult::failure(
      TransportError::ConnectFailed,
      "serial ports need the Win32 COM API, which this build does not carry");
}

TransportResult SerialTransport::write(const uint8_t*, size_t) {
  return TransportResult::failure(TransportError::NotConnected, "serial is not supported");
}

void SerialTransport::close() {}
bool SerialTransport::isConnected() const { return false; }
std::string SerialTransport::describe() const { return "serial://" + config_.device; }
bool SerialTransport::supported() noexcept { return false; }

}  // namespace pd

#else

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "printerdriver/transport.hpp"

namespace pd {
namespace {

constexpr size_t kReadBufferSize = 4096;

std::string errnoMessage(const std::string& what) {
  return what + ": " + std::strerror(errno);
}

// The documented rates a receipt printer's DIP switches or setup utility actually offer.
// A rate outside this table is refused rather than rounded to a neighbour: a port opened
// at the wrong speed does not fail, it prints noise, and rounding would make that the
// SDK's fault instead of a configuration error the caller can see.
bool baudConstant(uint32_t baud, speed_t* out) {
  switch (baud) {
    case 1200: *out = B1200; return true;
    case 2400: *out = B2400; return true;
    case 4800: *out = B4800; return true;
    case 9600: *out = B9600; return true;
    case 19200: *out = B19200; return true;
    case 38400: *out = B38400; return true;
    case 57600: *out = B57600; return true;
    case 115200: *out = B115200; return true;
    case 230400: *out = B230400; return true;
    default: return false;
  }
}

bool applyFrame(termios* settings, const SerialConfig& config, std::string* error) {
  // Raw mode first: no canonical line handling, no echo, no CR/LF translation, no
  // XON/XOFF unless it is asked for. A receipt is binary — a raster block contains 0x11
  // and 0x13 — so any of those left on would corrupt an image and nothing else, which is
  // the hardest kind of bug to find on a printer.
  ::cfmakeraw(settings);

  speed_t speed = B9600;
  if (!baudConstant(config.baud, &speed)) {
    *error = "unsupported serial baud rate " + std::to_string(config.baud);
    return false;
  }
  if (::cfsetispeed(settings, speed) != 0 || ::cfsetospeed(settings, speed) != 0) {
    *error = errnoMessage("cfsetspeed");
    return false;
  }

  settings->c_cflag &= static_cast<tcflag_t>(~CSIZE);
  switch (config.data_bits) {
    case 5: settings->c_cflag |= CS5; break;
    case 6: settings->c_cflag |= CS6; break;
    case 7: settings->c_cflag |= CS7; break;
    case 8: settings->c_cflag |= CS8; break;
    default:
      *error = "unsupported serial data bits " + std::to_string(config.data_bits);
      return false;
  }

  switch (config.parity) {
    case SerialParity::None:
      settings->c_cflag &= static_cast<tcflag_t>(~PARENB);
      break;
    case SerialParity::Even:
      settings->c_cflag |= PARENB;
      settings->c_cflag &= static_cast<tcflag_t>(~PARODD);
      break;
    case SerialParity::Odd:
      settings->c_cflag |= PARENB;
      settings->c_cflag |= PARODD;
      break;
  }

  if (config.stop_bits == 2) {
    settings->c_cflag |= CSTOPB;
  } else if (config.stop_bits == 1) {
    settings->c_cflag &= static_cast<tcflag_t>(~CSTOPB);
  } else {
    *error = "unsupported serial stop bits " + std::to_string(config.stop_bits);
    return false;
  }

  settings->c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));
#ifdef CRTSCTS
  settings->c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
  switch (config.flow) {
    case SerialFlowControl::None:
      break;
    case SerialFlowControl::RtsCts:
#ifdef CRTSCTS
      settings->c_cflag |= CRTSCTS;
#else
      *error = "this platform's termios has no RTS/CTS flow control";
      return false;
#endif
      break;
    case SerialFlowControl::XonXoff:
      settings->c_iflag |= static_cast<tcflag_t>(IXON | IXOFF);
      break;
  }

  // Read what is there, block on poll() rather than in read(): the reader thread owns its
  // own timing and must be interruptible through the wake pipe.
  settings->c_cc[VMIN] = 0;
  settings->c_cc[VTIME] = 0;
  // Receive enabled, and the line is ours rather than a controlling terminal's.
  settings->c_cflag |= static_cast<tcflag_t>(CREAD | CLOCAL);
  return true;
}

bool setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

SerialTransport::SerialTransport(SerialConfig config) : config_(std::move(config)) {}

SerialTransport::~SerialTransport() { close(); }

void SerialTransport::onBytes(BytesCallback callback) { on_bytes_ = std::move(callback); }

void SerialTransport::onDisconnected(DisconnectCallback callback) {
  on_disconnected_ = std::move(callback);
}

bool SerialTransport::supported() noexcept { return true; }

std::string SerialTransport::describe() const {
  return "serial://" + config_.device + "@" + std::to_string(config_.baud);
}

bool SerialTransport::isConnected() const { return connected_.load(); }

TransportResult SerialTransport::connect() {
  if (connected_.load()) {
    return TransportResult::success();
  }
  closing_.store(false);
  notified_.store(false);

  if (config_.device.empty()) {
    return TransportResult::failure(TransportError::ConnectFailed,
                                    "no serial device configured");
  }
  // O_NOCTTY because this port must never become the process's controlling terminal: a
  // hangup on the printer's cable would otherwise deliver SIGHUP to an application that
  // has nothing to do with terminals.
  const int fd = ::open(config_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return TransportResult::failure(TransportError::ConnectFailed,
                                    errnoMessage("open " + config_.device));
  }

  termios settings{};
  if (::tcgetattr(fd, &settings) != 0) {
    const std::string message = errnoMessage("tcgetattr");
    ::close(fd);
    return TransportResult::failure(TransportError::ConnectFailed, message);
  }
  std::string error;
  if (!applyFrame(&settings, config_, &error)) {
    ::close(fd);
    return TransportResult::failure(TransportError::ConnectFailed, error);
  }
  // TCSANOW rather than TCSADRAIN: nothing of ours is in the output queue yet, and
  // draining whatever a previous owner left there would block the open.
  if (::tcsetattr(fd, TCSANOW, &settings) != 0) {
    const std::string message = errnoMessage("tcsetattr");
    ::close(fd);
    return TransportResult::failure(TransportError::ConnectFailed, message);
  }
  // Discard anything buffered from before we owned the port. A stale status byte would
  // otherwise be attributed to this session's very first fence.
  ::tcflush(fd, TCIOFLUSH);

  int wake[2] = {-1, -1};
  if (::pipe(wake) != 0) {
    const std::string message = errnoMessage("pipe");
    ::close(fd);
    return TransportResult::failure(TransportError::ConnectFailed, message);
  }
  setNonBlocking(wake[0]);
  setNonBlocking(wake[1]);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd_ = fd;
    wake_read_fd_ = wake[0];
    wake_write_fd_ = wake[1];
  }
  connected_.store(true);
  reader_ = std::thread([this] { readerLoop(); });
  return TransportResult::success();
}

TransportResult SerialTransport::write(const uint8_t* data, size_t size) {
  if (!connected_.load()) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "serial port is not open");
  }
  if (data == nullptr || size == 0) {
    return TransportResult::success(0);
  }
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
  }
  if (fd < 0) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "serial port is not open");
  }
  size_t sent = 0;
  while (sent < size) {
    const ssize_t wrote = ::write(fd, data + sent, size - sent);
    if (wrote > 0) {
      sent += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      // The port is full — a printer with hardware handshaking asserting flow control, or
      // a slow line at 9600 baud. Wait for room, but only for the configured budget: a
      // printer that never lifts RTS again must not wedge the worker thread for ever.
      pollfd waiter{};
      waiter.fd = fd;
      waiter.events = POLLOUT;
      const int ready = ::poll(&waiter, 1, static_cast<int>(config_.write_timeout_ms));
      if (ready > 0) {
        continue;
      }
      // Reported with the byte count, not rounded up. Zero out is a known failure and one
      // byte out is Unknown (docs/api.md §4), and on a 9600-baud line that distinction is
      // reached far more often than on Ethernet.
      return TransportResult::failure(
          TransportError::WriteFailed,
          ready == 0 ? "serial write timed out with the port not accepting data"
                     : errnoMessage("poll"),
          sent);
    }
    return TransportResult::failure(TransportError::WriteFailed, errnoMessage("write"),
                                    sent);
  }
  return TransportResult::success(sent);
}

void SerialTransport::readerLoop() {
  std::vector<uint8_t> buffer(kReadBufferSize);
  for (;;) {
    int fd = -1;
    int wake = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      fd = fd_;
      wake = wake_read_fd_;
    }
    if (fd < 0 || closing_.load()) {
      return;
    }
    pollfd waiters[2]{};
    waiters[0].fd = fd;
    waiters[0].events = POLLIN;
    waiters[1].fd = wake;
    waiters[1].events = POLLIN;
    const int ready = ::poll(waiters, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      teardown(true, TransportError::Closed, errnoMessage("poll"));
      return;
    }
    if (closing_.load() || (waiters[1].revents & POLLIN) != 0) {
      return;
    }
    if ((waiters[0].revents & (POLLERR | POLLNVAL)) != 0) {
      teardown(true, TransportError::Closed, "serial port reported an error");
      return;
    }
    if ((waiters[0].revents & POLLIN) == 0) {
      // POLLHUP alone on a pty means the other end went away. On a real UART with CLOCAL
      // set it should not happen, and treating it as a drop is the honest reading either
      // way: nothing more is coming.
      if ((waiters[0].revents & POLLHUP) != 0) {
        teardown(true, TransportError::Closed, "serial peer closed the line");
        return;
      }
      continue;
    }
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got > 0) {
      if (on_bytes_) {
        on_bytes_(buffer.data(), static_cast<size_t>(got));
      }
      continue;
    }
    if (got == 0) {
      // End of file on a serial line: the pty peer closed, or the adapter was unplugged.
      teardown(true, TransportError::Closed, "serial peer closed the line");
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    teardown(true, TransportError::Closed, errnoMessage("read"));
    return;
  }
}

void SerialTransport::teardown(bool notify, TransportError error,
                               const std::string& message) {
  int fd = -1;
  int wake_read = -1;
  int wake_write = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
    wake_read = wake_read_fd_;
    wake_write = wake_write_fd_;
    fd_ = -1;
    wake_read_fd_ = -1;
    wake_write_fd_ = -1;
  }
  connected_.store(false);
  if (fd >= 0) {
    ::close(fd);
  }
  if (wake_read >= 0) {
    ::close(wake_read);
  }
  if (wake_write >= 0) {
    ::close(wake_write);
  }
  if (notify && !notified_.exchange(true) && on_disconnected_) {
    on_disconnected_(error, message);
  }
}

void SerialTransport::close() {
  if (closing_.exchange(true)) {
    return;
  }
  int wake_write = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_write = wake_write_fd_;
  }
  if (wake_write >= 0) {
    const char byte = 'x';
    const ssize_t ignored = ::write(wake_write, &byte, 1);
    (void)ignored;
  }
  if (reader_.joinable()) {
    reader_.join();
  }
  // notify = false: an explicit close is not a link drop, and reporting one would fail a
  // job that the caller has just decided to abandon.
  teardown(false, TransportError::Closed, "closed");
}

}  // namespace pd

#endif  // PD_PLATFORM_WINDOWS

namespace pd {

TransportFactory serial(SerialConfig config) {
  return [config]() -> std::unique_ptr<Transport> {
    return std::unique_ptr<Transport>(new SerialTransport(config));
  };
}

TransportFactory serial(std::string device, uint32_t baud) {
  SerialConfig config;
  config.device = std::move(device);
  config.baud = baud;
  return serial(config);
}

}  // namespace pd
