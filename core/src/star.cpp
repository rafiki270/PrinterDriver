#include "printerdriver/star.hpp"

#include <algorithm>

namespace pd::star {
namespace {

// The four bytes that open an ESC GS ETX answer. Matching on the whole prefix rather
// than on ESC alone is what keeps the frame apart from an ASB block: an ASB status byte
// cannot be 0x1B (its bit 0 is fixed clear), so the two families never collide on the
// first byte, and the remaining three bytes make a false positive vanishingly unlikely.
constexpr uint8_t kEtxPrefix[4] = {0x1B, 0x1D, 0x03, 0x01};

}  // namespace

// --- Fence primitives ---------------------------------------------------------------

Bytes etbFence() { return Bytes{kEtb}; }

Bytes asbEnable() { return Bytes{0x1B, 0x1E, 0x61, 0x01}; }

Bytes asbDisable() { return Bytes{0x1B, 0x1E, 0x61, 0x00}; }

Bytes asbRequestNow() { return Bytes{0x1B, 0x06, 0x01}; }

Bytes clearEtbCounter() { return Bytes{0x1B, 0x1E, 0x45, 0x00}; }

uint8_t etbCounter(uint8_t status_byte) noexcept {
  // docs/wire-protocols.md §2, verbatim. ASB bit6 -> counter bit4, bit5 -> 3, bit3 -> 2,
  // bit2 -> 1, bit1 -> 0. Written as the documented two-term expression rather than
  // "simplified", because the simplification is exactly what a reader would have to
  // verify against the specification again.
  return static_cast<uint8_t>(((status_byte & 0x60) >> 2) | ((status_byte & 0x0E) >> 1));
}

uint8_t nextEtbCounter(uint8_t counter) noexcept {
  return static_cast<uint8_t>((counter + 1u) & 0x1Fu);
}

Bytes escGsEtxFence(uint8_t n1, uint8_t n2) {
  return Bytes{0x1B, 0x1D, 0x03, 0x01, n1, n2};
}

// --- Response parsing -----------------------------------------------------------------

const char* to_string(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::EtxAck: return "EtxAck";
    case EventKind::AsbStatus: return "AsbStatus";
    case EventKind::UnknownByte: return "UnknownByte";
  }
  return "UnknownByte";
}

ResponseParser::Step ResponseParser::step(std::vector<Event>& out, bool force) {
  if (buffer_.empty()) {
    return Step::Empty;
  }

  // ESC GS ETX answer. Held rather than classified until the whole eight bytes are in:
  // a split read must not be shredded into an ESC plus seven unknown bytes.
  if (buffer_[0] == kEtxPrefix[0]) {
    const size_t known = std::min<size_t>(buffer_.size(), 4u);
    for (size_t i = 0; i < known; ++i) {
      if (buffer_[i] != kEtxPrefix[i]) {
        // A leading ESC that is not this frame. Emit the byte and re-scan from the next
        // one, which is the only way a stray ESC cannot wedge the parser forever.
        Event event;
        event.kind = EventKind::UnknownByte;
        event.byte = buffer_.front();
        buffer_.pop_front();
        out.push_back(event);
        return Step::Emitted;
      }
    }
    if (buffer_.size() < kEscGsEtxResponseBytes) {
      if (!force) {
        return Step::NeedMore;
      }
      Event event;
      event.kind = EventKind::UnknownByte;
      event.byte = buffer_.front();
      buffer_.pop_front();
      out.push_back(event);
      return Step::Emitted;
    }
    Event event;
    event.kind = EventKind::EtxAck;
    event.n1 = buffer_[4];
    event.n2 = buffer_[5];
    event.counter = buffer_[6];
    // buffer_[7] is the documented trailing 00. It is not asserted on: a device that
    // terminates the frame differently has still answered the correlated question, and
    // refusing the answer over its last byte would trade a confirmed receipt for a
    // pedantic Unknown.
    for (size_t i = 0; i < kEscGsEtxResponseBytes; ++i) {
      buffer_.pop_front();
    }
    out.push_back(event);
    return Step::Emitted;
  }

  // ASB block. Fixed length by profile, because the one field this core decodes lives at
  // a fixed offset and an offset without a length is not a decode.
  if (asb_block_bytes_ > 0) {
    if (buffer_.size() < asb_block_bytes_) {
      if (!force) {
        return Step::NeedMore;
      }
      Event event;
      event.kind = EventKind::UnknownByte;
      event.byte = buffer_.front();
      buffer_.pop_front();
      out.push_back(event);
      return Step::Emitted;
    }
    Event event;
    event.kind = EventKind::AsbStatus;
    event.frame.reserve(asb_block_bytes_);
    for (uint8_t i = 0; i < asb_block_bytes_; ++i) {
      event.frame.push_back(buffer_.front());
      buffer_.pop_front();
    }
    // Offset 7 is "printer status 6". A block shorter than eight bytes carries no
    // counter, so the event reports the block and leaves the counter at its zero value
    // rather than reading past the end of what the profile declared.
    if (event.frame.size() > 7) {
      event.counter = etbCounter(event.frame[7]);
    }
    out.push_back(event);
    return Step::Emitted;
  }

  Event event;
  event.kind = EventKind::UnknownByte;
  event.byte = buffer_.front();
  buffer_.pop_front();
  out.push_back(event);
  return Step::Emitted;
}

