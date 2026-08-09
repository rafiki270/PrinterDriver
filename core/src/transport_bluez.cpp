#if defined(__linux__) || defined(PD_FORCE_LINUX_BLUETOOTH)

#include "printerdriver/transport_bluez.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <utility>

// BlueZ RFCOMM for the Linux agent (docs/compatibility-brief.md §25, "classic SPP").
//
// -- Why this one lives in the core and the others do not ----------------------------
//
// Apple and Android reach Bluetooth through frameworks the core must not link
// (CoreBluetooth, ExternalAccessory, android.bluetooth), so those platforms drive a
// printer through the embedder-owned custom transport in transport_custom.cpp. Linux
// does not: BlueZ classic SPP is a BSD socket with a different address family, so the
// same POSIX code that already runs TcpTransport runs this one, and putting it behind
// the callback boundary would add a hop for nothing.
//
// -- Verification status -------------------------------------------------------------
//
// SYNTAX- AND TYPE-CHECKED ONLY, exactly like the Windows edge and for the same reason:
// the development host is a Mac, which has neither BlueZ nor a paired RFCOMM printer.
// What has been done to this file is
//
//   clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
//     -DPD_FORCE_LINUX_BLUETOOTH -I core/tests/linux_bluetooth_stub \
//     -I core/include core/src/transport_bluez.cpp
//
// against core/tests/linux_bluetooth_stub/bluetooth/*.h, a hand-written stand-in for
// BlueZ's headers whose declarations mirror the real ones in the ways that matter
// (bdaddr_t is six packed bytes, sockaddr_rc carries rc_family/rc_bdaddr/rc_channel,
// str2ba/ba2str have BlueZ's signatures, BTPROTO_RFCOMM is the third argument to
// socket()). See scripts/check_linux_bluetooth_syntax.sh, which also runs negative
// controls proving the check can fail.
//
// That establishes that every BlueZ call here has the right name, arity and argument
// types. It establishes NOTHING about behaviour: this has never been compiled against
// real BlueZ headers, never linked against libbluetooth, never connected to a printer.
// A paired RFCOMM device is the only thing that can make it evidence.
//
// The whole file is inside the guard above, so it is an empty translation unit
// everywhere else. That matters because SwiftPM compiles every file in core/src
// unconditionally: an unguarded Linux socket call here would break the iOS build.

namespace pd {
namespace {

// BlueZ's SPP channels are 1-30; 1 is what every receipt printer this brief covers
// advertises, and SDP lookup needs libbluetooth's SDP client, which is a dependency
// this core will not take. A caller that knows better sets the channel explicitly.
constexpr uint8_t kDefaultRfcommChannel = 1;

constexpr size_t kReadChunk = 512;

std::string errnoMessage(const char* what) {
  return std::string(what) + ": " + std::string(strerror(errno));
}

}  // namespace

BluezRfcommTransport::BluezRfcommTransport(BluezRfcommConfig config)
    : config_(std::move(config)) {
  if (config_.channel == 0) {
    config_.channel = kDefaultRfcommChannel;
  }
}

BluezRfcommTransport::~BluezRfcommTransport() { BluezRfcommTransport::close(); }

void BluezRfcommTransport::onBytes(BytesCallback callback) {
  on_bytes_ = std::move(callback);
}

void BluezRfcommTransport::onDisconnected(DisconnectCallback callback) {
  on_disconnected_ = std::move(callback);
}

TransportResult BluezRfcommTransport::connect() {
  if (connected_.load()) {
    return TransportResult::success();
  }
  bdaddr_t address;
  if (str2ba(config_.address.c_str(), &address) < 0) {
    return TransportResult::failure(TransportError::ConnectFailed,
                                    "not a Bluetooth address: " + config_.address);
  }

  const int fd = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (fd < 0) {
    return TransportResult::failure(TransportError::ConnectFailed,
                                    errnoMessage("rfcomm socket"));
  }

  // Zero-initialised in full: sockaddr_rc has padding on some ABIs and the kernel
  // reads the whole structure.
  struct sockaddr_rc target;
  memset(&target, 0, sizeof(target));
  target.rc_family = AF_BLUETOOTH;
  target.rc_channel = config_.channel;
  target.rc_bdaddr = address;

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&target), sizeof(target)) < 0) {
    const std::string message = errnoMessage("rfcomm connect");
    ::close(fd);
    // ETIMEDOUT here is the pairing/out-of-range case, which an operator can act on;
    // anything else is reported as a plain connect failure.
    return TransportResult::failure(errno == ETIMEDOUT ? TransportError::ConnectTimeout
                                                       : TransportError::ConnectFailed,
                                    message);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd_ = fd;
  }
  connected_.store(true);
  closing_.store(false);
  notified_.store(false);
  // The link stays open after the write, for the same reason TcpTransport's does:
  // closing after sendall() is what stops status bytes from ever coming back
  // (docs/techspec.md §4). On a 64 KB Bluetooth receive buffer that is the difference
  // between a fenced job and a hopeful one.
  reader_ = std::thread([this] { readerLoop(); });
  return TransportResult::success();
}

