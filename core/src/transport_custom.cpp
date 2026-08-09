#include "printerdriver/transport.hpp"

#include <utility>

// The embedder-owned transport (docs/compatibility-brief.md §25). Platform-independent
// on purpose: this file contains no socket call of any kind, because the whole point is
// that the socket is somebody else's. It is compiled on every platform, unlike
// transport.cpp / transport_win.cpp, which are alternatives.
//
// The division of responsibility is the same one the SDK makes everywhere else. The
// embedder answers "can bytes move?" and nothing more; the core answers "did the
// receipt print?", which is the question that needs the fence, the correlation token,
// the journal and the honesty rules. A Bluetooth wrapper therefore cannot report a
// stronger completion than the hardware gave, because it is not the thing doing the
// reporting.

namespace pd {

// --- CustomTransportLink -------------------------------------------------------------

CustomTransportLink::CustomTransportLink(Callbacks callbacks)
    : callbacks_(std::move(callbacks)), description_(callbacks_.description) {
  if (description_.empty()) {
    description_ = "custom";
  }
}

CustomTransportLink::~CustomTransportLink() = default;

void CustomTransportLink::bind(CustomTransport* transport) {
  std::lock_guard<std::mutex> lock(mutex_);
  current_ = transport;
}

void CustomTransportLink::unbind(CustomTransport* transport) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Only the instance that owns the binding may clear it: a transport destroyed after
  // the core already created its replacement must not unbind the newer one.
  if (current_ == transport) {
    current_ = nullptr;
  }
}

bool CustomTransportLink::feedBytes(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return false;
  }
  // Delivered under the lock, so a transport cannot be destroyed halfway through a
  // delivery. Safe because the receiving callback is the response parser, which by the
  // contract in transport.hpp never calls back into the transport or the link.
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_ == nullptr) {
    return false;
  }
  current_->deliver(data, size);
  return true;
}

bool CustomTransportLink::linkDropped(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_ == nullptr) {
    return false;
  }
  current_->dropped(message);
  return true;
}

bool CustomTransportLink::bound() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_ != nullptr;
}

// --- CustomTransport -----------------------------------------------------------------

// The callbacks are copied out of the link rather than referenced through it, so a
// write never has to take the link's mutex and can therefore run concurrently with the
// embedder feeding received bytes in — which is the normal case on a duplex link.
CustomTransport::CustomTransport(std::shared_ptr<CustomTransportLink> link)
    : link_(std::move(link)), callbacks_(link_->callbacks_) {}

CustomTransport::~CustomTransport() { CustomTransport::close(); }

void CustomTransport::onBytes(BytesCallback callback) {
  on_bytes_ = std::move(callback);
}

void CustomTransport::onDisconnected(DisconnectCallback callback) {
  on_disconnected_ = std::move(callback);
}

TransportResult CustomTransport::connect() {
  if (connected_.load()) {
    return TransportResult::success();
  }
  if (!callbacks_.connect) {
    return TransportResult::failure(TransportError::ConnectFailed,
                                    "custom transport has no connect callback");
  }
  // Bound before connect() runs, not after: a platform link can deliver its first
  // bytes from inside its own connect path (a BLE peripheral pushing a notification as
  // soon as the characteristic subscribes), and those bytes are as real as any other.
  if (link_) {
    link_->bind(this);
  }
  notified_.store(false);
  if (!callbacks_.connect()) {
    if (link_) {
      link_->unbind(this);
    }
    return TransportResult::failure(TransportError::ConnectFailed,
                                    "custom transport refused to connect: " +
                                        callbacks_.description);
  }
  connected_.store(true);
  return TransportResult::success();
}

TransportResult CustomTransport::write(const uint8_t* data, size_t size) {
  if (!connected_.load()) {
    return TransportResult::failure(TransportError::NotConnected,
                                    "custom transport is not connected");
  }
  if (size == 0) {
    return TransportResult::success(0);
  }
  if (data == nullptr) {
    return TransportResult::failure(TransportError::WriteFailed,
                                    "custom transport write has no data");
  }
  if (!callbacks_.write) {
    return TransportResult::failure(TransportError::WriteFailed,
                                    "custom transport has no write callback");
  }
  const int64_t written = callbacks_.write(data, size);
  if (written < 0) {
    return TransportResult::failure(TransportError::WriteFailed,
                                    "custom transport write failed: " +
                                        callbacks_.description);
  }
  const auto count = static_cast<size_t>(written);
  if (count < size) {
    // Reported with the byte count rather than as a plain failure: how far the write
    // got is what separates a job that certainly did not print from one that may have
    // printed most of a receipt (docs/api.md §4).
    return TransportResult::failure(TransportError::WriteFailed,
                                    "custom transport wrote " + std::to_string(count) +
                                        " of " + std::to_string(size) + " bytes",
                                    count);
  }
  return TransportResult::success(count);
}

void CustomTransport::close() {
  const bool was_connected = connected_.exchange(false);
  if (link_) {
    link_->unbind(this);
  }
  if (was_connected && callbacks_.close) {
    callbacks_.close();
  }
}

bool CustomTransport::isConnected() const { return connected_.load(); }

std::string CustomTransport::describe() const { return callbacks_.description; }

void CustomTransport::deliver(const uint8_t* data, size_t size) {
  if (on_bytes_) {
    on_bytes_(data, size);
  }
}

void CustomTransport::dropped(const std::string& message) {
  if (!connected_.exchange(false)) {
    return;
  }
  // At most once per successful connect, matching TcpTransport: a link that drops
  // during teardown must not race a second ConnectionLost onto the event stream.
  if (notified_.exchange(true)) {
    return;
  }
  if (on_disconnected_) {
    on_disconnected_(TransportError::Closed, message);
  }
}

TransportFactory customTransport(std::shared_ptr<CustomTransportLink> link) {
  if (!link) {
    return {};
  }
  return [link]() -> std::unique_ptr<Transport> {
    return std::unique_ptr<Transport>(new CustomTransport(link));
  };
}

}  // namespace pd