std::vector<Event> ResponseParser::feed(const uint8_t* data, size_t size) {
  std::vector<Event> out;
  if (data != nullptr) {
    for (size_t i = 0; i < size; ++i) {
      buffer_.push_back(data[i]);
    }
  }
  for (;;) {
    const Step result = step(out, false);
    if (result != Step::Emitted) {
      break;
    }
  }
  return out;
}

std::vector<Event> ResponseParser::feed(const Bytes& data) {
  return feed(data.data(), data.size());
}

std::vector<Event> ResponseParser::flush() {
  std::vector<Event> out;
  for (;;) {
    const Step result = step(out, true);
    if (result == Step::Empty) {
      break;
    }
  }
  return out;
}

void ResponseParser::reset() noexcept { buffer_.clear(); }

// --- Line Mode encoder ----------------------------------------------------------------

Bytes transliterateAscii(std::string_view utf8) {
  Bytes out;
  out.reserve(utf8.size());
  for (const char c : utf8) {
    const uint8_t value = static_cast<uint8_t>(c);
    // Everything above 7-bit ASCII collapses to '?'. Lossy and never an error: a receipt
    // that prints with a '?' is a receipt, and a thrown exception is a lost ticket.
    out.push_back(value < 0x80u ? value : escpos::kUnmappedCodepointFallback);
  }
  return out;
}

void Encoder::put(std::initializer_list<uint8_t> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

Encoder& Encoder::initialize() {
  put({0x1B, 0x40});
  alignment_ = escpos::Alignment::Left;
  bold_ = false;
  underline_ = false;
  return *this;
}

Encoder& Encoder::align(escpos::Alignment alignment) {
  if (alignment == alignment_) {
    return *this;
  }
  alignment_ = alignment;
  // ESC GS a n — Star's alignment command. Not ESC a n: that is the line-feed command
  // here, and sending the ESC/POS spelling to a Star printer feeds paper instead of
  // centring text, which is the whole reason these two engines are separate.
  put({0x1B, 0x1D, 0x61, static_cast<uint8_t>(alignment)});
  return *this;
}

Encoder& Encoder::bold(bool enabled) {
  if (enabled == bold_) {
    return *this;
  }
  bold_ = enabled;
  put({0x1B, enabled ? static_cast<uint8_t>(0x45) : static_cast<uint8_t>(0x46)});
  return *this;
}

Encoder& Encoder::underline(bool enabled) {
  if (enabled == underline_) {
    return *this;
  }
  underline_ = enabled;
  put({0x1B, 0x2D, enabled ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x00)});
  return *this;
}

Encoder& Encoder::text(std::string_view utf8) {
  const Bytes encoded = transliterateAscii(utf8);
  buffer_.insert(buffer_.end(), encoded.begin(), encoded.end());
  return *this;
}

Encoder& Encoder::line(std::string_view utf8) {
  text(utf8);
  buffer_.push_back(0x0A);
  return *this;
}

Encoder& Encoder::feed() {
  buffer_.push_back(0x0A);
  return *this;
}

