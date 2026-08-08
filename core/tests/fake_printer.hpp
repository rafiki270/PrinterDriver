#pragma once

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
  size_t rasterBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raster_blocks_;
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
        case 0x2D: case 0x64: return 3;             // ESC - / ESC d
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
          ++queued_requests_;
          if (script_.answer_queued_status) {
            out.push_back(script_.queued_status);
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
  size_t queued_requests_ = 0;
  size_t cuts_ = 0;
  size_t drawer_kicks_ = 0;
  size_t raster_blocks_ = 0;
  size_t process_ids_answered_ = 0;
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
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return false;
    }
    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_fd_, 4) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { serve(); });
    return true;
  }

  uint16_t port() const { return port_; }

  void stop() {
    running_.store(false);
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (client_fd_.load() >= 0) {
      ::shutdown(client_fd_.load(), SHUT_RDWR);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void serve() {
    while (running_.load()) {
      pollfd waiter{};
      waiter.fd = listen_fd_;
      waiter.events = POLLIN;
      if (listen_fd_ < 0 || ::poll(&waiter, 1, 100) <= 0) {
        continue;
      }
      const int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      int enabled = 1;
      ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
      client_fd_.store(client);
      std::vector<uint8_t> buffer(4096);
      while (running_.load()) {
        pollfd reader{};
        reader.fd = client;
        reader.events = POLLIN;
        const int ready = ::poll(&reader, 1, 100);
        if (ready <= 0) {
          continue;
        }
        const ssize_t got = ::recv(client, buffer.data(), buffer.size(), 0);
        if (got <= 0) {
          break;
        }
        const std::vector<uint8_t> response =
            device_->receive(buffer.data(), static_cast<size_t>(got));
        if (!response.empty()) {
          ssize_t ignored = ::send(client, response.data(), response.size(), 0);
          (void)ignored;
        }
      }
      client_fd_.store(-1);
      ::close(client);
    }
  }

  std::shared_ptr<FakePrinter> device_;
  int listen_fd_ = -1;
  std::atomic<int> client_fd_{-1};
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
    profile.supports_realtime_status = false;
  }
  return profile;
}

// Scratch storage directory that removes itself, so a store test leaves nothing
// behind for the next run to load.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    const char* base = std::getenv("TMPDIR");
    std::string root = base != nullptr && base[0] != '\0' ? std::string(base) : "/tmp";
    if (root.back() != '/') {
      root += '/';
    }
    path_ = root + "pdtest-" + tag + "-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter++);
  }
  ~TempDir() { remove(); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  void remove() {
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
  }

 private:
  std::string path_;
};

}  // namespace pdfake
