#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "printerdriver/star.hpp"
#include "printerdriver/transport.hpp"

// M13b. A scriptable in-process Star printer (docs/wire-protocols.md §2).
//
// A separate fixture from pdfake::FakePrinter rather than a mode of it, for the same
// reason there are two engines: the two languages disagree about what ESC d means, so one
// scanner that tried to be both would be wrong about at least one of them, and a test
// double that is wrong in the same way as the code under test proves nothing.
//
// It models the one property that makes fencing testable: the command stream is FIFO, so
// a fence answer is produced exactly when the scanner reaches that fence, after everything
// queued ahead of it.

namespace pdfake {

// The inverse of pd::star::etbCounter(): places the five counter bits into the
// non-contiguous positions the ASB byte uses. Written out separately so that the decoder
// under test is never used to construct its own input.
inline uint8_t packEtbCounter(uint8_t counter) {
  return static_cast<uint8_t>(((counter & 0x18u) << 2) | ((counter & 0x07u) << 1));
}

struct StarScript {
  // Answer the ESC GS ETX fence with the documented eight-byte echo.
  bool answer_etx = true;
  // Emit an ASB block when ETB is consumed and ASB is enabled.
  bool answer_etb = true;

  // How far off the correct value the next fence answer's counter should be. 0 is the
  // documented behaviour; anything else models the failure the misattribution guard
  // exists for — a counter that moved for somebody else's data.
  uint8_t etb_counter_skew = 0;

  // Emit one unsolicited ASB frame carrying this counter the first time print data is
  // consumed, i.e. **before any fence is outstanding**. This is the TCP 9100 broadcast of
  // docs/wire-protocols.md §2: another host's job finishing on a printer we are also
  // connected to.
  bool broadcast_foreign_asb = false;
  uint8_t foreign_counter = 9;

  // Stop answering fences after this many have been served; 0 = never.
  size_t fence_answer_limit = 0;

  uint8_t asb_block_bytes = 8;
};

class FakeStarPrinter {
 public:
  explicit FakeStarPrinter(StarScript script = {}) : script_(script) {}

  void setScript(const StarScript& script) {
    std::lock_guard<std::mutex> lock(mutex_);
    script_ = script;
  }