Encoder& Encoder::feedLines(uint8_t lines) {
  if (lines == 0) {
    return *this;
  }
  // ESC a n — feed n lines. The ESC/POS spelling of this is ESC d n, which on Star is
  // the *cut* command. Two commands that mean the opposite thing under the same bytes is
  // the sharpest possible argument against one shared encoder.
  put({0x1B, 0x61, lines});
  return *this;
}

Encoder& Encoder::feedDots(uint16_t dots) {
  uint16_t remaining = dots;
  while (remaining > 0) {
    const uint8_t step = remaining > 255 ? static_cast<uint8_t>(255)
                                         : static_cast<uint8_t>(remaining);
    put({0x1B, 0x4A, step});
    remaining = static_cast<uint16_t>(remaining - step);
  }
  return *this;
}

Encoder& Encoder::cut(Cut mode) {
  // ESC d n. 2 = full cut with feed to the cut position, 3 = partial cut with feed. The
  // feeding variants are the ones used here on purpose: the head sits ahead of the
  // blade, and the non-feeding variants slice the last printed line — the same geometry
  // that ate a trailing QR on the ESC/POS side (docs/testing-plan.md), solved by the
  // mechanism itself rather than by a measured dot count.
  put({0x1B, 0x64, mode == Cut::Full ? static_cast<uint8_t>(0x02)
                                     : static_cast<uint8_t>(0x03)});
  return *this;
}

Encoder& Encoder::rasterPacked(const uint8_t* data, size_t size, uint32_t width_dots,
                               uint32_t height_dots) {
  if (data == nullptr || width_dots == 0 || height_dots == 0) {
    return *this;
  }
  const size_t bytes_per_row = (static_cast<size_t>(width_dots) + 7u) / 8u;
  if (bytes_per_row == 0 || size < bytes_per_row * height_dots) {
    return *this;
  }
  // ESC * r A — enter raster mode.
  put({0x1B, 0x2A, 0x72, 0x41});
  for (uint32_t row = 0; row < height_dots; ++row) {
    // b n1 n2 d1..dk, k = n1 + n2 * 256. One command per row: rows are short, and a
    // per-row command keeps the length field far away from the 16-bit ceiling that a
    // whole-image block would approach on an 80 mm receipt.
    buffer_.push_back(0x62);
    buffer_.push_back(static_cast<uint8_t>(bytes_per_row & 0xFFu));
    buffer_.push_back(static_cast<uint8_t>((bytes_per_row >> 8) & 0xFFu));
    const uint8_t* source = data + static_cast<size_t>(row) * bytes_per_row;
    buffer_.insert(buffer_.end(), source, source + bytes_per_row);
  }
  // ESC * r B — quit raster mode.
  put({0x1B, 0x2A, 0x72, 0x42});
  return *this;
}

Encoder& Encoder::rasterGrayscale(const uint8_t* gray, uint32_t width, uint32_t height,
                                  uint32_t target_width_dots,
                                  escpos::Binarization binarization, uint8_t threshold) {
  if (gray == nullptr || width == 0 || height == 0 || target_width_dots == 0) {
    return *this;
  }
  // Deliberately the ESC/POS engine's packer. Scaling and binarisation are arithmetic,
  // not dialect: sharing them means the same source image produces the same dots on both
  // engines, and any difference between a Star receipt and an Epson one is the command
  // layer alone.
  const escpos::PackedRaster packed = escpos::packGrayscale(
      gray, width, height, target_width_dots, binarization, threshold);
  return rasterPacked(packed.data.data(), packed.data.size(), packed.width_dots,
                      packed.height_dots);
}

Encoder& Encoder::raw(const uint8_t* data, size_t size) {
  if (data != nullptr && size > 0) {
    buffer_.insert(buffer_.end(), data, data + size);
  }
  return *this;
}

Encoder& Encoder::raw(const Bytes& data) { return raw(data.data(), data.size()); }

// --- ESC/POS -> Star Line Mode transcoding ---------------------------------------------

namespace {

void note(std::vector<std::string>& dropped, const char* what) {
  const std::string entry(what);
  if (std::find(dropped.begin(), dropped.end(), entry) == dropped.end()) {
    dropped.push_back(entry);
  }
}

uint16_t le16(uint8_t low, uint8_t high) noexcept {
  return static_cast<uint16_t>(low | (static_cast<uint16_t>(high) << 8));
}

}  // namespace

