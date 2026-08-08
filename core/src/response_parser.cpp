#include "printerdriver/response_parser.hpp"

#include <algorithm>
#include <utility>

namespace pd::escpos {
namespace {

// GS ( H function 48 response: 37 22 d1 d2 d3 d4 00 (docs/techspec.md §3.1).
constexpr uint8_t kGsHHeader0 = 0x37;
constexpr uint8_t kGsHHeader1 = 0x22;
constexpr uint8_t kGsHTerminator = 0x00;
constexpr size_t kGsHFrameSize = 7;

constexpr size_t kAsbFrameSize = 4;

// Bits 7, 4, 1, 0 — the positions Epson fixes in both status families.
constexpr uint8_t kFixedBitMask = 0x93;
constexpr uint8_t kAsbFixedBits = 0x10;
constexpr uint8_t kRealtimeFixedBits = 0x12;

bool isPrintableAscii(uint8_t value) noexcept { return value >= 0x20 && value <= 0x7E; }

bool bit(uint8_t value, unsigned index) noexcept {
  return (value & (1u << index)) != 0;
}

}  // namespace

const char* to_string(ParsedEventKind kind) noexcept {
  switch (kind) {
    case ParsedEventKind::GsHAck: return "GsHAck";
    case ParsedEventKind::AsbStatus: return "AsbStatus";
    case ParsedEventKind::RealtimeStatus: return "RealtimeStatus";
    case ParsedEventKind::QueuedStatus: return "QueuedStatus";
    case ParsedEventKind::UnknownByte: return "UnknownByte";
  }
  return "UnknownByte";
}

bool looksLikeAsbByte(uint8_t value) noexcept {
  // Deliberately not also checking byte-2's fixed bit 3 or byte-1's fixed bit 5:
  // the uniform test keeps the four-byte scan symmetric, and clone firmware is more
  // likely to deviate on the rarely-exercised bits than on bits 7/4/1/0.
  return (value & kFixedBitMask) == kAsbFixedBits;
}

bool looksLikeRealtimeByte(uint8_t value) noexcept {
  return (value & kFixedBitMask) == kRealtimeFixedBits;
}

StatusFlags decodeAsb(const std::array<uint8_t, 4>& frame) noexcept {
  StatusFlags flags;
  // Byte 0 — printer status.
  flags.drawer_pin_high = bit(frame[0], 2);
  flags.online = !bit(frame[0], 3);
  flags.waiting_online_recovery = bit(frame[0], 5);
  flags.paper_feed_button = bit(frame[0], 6);
  // Byte 1 — offline cause.
  flags.cover_open = bit(frame[1], 2);
  flags.paper_being_fed = bit(frame[1], 3);
  flags.error = bit(frame[1], 6);
  // Byte 2 — error cause. Bit 3 is the autocutter error on Epson-compatible
  // implementations (docs/techspec.md §3).
  flags.cutter_error = bit(frame[2], 3);
  flags.unrecoverable_error = bit(frame[2], 5);
  flags.auto_recoverable_error = bit(frame[2], 6);
  // Byte 3 — paper sensors. Epson sets bits 2 and 3 together for near-end and
  // bits 5 and 6 together for paper-out; testing the lower bit of each pair is
  // enough and tolerates firmware that only drives one of them.
  flags.paper_near_end = bit(frame[3], 2);
  flags.paper_out = bit(frame[3], 5);
  return flags;
}

StatusFlags decodeRealtime(DleEotKind kind, uint8_t value) noexcept {
  StatusFlags flags;
  switch (kind) {
    case DleEotKind::PrinterStatus:
      flags.drawer_pin_high = bit(value, 2);
      flags.online = !bit(value, 3);
      flags.waiting_online_recovery = bit(value, 5);
      flags.paper_feed_button = bit(value, 6);
      break;
    case DleEotKind::OfflineCause:
      flags.cover_open = bit(value, 2);
      flags.paper_being_fed = bit(value, 3);
      flags.paper_out = bit(value, 5);
      flags.error = bit(value, 6);
      break;
    case DleEotKind::ErrorCause:
      flags.cutter_error = bit(value, 3);
      flags.unrecoverable_error = bit(value, 5);
      flags.auto_recoverable_error = bit(value, 6);
      break;
    case DleEotKind::PaperSensor:
      flags.paper_near_end = bit(value, 2);
      flags.paper_out = bit(value, 5);
      break;
  }
  return flags;
}

std::vector<DeviceEvent> toDeviceEvents(const StatusFlags& flags) {
  std::vector<DeviceEvent> events;
  if (flags.online.has_value()) {
    events.push_back(*flags.online ? DeviceEvent::Online : DeviceEvent::Offline);
  }
  if (flags.cover_open.has_value()) {
    events.push_back(*flags.cover_open ? DeviceEvent::CoverOpen
                                       : DeviceEvent::CoverClosed);
  }
  if (flags.paper_out.value_or(false)) {
    events.push_back(DeviceEvent::PaperOut);
  } else if (flags.paper_near_end.value_or(false)) {
    events.push_back(DeviceEvent::PaperNearEnd);
  } else if (flags.paper_out.has_value()) {
    events.push_back(DeviceEvent::PaperOk);
  }
  if (flags.cutter_error.value_or(false)) {
    events.push_back(DeviceEvent::CutterError);
  }
  if (flags.unrecoverable_error.value_or(false)) {
    events.push_back(DeviceEvent::UnrecoverableError);
  }
  if (flags.auto_recoverable_error.value_or(false)) {
    events.push_back(DeviceEvent::RecoverableError);
  }
  // The generic "error occurred" bit is deliberately not lowered to an event: it
  // carries no cause, and inventing one would be exactly the dishonest reporting
  // this SDK exists to remove. The state machine queries DLE EOT 3 instead.
  return events;
}

void ResponseParser::expectRealtime(DleEotKind kind) { realtime_.push_back(kind); }

void ResponseParser::expectQueued() { ++queued_; }

void ResponseParser::reset() noexcept {
  buffer_.clear();
  realtime_.clear();
  queued_ = 0;
}

ResponseParser::Step ResponseParser::step(std::vector<ParsedEvent>& out, bool force) {
  if (buffer_.empty()) {
    return Step::Empty;
  }
  const uint8_t first = buffer_.front();

  // Precedence 1a: the only self-delimiting frame in the stream. Bytes already in
  // hand are checked as soon as they arrive, so an impossible frame is abandoned
  // immediately rather than holding the stream hostage for bytes that would not
  // rescue it.
  if (first == kGsHHeader0) {
    const size_t available = std::min(buffer_.size(), kGsHFrameSize);
    bool disqualified = available >= 2 && buffer_[1] != kGsHHeader1;
    for (size_t i = 2; !disqualified && i < available && i < 6; ++i) {
      disqualified = !isPrintableAscii(buffer_[i]);
    }
    if (!disqualified && available == kGsHFrameSize &&
        buffer_[6] != kGsHTerminator) {
      disqualified = true;
    }
    if (!disqualified) {
      if (buffer_.size() >= kGsHFrameSize) {
        ParsedEvent event;
        event.kind = ParsedEventKind::GsHAck;
        event.token.assign(buffer_.begin() + 2, buffer_.begin() + 6);
        out.push_back(std::move(event));
        buffer_.erase(buffer_.begin(), buffer_.begin() + kGsHFrameSize);
        return Step::Emitted;
      }
      if (!force) {
        return Step::NeedMore;
      }
    }
    // Disqualified: reclassify the leading 0x37 as a single byte, which
    // resynchronises the stream on the byte after it.
  }

  // Precedence 1b: ASB frame. Requires all four bytes to carry the signature, so a
  // single stray 0x10-pattern byte cannot masquerade as a frame — but a genuine
  // single-byte answer that happens to carry it is indistinguishable from a frame
  // that has not fully arrived, and will be held until three more bytes arrive or
  // flush() is called. That ambiguity is inherent to the wire format: nothing
  // delimits these frames.
  if (looksLikeAsbByte(first)) {
    const size_t available = std::min(buffer_.size(), kAsbFrameSize);
    bool disqualified = false;
    for (size_t i = 1; !disqualified && i < available; ++i) {
      disqualified = !looksLikeAsbByte(buffer_[i]);
    }
    if (!disqualified) {
      if (buffer_.size() >= kAsbFrameSize) {
        ParsedEvent event;
        event.kind = ParsedEventKind::AsbStatus;
        for (size_t i = 0; i < kAsbFrameSize; ++i) {
          event.frame[i] = buffer_[i];
        }
        event.flags = decodeAsb(event.frame);
        out.push_back(std::move(event));
        buffer_.erase(buffer_.begin(), buffer_.begin() + kAsbFrameSize);
        return Step::Emitted;
      }
      if (!force) {
        return Step::NeedMore;
      }
    }
  }

  // Precedence 2: real-time answer, but only against an outstanding expectation.
  // Without one, a byte carrying the DLE EOT pattern is not evidence of anything we
  // asked for, so it is not promoted over a waiting queued expectation.
  if (!realtime_.empty() && looksLikeRealtimeByte(first)) {
    ParsedEvent event;
    event.kind = ParsedEventKind::RealtimeStatus;
    event.byte = first;
    event.realtime_kind = realtime_.front();
    event.flags = decodeRealtime(event.realtime_kind, first);
    realtime_.pop_front();
    out.push_back(std::move(event));
    buffer_.pop_front();
    return Step::Emitted;
  }

  // Precedence 3: oldest queued expectation. Pure attribution — the byte itself
  // carries no proof it is a GS r answer.
  if (queued_ > 0) {
    ParsedEvent event;
    event.kind = ParsedEventKind::QueuedStatus;
    event.byte = first;
    --queued_;
    out.push_back(std::move(event));
    buffer_.pop_front();
    return Step::Emitted;
  }

  // Precedence 4: unclassifiable. Reported, never dropped, and the parser is
  // immediately ready for the next byte.
  ParsedEvent event;
  event.kind = ParsedEventKind::UnknownByte;
  event.byte = first;
  out.push_back(std::move(event));
  buffer_.pop_front();
  return Step::Emitted;
}

void ResponseParser::drain(std::vector<ParsedEvent>& out, bool force) {
  while (step(out, force) == Step::Emitted) {
  }
}

std::vector<ParsedEvent> ResponseParser::feed(const uint8_t* data, size_t size) {
  buffer_.insert(buffer_.end(), data, data + size);
  std::vector<ParsedEvent> out;
  drain(out, false);
  return out;
}

std::vector<ParsedEvent> ResponseParser::feed(const std::vector<uint8_t>& data) {
  return feed(data.data(), data.size());
}

std::vector<ParsedEvent> ResponseParser::flush() {
  std::vector<ParsedEvent> out;
  drain(out, true);
  return out;
}

}  // namespace pd::escpos