  std::vector<uint8_t> receive(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    std::lock_guard<std::mutex> lock(mutex_);
    received_.insert(received_.end(), data, data + size);
    pending_.insert(pending_.end(), data, data + size);
    scan(out);
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
  size_t cuts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cuts_;
  }
  size_t etbFences() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return etb_fences_;
  }
  size_t etxFences() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return etx_fences_;
  }
  size_t asbEnables() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return asb_enables_;
  }
  size_t counterClears() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counter_clears_;
  }
  size_t rasterRows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raster_rows_;
  }
  uint8_t etbCounter() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return etb_counter_;
  }

  bool receivedContains(const std::vector<uint8_t>& needle) const {
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
  void emitAsb(std::vector<uint8_t>& out, uint8_t counter) {
    std::vector<uint8_t> block(script_.asb_block_bytes, 0x00);
    if (block.size() > 7) {
      block[7] = packEtbCounter(counter);
    }
    out.insert(out.end(), block.begin(), block.end());
  }

  bool fenceExhausted() const {
    return script_.fence_answer_limit != 0 && fences_answered_ >= script_.fence_answer_limit;
  }

  void scan(std::vector<uint8_t>& out) {
    size_t offset = 0;
    while (offset < pending_.size()) {
      const uint8_t first = pending_[offset];

      if (first == pd::star::kEtb) {
        ++etb_fences_;
        etb_counter_ = static_cast<uint8_t>(
            (etb_counter_ + 1u + script_.etb_counter_skew) & 0x1Fu);
        if (script_.answer_etb && asb_enabled_ && !fenceExhausted()) {
          ++fences_answered_;
          emitAsb(out, etb_counter_);
        }
        offset += 1;
        continue;
      }

      if (first == 0x1B && offset + 1 < pending_.size()) {
        const uint8_t second = pending_[offset + 1];
        if (second == 0x1E) {  // ESC RS ... — the ASB family
          if (offset + 3 >= pending_.size()) {
            break;
          }
          const uint8_t third = pending_[offset + 2];
          if (third == 0x61) {  // ESC RS a n
            if (pending_[offset + 3] != 0) {
              ++asb_enables_;
              asb_enabled_ = true;
            } else {
              asb_enabled_ = false;
            }
            offset += 4;
            continue;
          }
          if (third == 0x45) {  // ESC RS E 0 — clear the ETB counter and status
            ++counter_clears_;
            etb_counter_ = 0;
            offset += 4;
            continue;
          }
        }
        if (second == 0x1D && offset + 2 < pending_.size() && pending_[offset + 2] == 0x03) {
          if (offset + 6 > pending_.size()) {
            break;  // 1B 1D 03 01 n1 n2
          }
          ++etx_fences_;
          etx_counter_ = static_cast<uint8_t>(etx_counter_ + 1u);
          if (script_.answer_etx && !fenceExhausted()) {
            ++fences_answered_;
            out.push_back(0x1B);
            out.push_back(0x1D);
            out.push_back(0x03);
            out.push_back(0x01);
            out.push_back(pending_[offset + 4]);
            out.push_back(pending_[offset + 5]);
            out.push_back(etx_counter_);
            out.push_back(0x00);
          }
          offset += 6;
          continue;
        }
        if (second == 0x1D && offset + 3 < pending_.size() && pending_[offset + 2] == 0x61) {
          offset += 4;  // ESC GS a n — alignment
          continue;
        }
        if (second == 0x40) {  // ESC @
          offset += 2;
          continue;
        }
        if (second == 0x45 || second == 0x46) {  // ESC E / ESC F
          offset += 2;
          continue;
        }
        if (second == 0x2D || second == 0x61 || second == 0x4A) {  // ESC - / ESC a / ESC J
          if (offset + 2 >= pending_.size()) {
            break;
          }
          offset += 3;
          continue;
        }
        if (second == 0x64) {  // ESC d n — cut
          if (offset + 2 >= pending_.size()) {
            break;
          }
          ++cuts_;
          offset += 3;
          continue;
        }
        if (second == 0x69) {  // ESC i n1 n2 — character expansion
          if (offset + 3 >= pending_.size()) {
            break;
          }
          offset += 4;
          continue;
        }
        if (second == 0x2A) {  // ESC * r A | ESC * r B
          if (offset + 3 >= pending_.size()) {
            break;
          }
          offset += 4;
          continue;
        }
      }

      if (first == 0x62) {  // b n1 n2 <row>
        if (offset + 3 > pending_.size()) {
          break;
        }
        const size_t length = static_cast<size_t>(pending_[offset + 1]) |
                              (static_cast<size_t>(pending_[offset + 2]) << 8);
        if (offset + 3 + length > pending_.size()) {
          break;
        }
        ++raster_rows_;
        offset += 3 + length;
        continue;
      }

      if ((first == 0x1B) && offset + 1 >= pending_.size()) {
        break;  // escape byte with its selector still in flight
      }

      if (script_.broadcast_foreign_asb && !foreign_emitted_) {
        // The broadcast arrives with the payload, i.e. while nothing of ours is
        // outstanding. A driver that treats any counter change as its own completion
        // finishes the job here, on somebody else's receipt.
        foreign_emitted_ = true;
        emitAsb(out, script_.foreign_counter);
      }
      print_data_.push_back(first);
      offset += 1;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(offset));
  }

  mutable std::mutex mutex_;
  StarScript script_;
  std::vector<uint8_t> received_;
  std::vector<uint8_t> pending_;
  std::vector<uint8_t> print_data_;
  bool asb_enabled_ = false;
  bool foreign_emitted_ = false;
  uint8_t etb_counter_ = 0;
  uint8_t etx_counter_ = 0;
  size_t etb_fences_ = 0;
  size_t etx_fences_ = 0;
  size_t asb_enables_ = 0;
  size_t counter_clears_ = 0;
  size_t cuts_ = 0;
  size_t raster_rows_ = 0;
  size_t fences_answered_ = 0;
};

// Same synchronous-delivery contract as pdfake::MockTransport: the answer is produced by
// the scan or it is never coming, which removes every timing race from the engine tests.
class StarMockTransport : public pd::Transport {
 public:
  explicit StarMockTransport(std::shared_ptr<FakeStarPrinter> device)
      : device_(std::move(device)) {}

  void onBytes(BytesCallback callback) override { on_bytes_ = std::move(callback); }
  void onDisconnected(DisconnectCallback callback) override {
    on_disconnected_ = std::move(callback);
  }

  pd::TransportResult connect() override {
    connected_ = true;
    return pd::TransportResult::success();
  }

  pd::TransportResult write(const uint8_t* data, size_t size) override {
    if (!connected_) {
      return pd::TransportResult::failure(pd::TransportError::NotConnected,
                                          "star mock transport not connected");
    }
    const std::vector<uint8_t> response = device_->receive(data, size);
    if (!response.empty() && on_bytes_) {
      on_bytes_(response.data(), response.size());
    }
    return pd::TransportResult::success(size);
  }

  void close() override { connected_ = false; }
  bool isConnected() const override { return connected_; }
  std::string describe() const override { return "mock://fake-star-printer"; }

 private:
  std::shared_ptr<FakeStarPrinter> device_;
  BytesCallback on_bytes_;
  DisconnectCallback on_disconnected_;
  bool connected_ = false;
};

struct StarMockLink {
  std::shared_ptr<FakeStarPrinter> device = std::make_shared<FakeStarPrinter>();

  pd::TransportFactory factory() {
    auto device_copy = device;
    return [device_copy]() -> std::unique_ptr<pd::Transport> {
      return std::unique_ptr<pd::Transport>(new StarMockTransport(device_copy));
    };
  }
};

}  // namespace pdfake
