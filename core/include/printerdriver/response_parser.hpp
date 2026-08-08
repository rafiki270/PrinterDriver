#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/types.hpp"

// Incremental parser for the printer -> host stream. The stream is genuinely
// interleaved (docs/techspec.md §3.4): GS ( H frames, ASB frames, single-byte
// real-time DLE EOT answers, single-byte queued GS r answers, and vendor bytes
// nobody documented, all on one socket with no framing layer underneath.
//
// Honesty note, because this matters more than the code: only the GS ( H frame is
// self-delimiting. ASB frames and real-time bytes are recognised by Epson's fixed
// bit patterns, which cheap "ESC/POS compatible" clones are not contractually bound
// to honour; queued GS r answers have no usable signature at all and are attributed
// purely from caller expectations. Every such classification is flagged in the
// comments below as HEURISTIC and must be validated per model against real hardware
// (docs/testing-plan.md fault-injection matrix) before a capability profile claims it.

namespace pd::escpos {

enum class ParsedEventKind {
  GsHAck,           // structural: 37 22 <4 printable bytes> 00
  AsbStatus,        // heuristic: four bytes carrying Epson's ASB fixed bits
  RealtimeStatus,   // heuristic: one byte carrying DLE EOT fixed bits, matched to an expectation
  QueuedStatus,     // attributed: one byte handed to the oldest queued expectation
  UnknownByte,      // classified as nothing; never dropped
};

const char* to_string(ParsedEventKind) noexcept;

// Decoded status bits. Every field is optional because each status kind reports a
// different subset: DLE EOT 1 says nothing about the cutter, DLE EOT 3 says nothing
// about the cover, and an absent field means "this frame did not carry it" rather
// than "false".
struct StatusFlags {
  std::optional<bool> online;
  std::optional<bool> cover_open;
  std::optional<bool> paper_out;
  std::optional<bool> paper_near_end;
  std::optional<bool> cutter_error;
  std::optional<bool> unrecoverable_error;
  std::optional<bool> auto_recoverable_error;
  std::optional<bool> error;  // generic "an error occurred" bit, no cause attached
  std::optional<bool> drawer_pin_high;
  std::optional<bool> paper_feed_button;
  std::optional<bool> waiting_online_recovery;
  std::optional<bool> paper_being_fed;
};

struct ParsedEvent {
  ParsedEventKind kind = ParsedEventKind::UnknownByte;
  std::string token;                    // GsHAck: the four-character process ID
  std::array<uint8_t, 4> frame{};       // AsbStatus: the raw four bytes
  uint8_t byte = 0;                     // Realtime/Queued/Unknown: the raw byte
  DleEotKind realtime_kind = DleEotKind::PrinterStatus;  // RealtimeStatus only
  StatusFlags flags;                    // AsbStatus and RealtimeStatus only
};

// Epson fixed-bit signatures. Both use mask 0b1001'0011 (bits 7, 4, 1, 0):
//   ASB byte:      bit 7 = 0, bit 4 = 1, bits 1 and 0 = 0   -> 0x10
//   DLE EOT byte:  bit 7 = 0, bit 4 = 1, bit 1 = 1, bit 0 = 0 -> 0x12
// Bit 1 is what separates them, which is why the two families can coexist on one
// socket at all. The real probe value 0x16 (docs/testing-plan.md) satisfies the
// DLE EOT pattern. HEURISTIC on non-Epson firmware.
bool looksLikeAsbByte(uint8_t value) noexcept;
bool looksLikeRealtimeByte(uint8_t value) noexcept;

StatusFlags decodeAsb(const std::array<uint8_t, 4>& frame) noexcept;
StatusFlags decodeRealtime(DleEotKind kind, uint8_t value) noexcept;

// Lowers decoded bits to the public per-printer event enum (docs/sdk-spec.md §5).
// Only fields the frame actually carried produce events.
std::vector<DeviceEvent> toDeviceEvents(const StatusFlags& flags);

class ResponseParser {
 public:
  // A DLE EOT answer is real-time: it can arrive at any moment, including in the
  // middle of a queued job's data. Nothing in the single response byte identifies
  // which DLE EOT n it answers, so outstanding real-time expectations are consumed
  // oldest-first and the recorded kind is an attribution, not a fact.
  void expectRealtime(DleEotKind kind);

  // Queued answers (GS r 1, and any other queued single-byte status) are strictly
  // FIFO with respect to the command stream, so oldest-first attribution here is
  // sound as long as the caller registers one expectation per queued command.
  void expectQueued();

  std::vector<ParsedEvent> feed(const uint8_t* data, size_t size);
  std::vector<ParsedEvent> feed(const std::vector<uint8_t>& data);

  // Force-classifies bytes still held as a possible incomplete frame. Call it when
  // the read side has gone idle: a lone 0x37, or a lone byte that happens to carry
  // the ASB signature, is otherwise held forever waiting for a frame that will never
  // arrive. Never call it mid-burst — it will shred a genuinely split frame.
  std::vector<ParsedEvent> flush();

  size_t pendingBytes() const noexcept { return buffer_.size(); }
  size_t outstandingRealtime() const noexcept { return realtime_.size(); }
  size_t outstandingQueued() const noexcept { return queued_; }

  // Drops held bytes and expectations. For reconnects, where nothing in flight can
  // still be attributed to anything.
  void reset() noexcept;

 private:
  enum class Step { Emitted, NeedMore, Empty };

  Step step(std::vector<ParsedEvent>& out, bool force);
  void drain(std::vector<ParsedEvent>& out, bool force);

  std::deque<uint8_t> buffer_;
  std::deque<DleEotKind> realtime_;
  size_t queued_ = 0;
};

}  // namespace pd::escpos
