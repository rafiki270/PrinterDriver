#pragma once

// printerdriver/net_platform.hpp brings in the platform's socket headers — BSD sockets
// or Winsock2 — so that the one real-socket fixture in this file (FakePrinterServer,
// used by the end-to-end smoke test) compiles on both. The directory and process-id
// calls TempDir needs have no such shared spelling and are branched inline below.
#include "printerdriver/net_platform.hpp"

#if PD_PLATFORM_WINDOWS
#ifndef PD_WINDOWS_SYNTAX_CHECK
#include <windows.h>
#endif
#else
#include <dirent.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "printerdriver/capability_profile.hpp"
#include "printerdriver/transport.hpp"

// A scriptable in-process printer. It models the one property that matters for
// completion fencing: the command stream is FIFO, so a queued response (GS ( H echo,
// GS r 1 answer) is produced exactly when the scanner reaches that command, after
// everything printed ahead of it. Real-time DLE EOT answers are produced the moment
// they are scanned, which is the same "may overtake buffered data" behaviour the real
// device has when the buffer is not empty.

namespace pdfake {

struct MarkerRecord {
  std::string token;
  // How much print data had been consumed when this marker was reached. Interleaving
  // between two jobs is visible as a marker recorded after the next job's text.
  size_t print_data_len = 0;
};

struct Script {
  bool answer_realtime = true;
  // Defaults are the healthy pattern: bit 4 set, bit 1 set, everything else clear,
  // which is what pd::escpos::looksLikeRealtimeByte recognises.
  uint8_t printer_status = 0x16;  // DLE EOT 1: online, drawer pin high
  uint8_t offline_cause = 0x12;   // DLE EOT 2: cover closed, paper present
  uint8_t error_cause = 0x12;     // DLE EOT 3: no cutter error
  uint8_t paper_sensor = 0x12;    // DLE EOT 4: paper ok

  bool answer_process_id = true;
  bool answer_queued_status = true;
  uint8_t queued_status = 0x00;

  // Stop answering process-ID markers after this many have been echoed; 0 = never.
  size_t process_id_answer_limit = 0;

  // Emitted once, as an extra well-formed GS ( H frame, the first time this device is
  // asked for a process-ID echo. This is the multi-writer case of docs/sdk-spec.md §14:
  // a second instance's receipt finishing on the same socket. Must be four printable
  // bytes; the driver decides whose it is from the token's instance nonce.
  std::string foreign_process_id;

  // GS I identification. The defaults are Rongta's documented Epson impersonation
  // (docs/capability-profiles.md §5), because that is the case the identification
  // path has to survive.
  bool answer_identity = false;
  std::string gs_i_manufacturer = "EPOSN";
  std::string gs_i_model = "TM-T88V";
  std::string gs_i_firmware = "1.02";
  std::string gs_i_serial;
  uint8_t gs_i_model_id = 0x20;
  uint8_t gs_i_type_id = 0x02;
  uint8_t gs_i_rom_version = 0x01;
  // Epson frames text answers 5F <data> 00; clones vary, and the parser has to cope.
  bool gs_i_header = true;

  bool answer_asb = true;
  // ASB frame sent when GS a enables status back: healthy, cover closed, paper ok.
  std::array<uint8_t, 4> asb_frame{0x10, 0x10, 0x10, 0x10};

  // --- M14: the cash drawer (docs/cash-drawer.md) ---------------------------------
  // A drawer is a solenoid latch plus a microswitch, and the whole point of the
  // milestone is that those two are separate: the pulse is an output and the switch is
  // an input, and a device can perfectly well accept the first while the second says
  // nothing. Each flag below turns one of the doc's cases into something a test can
  // hold still.