TranscodeResult transcodeFromEscPos(const uint8_t* data, size_t size,
                                    const TranscodeOptions& options) {
  TranscodeResult result;
  if (data == nullptr || size == 0) {
    return result;
  }
  Encoder out;
  size_t i = 0;

  // `have(n)` guards every multi-byte read. A truncated command at the end of the buffer
  // is passed through as unrecognised bytes rather than read past the end: the input is
  // a payload, and a payload can be malformed.
  const auto have = [&](size_t n) { return i + n <= size; };

  while (i < size) {
    const uint8_t first = data[i];

    if (first == 0x1B && have(2)) {
      const uint8_t second = data[i + 1];
      switch (second) {
        case 0x40:  // ESC @ — initialise. Same spelling on both.
          out.raw(data + i, 2);
          i += 2;
          continue;
        case 0x74:  // ESC t n — select code page. No Star equivalent is carried here.
          if (have(3)) {
            note(result.dropped,
                 "code page selection (ESC t): Star text is transcoded as 7-bit ASCII");
            i += 3;
            continue;
          }
          break;
        case 0x61:  // ESC a n — ESC/POS alignment.
          if (have(3)) {
            // Star spells alignment ESC GS a n. Sending the ESC/POS bytes through would
            // feed paper instead, because ESC a n is Star's *line feed*.
            out.raw(Bytes{0x1B, 0x1D, 0x61, data[i + 2]});
            i += 3;
            continue;
          }
          break;
        case 0x45:  // ESC E n — emphasis.
          if (have(3)) {
            out.raw(Bytes{0x1B, data[i + 2] != 0 ? static_cast<uint8_t>(0x45)
                                                 : static_cast<uint8_t>(0x46)});
            i += 3;
            continue;
          }
          break;
        case 0x2D:  // ESC - n — underline. Same spelling, and Star takes 0 or 1.
          if (have(3)) {
            out.raw(Bytes{0x1B, 0x2D, data[i + 2] != 0 ? static_cast<uint8_t>(0x01)
                                                       : static_cast<uint8_t>(0x00)});
            i += 3;
            continue;
          }
          break;
        case 0x64:  // ESC d n — ESC/POS "feed n lines".
          if (have(3)) {
            // ESC d on Star is the *cut* command. This single collision is the clearest
            // possible argument for transcoding rather than hoping an emulation copes.
            out.raw(Bytes{0x1B, 0x61, data[i + 2]});
            i += 3;
            continue;
          }
          break;
        case 0x4A:  // ESC J n — feed n dots. One-for-one: both units are 0.125 mm.
          if (have(3)) {
            out.raw(data + i, 3);
            i += 3;
            continue;
          }
          break;
        case 0x70:  // ESC p m t1 t2 — cash drawer.
          if (have(5)) {
            note(result.dropped,
                 "cash drawer kick (ESC p): Star's drawer command varies by model and "
                 "interface and is not established here");
            i += 5;
            continue;
          }
          break;
        default:
          break;
      }
    }

    if (first == 0x1D && have(2)) {
      const uint8_t second = data[i + 1];
      if (second == 0x21 && have(3)) {  // GS ! n — character size.
        // GS ! packs width-1 in the high nibble and height-1 in the low nibble. Star's
        // ESC i n1 n2 takes height first, then width, both zero-based — the same two
        // numbers in the other order.
        const uint8_t packed = data[i + 2];
        out.raw(Bytes{0x1B, 0x69, static_cast<uint8_t>(packed & 0x07u),
                      static_cast<uint8_t>((packed >> 4) & 0x07u)});
        i += 3;
        continue;
      }
      if (second == 0x56 && have(3)) {  // GS V m | GS V 65/66 n — cut.
        const uint8_t mode = data[i + 2];
        const size_t length = (mode == 65 || mode == 66) ? 4u : 3u;
        if (!have(length)) {
          break;
        }
        const bool full = mode == 0 || mode == 48 || mode == 65;
        out.cut(full ? Cut::Full : Cut::Partial);
        i += length;
        continue;
      }
      if (second == 0x76 && have(3) && data[i + 2] == 0x30) {  // GS v 0 m xL xH yL yH d..
        if (!have(8)) {
          break;
        }
        const size_t bytes_per_row = le16(data[i + 4], data[i + 5]);
        const size_t rows = le16(data[i + 6], data[i + 7]);
        const size_t total = 8u + bytes_per_row * rows;
        if (!have(total)) {
          break;
        }
        if (options.raster_line_mode && bytes_per_row > 0 && rows > 0) {
          out.rasterPacked(data + i + 8, bytes_per_row * rows,
                           static_cast<uint32_t>(bytes_per_row * 8u),
                           static_cast<uint32_t>(rows));
        } else {
          note(result.dropped,
               "raster image (GS v 0): declined because this profile does not enable "
               "Star raster line mode");
        }
        i += total;
        continue;
      }
      if (second == 0x28 && have(5)) {  // GS ( X pL pH ...
        const size_t length = le16(data[i + 3], data[i + 4]);
        const size_t total = 5u + length;
        if (!have(total)) {
          break;
        }
        if (data[i + 2] == 0x6B) {
          note(result.dropped,
               "QR code / 2D symbol (GS ( k): no Star symbol command is emitted by this "
               "engine");
        } else if (data[i + 2] == 0x48) {
          // The ESC/POS process-ID fence. Dropped rather than translated: Star's fence is
          // ETB or ESC GS ETX and the job path issues it separately.
          note(result.dropped, "ESC/POS process-ID fence (GS ( H): Star uses its own");
        } else {
          note(result.dropped, "unsupported GS ( command");
        }
        i += total;
        continue;
      }
      if (second == 0x72 && have(3)) {  // GS r n — queued status.
        note(result.dropped, "ESC/POS queued status (GS r): Star uses its own fence");
        i += 3;
        continue;
      }
      if (second == 0x61 && have(3)) {  // GS a n — ESC/POS ASB.
        note(result.dropped, "ESC/POS automatic status back (GS a): Star spells it "
                             "ESC RS a and the job path issues it");
        i += 3;
        continue;
      }
    }

    if (first == 0x10 && have(3) && data[i + 1] == 0x04) {  // DLE EOT n
      note(result.dropped, "ESC/POS realtime status (DLE EOT): not issued on Star");
      i += 3;
      continue;
    }

    if (first < 0x80u || first == 0x0A) {
      out.raw(&first, 1);
      i += 1;
      continue;
    }
    // Above 7-bit ASCII. The document was encoded in an ESC/POS code page whose Star
    // equivalent is a per-model table this core does not carry, so the byte becomes '?'
    // rather than a different wrong character.
    const uint8_t fallback = escpos::kUnmappedCodepointFallback;
    out.raw(&fallback, 1);
    note(result.dropped,
         "non-ASCII characters: the document's code page has no Star mapping here, so "
         "they print as '?'");
    ++result.unrecognised_bytes;
    i += 1;
  }

  // Whatever is left is a truncated command; pass it through and count it.
  while (i < size) {
    out.raw(data + i, 1);
    ++result.unrecognised_bytes;
    ++i;
  }

  result.bytes = out.take();
  return result;
}

