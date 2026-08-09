#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "printerdriver/escpos_encoder.hpp"

// M13b. Star without the SDK (docs/wire-protocols.md §2).
//
// Star is not an ESC/POS clone and this file does not pretend otherwise
// (docs/compatibility-brief.md §7-§8). It is a separate command language with its own
// completion primitives, and the reason it can be driven here at all is that both of
// those primitives are documented at the byte level in Star's own Line Mode Command
// Specifications — unlike beginCheckedBlock()/endCheckedBlock(), which are SDK calls
// whose underlying wire behaviour must not be assumed per emulation and interface.
//
// -- The two fences, and why the default is the second one ---------------------------
//
//   * **ETB** (0x17). On consume the printer waits for all preceding printing, then
//     increments a five-bit counter, sets ETB status and emits an ASB frame if ASB is
//     enabled. The counter is genuine device completion. What makes it dangerous is the
//     delivery path: **on TCP 9100 the ASB frame is broadcast to every connected host**,
//     so with two clients on one printer each sees the other's increments and can read
//     somebody else's finished receipt as its own. There is no way to tell the two apart
//     from inside one client, so ETB is permitted only where the topology rules it out —
//     serial, USB, or a 9100 session this driver is enforcing exclusively.
//
//   * **ESC GS ETX** (1B 1D 03 01 n1 n2). Star's preferred Ethernet fence. It waits for
//     prior printing and motor activity in the same way, carries an eight-bit print-end
//     counter, echoes back the two correlation bytes it was handed, and — the property
//     that matters — **replies only to the issuing session**. Correlated and private, so
//     it is the default everywhere the exclusivity of the socket is not established.
//
// Everything here is a pure encoder/decoder: no socket, no timing, no state machine, so
// every byte is testable as a golden buffer and every decode is testable as a table.

namespace pd::star {

using escpos::Bytes;

// --- Fence primitives ---------------------------------------------------------------

// ETB, the fence byte itself.
inline constexpr uint8_t kEtb = 0x17;

// 0x17. Meaningful only with ASB enabled: the counter it increments is reported through
// the ASB frame and nowhere else.
Bytes etbFence();

// 1B 1E 61 01 / 1B 1E 61 00 — automatic status back on and off.
Bytes asbEnable();
Bytes asbDisable();
// 1B 06 01 — ask for a status frame right now, outside the automatic triggers.
Bytes asbRequestNow();
// 1B 1E 45 00 — clear the ETB counter and the ETB status bit. Issued at the start of a
// session so the first fence has a known baseline instead of whatever the last owner of
// the printer left behind.
Bytes clearEtbCounter();

// The five-bit ETB counter, unpacked from "printer status 6" — ASB byte offset 7 — where
// it is stored in **non-contiguous bits**: ASB bit6 -> counter bit4, bit5 -> 3, bit3 -> 2,
// bit2 -> 1, bit1 -> 0 (docs/wire-protocols.md §2). The gaps are not a mistake in the
// documentation; the intervening bits carry other status, which is why this cannot be a
// shift-and-mask of one contiguous field.
uint8_t etbCounter(uint8_t status_byte) noexcept;

// Wraps 31 -> 0. A fence expects exactly this value, so the wrap has to be modelled
// rather than left to an int that quietly reaches 32.
uint8_t nextEtbCounter(uint8_t counter) noexcept;

// 1B 1D 03 01 n1 n2. n1/n2 are the caller's correlation bytes and come back verbatim in
// the answer, which is what turns "a counter changed" into "the data *I* sent finished".
Bytes escGsEtxFence(uint8_t n1, uint8_t n2);

// 1B 1D 03 01 n1 n2 counter 00 — eight bytes, self-delimiting.
inline constexpr size_t kEscGsEtxResponseBytes = 8;

// --- Response parsing -----------------------------------------------------------------

enum class EventKind {
  // A complete ESC GS ETX answer: correlation bytes echoed, print-end counter attached.
  // Structural, not heuristic — the four-byte prefix and the fixed length delimit it.
  EtxAck,
  // One ASB block. Only the ETB counter is decoded from it: docs/wire-protocols.md §2
  // pins down byte offset 7 and the bit packing and nothing else, so claiming to read
  // cover or paper state out of the remaining bytes would be invention. The raw block is
  // carried so a caller that has a per-model ASB map can decode the rest itself.
  AsbStatus,
  // Classified as nothing, and never dropped.
  UnknownByte,
};

struct Event {
  EventKind kind = EventKind::UnknownByte;
  uint8_t n1 = 0;        // EtxAck: the echoed correlation bytes
  uint8_t n2 = 0;
  uint8_t counter = 0;   // EtxAck: the eight-bit print-end counter
                         // AsbStatus: the five-bit ETB counter, already unpacked
  uint8_t byte = 0;      // UnknownByte: the raw byte
  Bytes frame;           // AsbStatus: the whole block, undecoded
};

const char* to_string(EventKind) noexcept;

// Incremental, like the ESC/POS parser and for the same reason: the stream is one socket
// with no framing layer, and an answer can be split across reads.
class ResponseParser {
 public:
  // How many bytes one ASB block occupies. The counter is at offset 7, which only means
  // anything against a known block length, so the length is configuration carried by the
  // capability profile rather than something guessed from the stream. Zero disables ASB
  // decoding entirely, which is the right setting when only ESC GS ETX is in use.
  void setAsbBlockBytes(uint8_t bytes) noexcept { asb_block_bytes_ = bytes; }
  uint8_t asbBlockBytes() const noexcept { return asb_block_bytes_; }