  // Whether GS r 2 is answered at all. False is the print-server / no-back-channel
  // case, where the kick travels forward and the sensor answer never returns.
  bool answer_drawer_status = true;
  // The microswitch's current position, which the fixture moves rather than the test.
  bool drawer_open = false;
  // A healthy drawer opens when the solenoid is energised. False is the locked
  // drawer, the jam, the wrong channel and the wrong cable — every case that ends at
  // FailedToOpen, all of which look identical from this side of the connector.
  bool drawer_opens_on_kick = true;
  // Star's warning made concrete: the level the sense line sits at when the drawer is
  // open depends on the drawer, not on the printer.
  bool drawer_pin_high_when_open = true;
  // Only this channel physically moves anything. Kicking the other one is accepted by
  // the firmware and does nothing, which is exactly what a miswired install looks like.
  uint8_t drawer_wired_channel = 1;
};

// One ESC p as the device saw it, decoded (docs/cash-drawer.md §1): channel 1 is
// m = 0/48 (pin 2) and channel 2 is m = 1/49 (pin 5); the times are in 2 ms units.
struct DrawerKickRecord {
  uint8_t channel = 1;
  uint8_t on_units = 0;
  uint8_t off_units = 0;
  // How much print data had been consumed when the pulse was reached, so a test can
  // prove that a pulse never landed inside a receipt.
  size_t print_data_len = 0;
};

inline uint8_t withBit(uint8_t base, unsigned index) {
  return static_cast<uint8_t>(base | (1u << index));
}

// DLE EOT 2 / ASB byte 1: cover open is bit 2.
inline uint8_t coverOpenByte() { return withBit(0x12, 2); }
// DLE EOT 4 / DLE EOT 2: paper out is bit 5.
inline uint8_t paperOutByte() { return withBit(0x12, 5); }
// DLE EOT 3: autocutter error is bit 3.
inline uint8_t cutterErrorByte() { return withBit(0x12, 3); }

class FakePrinter {
 public:
  explicit FakePrinter(Script script = {}) : script_(script) {}

  void setScript(const Script& script) {
    std::lock_guard<std::mutex> lock(mutex_);
    script_ = script;
  }

  // Consumes host bytes and returns whatever the device would send back.
  std::vector<uint8_t> receive(const uint8_t* data, size_t size) {
    const int concurrent = ++in_flight_;
    if (concurrent > 1) {
      concurrent_writes_.store(true);
    }
    std::vector<uint8_t> out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      received_.insert(received_.end(), data, data + size);
      pending_.insert(pending_.end(), data, data + size);
      scan(out);
    }
    --in_flight_;
    return out;
  }