TranscodeResult transcodeFromEscPos(const Bytes& data, const TranscodeOptions& options) {
  return transcodeFromEscPos(data.data(), data.size(), options);
}

const std::vector<std::string>& declaredDegradations() {
  // Written out rather than implied. docs/compatibility-brief.md's whole position is
  // that a capability nobody implemented and a capability nobody wrote down are the same
  // failure from the caller's side.
  static const std::vector<std::string> kDegradations = {
      "text is 7-bit ASCII only; Star character-set selection is per model and is not "
      "carried here, so anything above 0x7F prints as '?'",
      "no barcode or 2D symbol commands: a QR or a barcode in a document is dropped "
      "rather than emitted as an unverified command sequence",
      "no cash-drawer kick: Star's drawer command differs by model and interface and is "
      "not established here, so open_drawer is ignored on a Star profile",
      "no NV graphics, no downloaded logos, no macro storage",
      "raster is the documented ESC * r A / b / ESC * r B path and is "
      "documented-provisional rather than hardware-confirmed in this repository; "
      "StarCapabilities::raster_line_mode false declines it and reports Unsupported",
      "ASB decoding is limited to the ETB counter at block offset 7; the remaining "
      "status bytes are reported raw because only that field is documented here",
      "no realtime status query, so a Star job has no preflight and cannot refuse before "
      "printing on cover-open or paper-out",
  };
  return kDegradations;
}

}  // namespace pd::star