TransportResult BluezRfcommTransport::write(const uint8_t* data, size_t size) {
  if (!connected_.load()) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "rfcomm link is not connected");
  }
  if (size == 0) {
    return TransportResult::success(0);
  }
  if (data == nullptr) {
    return TransportResult::failure(TransportError::WriteFailed, "rfcomm write has no data");
  }

  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
  }
  if (fd < 0) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "rfcomm link is not connected");
  }

  size_t sent = 0;
  while (sent < size) {
    const ssize_t wrote = ::send(fd, data + sent, size - sent, 0);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      // The byte count travels with the failure: an RFCOMM link that dies mid-receipt
      // leaves a job Unknown rather than Failed, and only this number says which.
      return TransportResult::failure(TransportError::WriteFailed,
                                      errnoMessage("rfcomm send"), sent);
    }
    if (wrote == 0) {
      return TransportResult::failure(TransportError::Closed, "rfcomm link closed", sent);
    }
    sent += static_cast<size_t>(wrote);
  }
  return TransportResult::success(sent);
}

void BluezRfcommTransport::close() {
  closing_.store(true);
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
    fd_ = -1;
  }
  if (fd >= 0) {
    // shutdown() before close() so a reader blocked in recv() returns immediately;
    // there is no self-pipe here because an RFCOMM socket has no second descriptor to
    // multiplex and the reader has nothing else to wait on.
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  connected_.store(false);
  if (reader_.joinable()) {
    if (std::this_thread::get_id() == reader_.get_id()) {
      reader_.detach();
    } else {
      reader_.join();
    }
  }
}

bool BluezRfcommTransport::isConnected() const { return connected_.load(); }

std::string BluezRfcommTransport::describe() const {
  return "bt-rfcomm:" + config_.address + ":" + std::to_string(config_.channel);
}

void BluezRfcommTransport::readerLoop() {
  std::vector<uint8_t> buffer(kReadChunk);
  for (;;) {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      fd = fd_;
    }
    if (fd < 0 || closing_.load()) {
      return;
    }
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (got > 0) {
      if (on_bytes_) {
        on_bytes_(buffer.data(), static_cast<size_t>(got));
      }
      continue;
    }
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (closing_.load()) {
      return;
    }
    connected_.store(false);
    if (!notified_.exchange(true) && on_disconnected_) {
      on_disconnected_(TransportError::Closed,
                       got == 0 ? std::string("rfcomm peer closed the link")
                                : errnoMessage("rfcomm recv"));
    }
    return;
  }
}

TransportFactory bluezRfcomm(BluezRfcommConfig config) {
  return [config]() -> std::unique_ptr<Transport> {
    return std::unique_ptr<Transport>(new BluezRfcommTransport(config));
  };
}

}  // namespace pd

#endif  // __linux__ || PD_FORCE_LINUX_BLUETOOTH