  std::vector<uint8_t> received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
  }
  std::string printText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::string(print_data_.begin(), print_data_.end());
  }
  size_t printDataBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return print_data_.size();
  }
  std::vector<MarkerRecord> markers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return markers_;
  }
  std::vector<uint8_t> realtimeRequests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return realtime_requests_;
  }
  size_t queuedRequests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_requests_;
  }
  size_t cuts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cuts_;
  }
  size_t drawerKicks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drawer_kicks_;
  }
  // --- M14 ---
  std::vector<DrawerKickRecord> drawerKickRecords() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drawer_kick_records_;
  }
  size_t drawerStatusRequests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drawer_status_requests_;
  }
  bool drawerOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return script_.drawer_open;
  }
  // Moves the physical drawer without going through the printer, which is what an
  // operator does during the non-destructive calibration of `pdctl drawer-probe`.
  void setDrawerOpen(bool open) {
    std::lock_guard<std::mutex> lock(mutex_);
    script_.drawer_open = open;
  }
  // --- end M14 ---
  size_t rasterBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raster_blocks_;
  }
  std::vector<uint8_t> identityRequests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_requests_;
  }
  size_t asbEnables() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return asb_enables_;
  }
  bool sawConcurrentWrites() const { return concurrent_writes_.load(); }

  bool receivedContains(const std::string& needle) const {
    const std::vector<uint8_t> bytes = received();
    if (needle.empty() || bytes.size() < needle.size()) {
      return false;
    }
    for (size_t i = 0; i + needle.size() <= bytes.size(); ++i) {
      if (std::memcmp(bytes.data() + i, needle.data(), needle.size()) == 0) {
        return true;
      }
    }
    return false;
  }

 private:
  static uint16_t le16(uint8_t low, uint8_t high) {
    return static_cast<uint16_t>(low | (static_cast<uint16_t>(high) << 8));
  }

  void emitIdentityText(std::vector<uint8_t>& out, const std::string& text) {
    if (text.empty()) {
      return;
    }
    if (script_.gs_i_header) {
      out.push_back(0x5F);
    }
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0x00);
  }

  void emitIdentity(std::vector<uint8_t>& out, uint8_t kind) {
    switch (kind) {
      case 1: out.push_back(script_.gs_i_model_id); break;
      case 2: out.push_back(script_.gs_i_type_id); break;
      case 3: out.push_back(script_.gs_i_rom_version); break;
      case 65: emitIdentityText(out, script_.gs_i_firmware); break;
      case 66: emitIdentityText(out, script_.gs_i_manufacturer); break;
      case 67: emitIdentityText(out, script_.gs_i_model); break;
      case 68: emitIdentityText(out, script_.gs_i_serial); break;
      default: break;
    }
  }

  void emitProcessIdAck(std::vector<uint8_t>& out, const std::string& token) {
    out.push_back(0x37);
    out.push_back(0x22);
    for (const char c : token) {
      out.push_back(static_cast<uint8_t>(c));
    }
    out.push_back(0x00);
  }

  // Returns the length of a fixed-size command starting at `offset`, or 0 when the
  // bytes there are not one of the commands this core emits.
  size_t fixedCommandLength(size_t offset) const {
    const uint8_t first = pending_[offset];
    const uint8_t second = offset + 1 < pending_.size() ? pending_[offset + 1] : 0;
    if (first == 0x1B) {
      switch (second) {
        case 0x40: return 2;                        // ESC @
        case 0x74: case 0x61: case 0x45:            // ESC t / ESC a / ESC E
        case 0x2D: case 0x64: case 0x4A: return 3;  // ESC - / ESC d / ESC J
        case 0x70: return 5;                        // ESC p
        default: return 0;
      }
    }
    if (first == 0x1D) {
      switch (second) {
        case 0x21: case 0x61: return 3;             // GS ! / GS a
        default: return 0;
      }
    }
    return 0;
  }

  void scan(std::vector<uint8_t>& out) {
    size_t offset = 0;
    while (offset < pending_.size()) {
      const uint8_t first = pending_[offset];

      if (first == 0x10) {  // DLE EOT n
        if (offset + 2 >= pending_.size()) {
          break;
        }
        if (pending_[offset + 1] == 0x04) {
          const uint8_t kind = pending_[offset + 2];
          realtime_requests_.push_back(kind);
          if (script_.answer_realtime) {
            switch (kind) {
              case 1: out.push_back(script_.printer_status); break;
              case 2: out.push_back(script_.offline_cause); break;
              case 3: out.push_back(script_.error_cause); break;
              case 4: out.push_back(script_.paper_sensor); break;
              default: break;
            }
          }
          offset += 3;
          continue;
        }
      }

      if (first == 0x1D && offset + 1 < pending_.size()) {
        const uint8_t second = pending_[offset + 1];
        if (second == 0x28) {  // GS ( X pL pH ...
          if (offset + 4 >= pending_.size()) {
            break;
          }
          const uint16_t length = le16(pending_[offset + 3], pending_[offset + 4]);
          const size_t total = 5u + length;
          if (offset + total > pending_.size()) {
            break;
          }
          if (pending_[offset + 2] == 0x48 && length == 6 &&
              pending_[offset + 5] == 0x30) {
            const std::string token(pending_.begin() + static_cast<long>(offset) + 7,
                                    pending_.begin() + static_cast<long>(offset) + 11);
            markers_.push_back(MarkerRecord{token, print_data_.size()});
            if (!script_.foreign_process_id.empty() && !foreign_emitted_) {
              foreign_emitted_ = true;
              emitProcessIdAck(out, script_.foreign_process_id);
            }
            const bool exhausted =
                script_.process_id_answer_limit != 0 &&
                process_ids_answered_ >= script_.process_id_answer_limit;
            if (script_.answer_process_id && !exhausted) {
              ++process_ids_answered_;
              emitProcessIdAck(out, token);
            }
          }
          offset += total;
          continue;
        }
        if (second == 0x72 && offset + 2 < pending_.size()) {  // GS r n
          // M14. GS r 1 is the paper-status completion fence; GS r 2 (and GS r 50) is
          // the drawer-kick-out connector status, and the two are different questions
          // answered by different bytes. Answering one with the other is precisely the
          // conflation docs/cash-drawer.md keeps separating.
          const uint8_t kind = pending_[offset + 2];
          if (kind == 2 || kind == 50) {
            ++drawer_status_requests_;
            if (script_.answer_drawer_status) {
              const bool high = script_.drawer_open == script_.drawer_pin_high_when_open;
              out.push_back(static_cast<uint8_t>(high ? 0x01 : 0x00));
            }
          } else {
            ++queued_requests_;
            if (script_.answer_queued_status) {
              out.push_back(script_.queued_status);
            }
          }
          offset += 3;
          continue;
        }
        if (second == 0x49) {  // GS I n
          if (offset + 2 >= pending_.size()) {
            break;
          }
          const uint8_t kind = pending_[offset + 2];
          identity_requests_.push_back(kind);
          if (script_.answer_identity) {
            emitIdentity(out, kind);
          }
          offset += 3;
          continue;
        }
        if (second == 0x61) {  // GS a n
          if (offset + 2 >= pending_.size()) {
            break;
          }
          if (pending_[offset + 2] != 0x00) {
            ++asb_enables_;
            if (script_.answer_asb) {
              out.insert(out.end(), script_.asb_frame.begin(), script_.asb_frame.end());
            }
          }
          offset += 3;
          continue;
        }
        if (second == 0x76 && offset + 2 < pending_.size() &&
            pending_[offset + 2] == 0x30) {  // GS v 0 m xL xH yL yH d...
          if (offset + 7 >= pending_.size()) {
            break;
          }
          const size_t bytes_per_row = le16(pending_[offset + 4], pending_[offset + 5]);
          const size_t rows = le16(pending_[offset + 6], pending_[offset + 7]);
          const size_t total = 8u + bytes_per_row * rows;
          if (offset + total > pending_.size()) {
            break;
          }
          raster_blocks_ += 1;
          offset += total;
          continue;
        }
        if (second == 0x56) {  // GS V m | GS V 65/66 n
          if (offset + 2 >= pending_.size()) {
            break;
          }
          const uint8_t mode = pending_[offset + 2];
          const size_t total = (mode == 65 || mode == 66) ? 4u : 3u;
          if (offset + total > pending_.size()) {
            break;
          }
          ++cuts_;
          offset += total;
          continue;
        }
      }

      if (first == 0x1B && offset + 1 < pending_.size() && pending_[offset + 1] == 0x70) {
        if (offset + 5 > pending_.size()) {
          break;
        }
        ++drawer_kicks_;
        // M14. Decode it rather than just counting it: which output was energised and
        // for how long are the two things an integrator is actually testing.
        DrawerKickRecord record;
        record.channel = static_cast<uint8_t>((pending_[offset + 2] % 48u) + 1u);
        record.on_units = pending_[offset + 3];
        record.off_units = pending_[offset + 4];
        record.print_data_len = print_data_.size();
        drawer_kick_records_.push_back(record);
        // The solenoid only moves the drawer it is wired to.
        if (script_.drawer_opens_on_kick && record.channel == script_.drawer_wired_channel) {
          script_.drawer_open = true;
        }
        offset += 5;
        continue;
      }

      const size_t fixed = fixedCommandLength(offset);
      if (fixed != 0) {
        if (offset + fixed > pending_.size()) {
          break;
        }
        offset += fixed;
        continue;
      }
      if ((first == 0x1B || first == 0x1D) && offset + 1 >= pending_.size()) {
        break;  // Escape byte with its selector still in flight.
      }

      print_data_.push_back(first);
      offset += 1;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(offset));
  }

  mutable std::mutex mutex_;
  Script script_;
  std::vector<uint8_t> received_;
  std::vector<uint8_t> pending_;
  std::vector<uint8_t> print_data_;
  std::vector<MarkerRecord> markers_;
  std::vector<uint8_t> realtime_requests_;
  std::vector<uint8_t> identity_requests_;
  size_t asb_enables_ = 0;
  size_t queued_requests_ = 0;
  size_t cuts_ = 0;
  size_t drawer_kicks_ = 0;
  std::vector<DrawerKickRecord> drawer_kick_records_;  // M14
  size_t drawer_status_requests_ = 0;                  // M14
  size_t raster_blocks_ = 0;
  size_t process_ids_answered_ = 0;
  bool foreign_emitted_ = false;
  std::atomic<int> in_flight_{0};
  std::atomic<bool> concurrent_writes_{false};
};