  std::vector<Event> feed(const uint8_t* data, size_t size);
  std::vector<Event> feed(const Bytes& data);

  // Force-classifies bytes still held as a possible incomplete frame. For an idle read
  // side; never call it mid-burst, because it will shred a genuinely split answer.
  std::vector<Event> flush();

  size_t pendingBytes() const noexcept { return buffer_.size(); }
  void reset() noexcept;

 private:
  enum class Step { Emitted, NeedMore, Empty };

  Step step(std::vector<Event>& out, bool force);

  std::deque<uint8_t> buffer_;
  uint8_t asb_block_bytes_ = 8;
};

// --- Line Mode encoder ----------------------------------------------------------------
//
// The minimal documented subset that a receipt needs, and no more. What is deliberately
// absent is listed on `declaredDegradations()` below rather than silently missing: a
// capability nobody implemented and a capability nobody wrote down are the same bug from
// the caller's side, and this project's whole position is that they must not be.

enum class Cut {
  Partial,  // ESC d 3 — partial cut, feeding to the cut position first
  Full,     // ESC d 2 — full cut, feeding to the cut position first
};

class Encoder {
 public:
  Encoder() = default;

  // ESC @ — back to the documented power-on state, so a job never inherits the styling
  // of whatever ran before it.
  Encoder& initialize();

  // ESC GS a n — 0 left, 1 centre, 2 right.
  Encoder& align(escpos::Alignment alignment);
  // ESC E / ESC F.
  Encoder& bold(bool enabled);
  // ESC - n, 0 or 1.
  Encoder& underline(bool enabled);

  // ASCII only, and lossy on purpose: anything above 0x7F becomes '?'. Star's character
  // set selection is a per-model table this core does not carry, and a receipt with a
  // '?' in a customer name still prints, while a guessed code page prints a different
  // wrong character in every position. Declared as a degradation, not hidden.
  Encoder& text(std::string_view utf8);
  Encoder& line(std::string_view utf8 = {});
  Encoder& feed();               // LF
  Encoder& feedLines(uint8_t lines);  // ESC a n
  // ESC J n — feed n units without printing. Star's unit here is 0.125 mm and a dot at
  // 203 dpi is 0.125 mm, so on the whole deployed fleet this is a one-for-one match with
  // the ESC/POS spelling of the same command and the caller's margin in dots means the
  // same distance on both engines. Requests above 255 are split across several commands.
  Encoder& feedDots(uint16_t dots);