// --- Mock transport ---------------------------------------------------------------

struct MockBehaviour {
  bool connect_fails = false;
  std::string connect_error = "connection refused";
  // Total payload bytes accepted before writes start failing; 0 = never fail.
  size_t fail_write_after_bytes = 0;
  bool disconnect_on_write_failure = true;
  // Runs on the writing thread with the bytes about to be handed to the device.
  std::function<void(const uint8_t*, size_t)> before_write;
};

// Responses are delivered synchronously from inside write(), which removes every
// timing race from the engine tests: the ack is either produced by the scan or it is
// never coming, and the engine has to behave the same way either way.
class MockTransport : public pd::Transport {
 public:
  struct Stats {
    std::atomic<size_t> writes{0};
    std::atomic<size_t> bytes{0};
    std::atomic<size_t> connects{0};
    std::atomic<size_t> closes{0};
  };

  MockTransport(std::shared_ptr<FakePrinter> device, MockBehaviour behaviour,
                std::shared_ptr<Stats> stats)
      : device_(std::move(device)), behaviour_(std::move(behaviour)),
        stats_(std::move(stats)) {}

  void onBytes(BytesCallback callback) override { on_bytes_ = std::move(callback); }
  void onDisconnected(DisconnectCallback callback) override {
    on_disconnected_ = std::move(callback);
  }

  pd::TransportResult connect() override {
    ++stats_->connects;
    if (behaviour_.connect_fails) {
      return pd::TransportResult::failure(pd::TransportError::ConnectFailed,
                                          behaviour_.connect_error);
    }
    connected_ = true;
    return pd::TransportResult::success();
  }

  pd::TransportResult write(const uint8_t* data, size_t size) override {
    if (!connected_) {
      return pd::TransportResult::failure(pd::TransportError::NotConnected,
                                          "mock transport not connected");
    }
    if (behaviour_.before_write) {
      behaviour_.before_write(data, size);
    }
    size_t accept = size;
    bool fail = false;
    if (behaviour_.fail_write_after_bytes != 0) {
      const size_t already = stats_->bytes.load();
      if (already + size > behaviour_.fail_write_after_bytes) {
        accept = behaviour_.fail_write_after_bytes > already
                     ? behaviour_.fail_write_after_bytes - already
                     : 0;
        fail = true;
      }
    }
    if (accept > 0) {
      ++stats_->writes;
      stats_->bytes += accept;
      const std::vector<uint8_t> response = device_->receive(data, accept);
      if (!response.empty() && on_bytes_) {
        on_bytes_(response.data(), response.size());
      }
    }
    if (fail) {
      connected_ = false;
      if (behaviour_.disconnect_on_write_failure && on_disconnected_) {
        on_disconnected_(pd::TransportError::Closed, "mock link dropped mid-write");
      }
      return pd::TransportResult::failure(pd::TransportError::WriteFailed,
                                          "mock link dropped mid-write", accept);
    }
    return pd::TransportResult::success(size);
  }

  void close() override {
    if (connected_) {
      ++stats_->closes;
    }
    connected_ = false;
  }
  bool isConnected() const override { return connected_; }
  std::string describe() const override { return "mock://fake-printer"; }

 private:
  std::shared_ptr<FakePrinter> device_;
  MockBehaviour behaviour_;
  std::shared_ptr<Stats> stats_;
  BytesCallback on_bytes_;
  DisconnectCallback on_disconnected_;
  bool connected_ = false;
};

struct MockLink {
  std::shared_ptr<FakePrinter> device = std::make_shared<FakePrinter>();
  std::shared_ptr<MockTransport::Stats> stats = std::make_shared<MockTransport::Stats>();
  MockBehaviour behaviour;

  pd::TransportFactory factory() {
    auto device_copy = device;
    auto stats_copy = stats;
    auto behaviour_copy = behaviour;
    return [device_copy, stats_copy, behaviour_copy]() -> std::unique_ptr<pd::Transport> {
      return std::unique_ptr<pd::Transport>(
          new MockTransport(device_copy, behaviour_copy, stats_copy));
    };
  }
};

// --- Real-socket server (for the one end-to-end smoke test) ------------------------