  Encoder& cut(Cut mode = Cut::Partial);

  // ESC * r A ... b n1 n2 <row> ... ESC * r B. One `b` block per raster row, MSB-first,
  // 1 = dot printed — the same packing the ESC/POS raster path produces, so the shared
  // packGrayscale() output feeds both engines unchanged.
  //
  // Documented-provisional (docs/wire-protocols.md verdict table): this is the raster
  // path Star's Line Mode specification describes, and it is the one place in this
  // encoder that has not been confirmed against hardware in this repository. A profile
  // that would rather not risk it sets StarCapabilities::raster_line_mode false and gets
  // an honest Unsupported instead.
  Encoder& rasterPacked(const uint8_t* data, size_t size, uint32_t width_dots,
                        uint32_t height_dots);
  Encoder& rasterGrayscale(const uint8_t* gray, uint32_t width, uint32_t height,
                           uint32_t target_width_dots, escpos::Binarization binarization,
                           uint8_t threshold = 128);

  Encoder& raw(const uint8_t* data, size_t size);
  Encoder& raw(const Bytes& data);

  const Bytes& bytes() const noexcept { return buffer_; }
  Bytes take() noexcept {
    Bytes out = std::move(buffer_);
    buffer_.clear();
    return out;
  }
  size_t size() const noexcept { return buffer_.size(); }

 private:
  void put(std::initializer_list<uint8_t> data);

  Bytes buffer_;
  escpos::Alignment alignment_ = escpos::Alignment::Left;
  bool bold_ = false;
  bool underline_ = false;
};

// UTF-8 in, 7-bit ASCII out; unmapped code points and malformed input become '?'.
Bytes transliterateAscii(std::string_view utf8);

// --- ESC/POS -> Star Line Mode transcoding ---------------------------------------------
//
// The document tier of docs/api.md §3 is bytes produced by escpos::Encoder, and the
// receipt DSL renders into exactly the same encoder. Without this, every document in the
// system would be unprintable on a Star device and the honest answer would be a permanent
// Unsupported — which is a correct answer and a useless one.
//
// The transcoder works because the input is not "arbitrary ESC/POS": it is the closed,
// conservative subset **this core itself emits** (docs/sdk-spec.md §3), so every command
// it can meet is known in advance. Commands with a Star equivalent are rewritten;
// commands without one are DROPPED AND NAMED. Nothing is guessed, and nothing silently
// disappears — `TranscodeResult::dropped` is what the job's declared degradation is built
// from, so a caller learns that the QR on their receipt did not print rather than
// discovering it on the counter.
//
// Byte sequences the subset does not contain are passed through unchanged and counted, so
// a Raw payload that was handed to the wrong engine shows up as a large unrecognised
// count instead of as confetti nobody can explain.

struct TranscodeOptions {
  // False makes an image an honest drop instead of the documented-provisional raster
  // command sequence (see Encoder::rasterPacked).
  bool raster_line_mode = true;
};

struct TranscodeResult {
  Bytes bytes;
  // Human-readable, one per distinct kind of thing that did not survive, e.g.
  // "QR code (GS ( k)". Deduplicated: five dropped QR codes are one line, not five.
  std::vector<std::string> dropped;
  // Bytes that matched no command in the subset and were passed through verbatim.
  size_t unrecognised_bytes = 0;
};

TranscodeResult transcodeFromEscPos(const uint8_t* data, size_t size,
                                    const TranscodeOptions& options = {});
TranscodeResult transcodeFromEscPos(const Bytes& data,
                                    const TranscodeOptions& options = {});

// What this engine does NOT do on a Star printer, in the words a caller needs to read
// before choosing it. Returned as data so `pdctl` and the agent can print it verbatim
// rather than each maintaining its own list that drifts from the code.
const std::vector<std::string>& declaredDegradations();

}  // namespace pd::star