class FakePrinterServer {
 public:
  explicit FakePrinterServer(std::shared_ptr<FakePrinter> device)
      : device_(std::move(device)) {}
  ~FakePrinterServer() { stop(); }

  bool start() {
    listen_socket_ = pd::net::create(AF_INET, SOCK_STREAM, 0);
    if (!pd::net::valid(listen_socket_)) {
      return false;
    }
#if !PD_PLATFORM_WINDOWS
    // Deliberately POSIX-only. Winsock's SO_REUSEADDR is not the POSIX one: it lets a
    // second socket steal a bound address outright rather than reusing a TIME_WAIT
    // one, which on an ephemeral loopback port is a hijack waiting to happen. Windows
    // does not need it for this fixture, so it does not get it.
    pd::net::setIntOption(listen_socket_, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = pd::net::loopbackAddress();
    address.sin_port = 0;
    if (::bind(listen_socket_, reinterpret_cast<sockaddr*>(&address),
               static_cast<pd::net::SockLen>(sizeof(address))) != 0 ||
        ::listen(listen_socket_, 4) != 0) {
      pd::net::closeSocket(listen_socket_);
      listen_socket_ = pd::net::invalidSocket();
      return false;
    }
    pd::net::SockLen length = static_cast<pd::net::SockLen>(sizeof(address));
    if (::getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
      pd::net::closeSocket(listen_socket_);
      listen_socket_ = pd::net::invalidSocket();
      return false;
    }
    port_ = pd::net::fromNetwork16(address.sin_port);
    thread_ = std::thread([this] { serve(); });
    return true;
  }

  uint16_t port() const { return port_; }

  void stop() {
    running_.store(false);
    // Shut both sockets down to wake anything blocked on them, but do not close the
    // listener yet: serve() is still polling on that descriptor, and closing it here
    // frees the number for the OS to reissue mid-poll. It closes after the join.
    if (pd::net::valid(listen_socket_.load())) {
      pd::net::shutdownBoth(listen_socket_.load());
    }
    if (pd::net::valid(client_socket_.load())) {
      pd::net::shutdownBoth(client_socket_.load());
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    const pd::net::Socket listener = listen_socket_.exchange(pd::net::invalidSocket());
    if (pd::net::valid(listener)) {
      pd::net::closeSocket(listener);
    }
  }

 private:
  void serve() {
    while (running_.load()) {
      const pd::net::Socket listener = listen_socket_.load();
      pd::net::PollFd waiter;
      waiter.socket = listener;
      waiter.events = pd::net::kPollIn;
      if (!pd::net::valid(listener) || pd::net::poll(&waiter, 1, 100) <= 0) {
        continue;
      }
      const pd::net::Socket client =
          static_cast<pd::net::Socket>(::accept(listener, nullptr, nullptr));
      if (!pd::net::valid(client)) {
        continue;
      }
      pd::net::setIntOption(client, IPPROTO_TCP, TCP_NODELAY, 1);
      client_socket_.store(client);
      std::vector<uint8_t> buffer(4096);
      while (running_.load()) {
        pd::net::PollFd reader;
        reader.socket = client;
        reader.events = pd::net::kPollIn;
        const int ready = pd::net::poll(&reader, 1, 100);
        if (ready <= 0) {
          continue;
        }
        const int64_t got = pd::net::recvSome(client, buffer.data(), buffer.size());
        if (got <= 0) {
          break;
        }
        const std::vector<uint8_t> response =
            device_->receive(buffer.data(), static_cast<size_t>(got));
        if (!response.empty()) {
          const int64_t ignored =
              pd::net::sendSome(client, response.data(), response.size());
          (void)ignored;
        }
      }
      client_socket_.store(pd::net::invalidSocket());
      pd::net::closeSocket(client);
    }
  }

  std::shared_ptr<FakePrinter> device_;
  // Atomic for the same reason HttpServer's is: stop() retires it while serve() is
  // still polling on it. start() writes it before the thread exists, so the plain
  // conversions up there are single-threaded.
  std::atomic<pd::net::Socket> listen_socket_{pd::net::invalidSocket()};
  std::atomic<pd::net::Socket> client_socket_{pd::net::invalidSocket()};
  uint16_t port_ = 0;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

// --- Test helpers -----------------------------------------------------------------

// Built-in profiles carry production timeouts; tests need the same sequencing with
// budgets they can afford to actually wait out.
inline pd::CapabilityProfile fastProfile(pd::CompletionMechanism mechanism) {
  pd::CapabilityProfile profile = mechanism == pd::CompletionMechanism::GsParenH
                                      ? pd::xp_s260m()
                                      : pd::generic_escpos();
  profile.completion = mechanism;
  profile.chunk_bytes = 0;
  profile.inter_chunk_delay_ms = 0;
  profile.completion_timeout_ms = 200;
  profile.preflight_timeout_ms = 200;
  profile.final_feed_lines = 2;
  profile.code_page = pd::escpos::CodePage::PC437;
  if (mechanism == pd::CompletionMechanism::None) {
    profile.name = "none-profile";
    profile.status.dle_eot = false;
  }
  return profile;
}

// M14. The same trick for the drawer: production profiles watch the switch for 1-2
// seconds after the pulse (docs/cash-drawer.md step 4), which is right on a counter and
// unaffordable in a suite that runs the failure cases too. Everything else — the port
// classification, the method, the cooldown, the polarity — is left to the caller,
// because those are the facts under test.
inline pd::CapabilityProfile drawerProfile(pd::CompletionMechanism mechanism) {
  pd::CapabilityProfile profile = fastProfile(mechanism);
  profile.drawer.present = true;
  profile.drawer.electrical.standard = pd::DrawerPortStandard::Epson24V6P6C;
  profile.drawer.electrical.voltage = 24;
  profile.drawer.electrical.max_current_ma = 1000;
  profile.drawer.electrical.channel_count = 2;
  profile.drawer.electrical.sensor_pin = 3;
  profile.drawer.kick.method = pd::DrawerKickMethod::EpsonEscP;
  profile.drawer.kick.default_pulse_ms = 200;
  profile.drawer.kick.max_pulse_ms = 500;
  profile.drawer.kick.cooldown_ms = 0;
  profile.drawer.status.available = true;
  profile.drawer.status.method = pd::DrawerStatusMethod::GsR2;
  profile.drawer.status.verify_window_ms = 120;
  profile.drawer.status.poll_interval_ms = 10;
  profile.drawer.evidence.electrical = pd::Provenance::Documented;
  profile.drawer.evidence.commands = pd::Provenance::Documented;
  return profile;
}

// Scratch storage directory that removes itself, so a store test leaves nothing
// behind for the next run to load.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
#if PD_PLATFORM_WINDOWS
    const char* base = std::getenv("TEMP");
    if (base == nullptr || base[0] == '\0') {
      base = std::getenv("TMP");
    }
    std::string root =
        base != nullptr && base[0] != '\0' ? std::string(base) : std::string(".");
    const unsigned long pid = ::GetCurrentProcessId();
    if (root.back() != '/' && root.back() != '\\') {
      root += '\\';
    }
#else
    const char* base = std::getenv("TMPDIR");
    std::string root = base != nullptr && base[0] != '\0' ? std::string(base) : "/tmp";
    const long pid = ::getpid();
    if (root.back() != '/') {
      root += '/';
    }
#endif
    path_ = root + "pdtest-" + tag + "-" + std::to_string(pid) + "-" +
            std::to_string(counter++);
  }
  ~TempDir() { remove(); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  // Flat by design: the journal store never creates subdirectories, so a one-level
  // sweep is the whole job on either platform.
  void remove() {
#if PD_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    const HANDLE search = ::FindFirstFileA((path_ + "\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
      return;
    }
    do {
      const std::string name = entry.cFileName;
      if (name == "." || name == "..") {
        continue;
      }
      ::DeleteFileA((path_ + "\\" + name).c_str());
    } while (::FindNextFileA(search, &entry) != 0);
    ::FindClose(search);
    ::RemoveDirectoryA(path_.c_str());
#else
    DIR* dir = ::opendir(path_.c_str());
    if (dir == nullptr) {
      return;
    }
    while (dirent* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      ::unlink((path_ + "/" + name).c_str());
    }
    ::closedir(dir);
    ::rmdir(path_.c_str());
#endif
  }

 private:
  std::string path_;
};

}  // namespace pdfake
