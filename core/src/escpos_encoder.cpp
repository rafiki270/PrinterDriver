#include "printerdriver/escpos_encoder.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace pd::escpos {
namespace {

constexpr uint8_t kEsc = 0x1B;
constexpr uint8_t kGs = 0x1D;
constexpr uint8_t kDle = 0x10;
constexpr uint8_t kEot = 0x04;
constexpr uint8_t kEnq = 0x05;
constexpr uint8_t kLf = 0x0A;

constexpr uint32_t kMaxRasterBytesPerRow = 0xFFFF;
constexpr uint32_t kMaxRasterRows = 0xFFFF;

void appendAll(Bytes& out, std::initializer_list<uint8_t> data) {
  out.insert(out.end(), data.begin(), data.end());
}

// Emits one GS v 0 command. Callers guarantee `rows` fits the 16-bit height field.
void appendRasterCommand(Bytes& out, RasterScale scale, uint32_t bytes_per_row,
                         uint32_t rows, const uint8_t* data) {
  appendAll(out, {kGs, 0x76, 0x30, static_cast<uint8_t>(scale),
                  static_cast<uint8_t>(bytes_per_row & 0xFF),
                  static_cast<uint8_t>((bytes_per_row >> 8) & 0xFF),
                  static_cast<uint8_t>(rows & 0xFF),
                  static_cast<uint8_t>((rows >> 8) & 0xFF)});
  out.insert(out.end(), data, data + static_cast<size_t>(bytes_per_row) * rows);
}

void appendBandedRaster(Bytes& out, const uint8_t* data, uint32_t bytes_per_row,
                        uint32_t height, RasterScale scale, uint32_t max_rows_per_band) {
  const uint32_t band = (max_rows_per_band == 0) ? height : max_rows_per_band;
  uint32_t row = 0;
  while (row < height) {
    const uint32_t rows = std::min(band, height - row);
    appendRasterCommand(out, scale, bytes_per_row,
                        rows, data + static_cast<size_t>(row) * bytes_per_row);
    row += rows;
  }
}

// Integer box filter. Deterministic by construction: no floating point, and the
// summation order is fixed by the loop nest, so the same input always yields the
// same output on every platform.
std::vector<uint8_t> resampleGray(const uint8_t* gray, uint32_t src_w, uint32_t src_h,
                                  uint32_t dst_w, uint32_t dst_h) {
  std::vector<uint8_t> out(static_cast<size_t>(dst_w) * dst_h, 0);
  for (uint32_t y = 0; y < dst_h; ++y) {
    uint32_t sy0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * src_h / dst_h);
    uint32_t sy1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * src_h / dst_h);
    if (sy1 <= sy0) {
      sy1 = sy0 + 1;
    }
    if (sy1 > src_h) {
      sy1 = src_h;
    }
    for (uint32_t x = 0; x < dst_w; ++x) {
      uint32_t sx0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * src_w / dst_w);
      uint32_t sx1 = static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * src_w / dst_w);
      if (sx1 <= sx0) {
        sx1 = sx0 + 1;
      }
      if (sx1 > src_w) {
        sx1 = src_w;
      }
      uint64_t sum = 0;
      uint64_t count = 0;
      for (uint32_t sy = sy0; sy < sy1; ++sy) {
        for (uint32_t sx = sx0; sx < sx1; ++sx) {
          sum += gray[static_cast<size_t>(sy) * src_w + sx];
          ++count;
        }
      }
      out[static_cast<size_t>(y) * dst_w + x] =
          static_cast<uint8_t>(count == 0 ? 0 : sum / count);
    }
  }
  return out;
}

void setDot(Bytes& packed, uint32_t bytes_per_row, uint32_t x, uint32_t y) {
  packed[static_cast<size_t>(y) * bytes_per_row + (x >> 3)] |=
      static_cast<uint8_t>(0x80u >> (x & 7u));
}

}  // namespace

Bytes dleEot(DleEotKind kind) {
  return Bytes{kDle, kEot, static_cast<uint8_t>(kind)};
}

Bytes gsPaperStatus() { return Bytes{kGs, 0x72, 0x01}; }

bool isValidProcessIdToken(std::string_view token) noexcept {
  if (token.size() != 4) {
    return false;
  }
  for (const char c : token) {
    const auto value = static_cast<uint8_t>(c);
    if (value < 0x20 || value > 0x7E) {
      return false;
    }
  }
  return true;
}

Bytes processIdMarker(std::string_view token) {
  if (!isValidProcessIdToken(token)) {
    throw EncodingError(
        "GS ( H process ID must be exactly four printable-ASCII bytes (0x20-0x7E)");
  }
  // pL pH = 06 00 (six operand bytes: fn m d1 d2 d3 d4), fn = 0x30 (48), m = 0x30.
  Bytes out{kGs, 0x28, 0x48, 0x06, 0x00, 0x30, 0x30};
  out.insert(out.end(), token.begin(), token.end());
  return out;
}

Bytes asbEnable(uint8_t mask) { return Bytes{kGs, 0x61, mask}; }

Bytes asbDisable() { return Bytes{kGs, 0x61, 0x00}; }

Bytes gsIdentity(uint8_t n) { return Bytes{kGs, 0x49, n}; }

Bytes gsIdentity(PrinterInfoKind kind) {
  return gsIdentity(static_cast<uint8_t>(kind));
}

Bytes dleEnq(uint8_t n) { return Bytes{kDle, kEnq, n}; }

Bytes gsMaintenanceCounter(uint16_t counter) {
  // GS g 2 m nL nH, m = 0.
  return Bytes{kGs, 0x67, 0x32, 0x00, static_cast<uint8_t>(counter & 0xFF),
               static_cast<uint8_t>((counter >> 8) & 0xFF)};
}

Bytes gsTestPrint(uint8_t paper, uint8_t pattern) {
  return Bytes{kGs, 0x28, 0x41, 0x02, 0x00, paper, pattern};
}

Bytes gsReadMemorySwitch(uint8_t switch_number) {
  return Bytes{kGs, 0x28, 0x45, 0x02, 0x00, 0x04, switch_number};
}

Bytes gsReadCustomizedSetting(uint8_t setting_number) {
  return Bytes{kGs, 0x28, 0x45, 0x02, 0x00, 0x06, setting_number};
}

void Encoder::put(std::initializer_list<uint8_t> data) { appendAll(buffer_, data); }

void Encoder::resetStyleState() noexcept {
  code_page_ = CodePage::PC437;
  alignment_ = Alignment::Left;
  bold_ = false;
  underline_ = 0;
  size_width_ = 1;
  size_height_ = 1;
}

void Encoder::clear() noexcept {
  buffer_.clear();
  resetStyleState();
  cut_with_feed_ = false;
  cut_feed_units_ = 0;
}

Encoder& Encoder::initialize() {
  put({kEsc, 0x40});
  resetStyleState();
  return *this;
}

Encoder& Encoder::selectCodePage(CodePage code_page) {
  if (code_page_ == code_page) {
    return *this;
  }
  code_page_ = code_page;
  put({kEsc, 0x74, static_cast<uint8_t>(code_page)});
  return *this;
}

Encoder& Encoder::align(Alignment alignment) {
  if (alignment_ == alignment) {
    return *this;
  }
  alignment_ = alignment;
  put({kEsc, 0x61, static_cast<uint8_t>(alignment)});
  return *this;
}

Encoder& Encoder::bold(bool enabled) {
  if (bold_ == enabled) {
    return *this;
  }
  bold_ = enabled;
  put({kEsc, 0x45, static_cast<uint8_t>(enabled ? 1 : 0)});
  return *this;
}

Encoder& Encoder::underline(uint8_t dots) {
  if (dots > 2) {
    throw EncodingError("ESC - underline thickness must be 0, 1 or 2 dots");
  }
  if (underline_ == dots) {
    return *this;
  }
  underline_ = dots;
  put({kEsc, 0x2D, dots});
  return *this;
}

Encoder& Encoder::textSize(uint8_t width_multiplier, uint8_t height_multiplier) {
  if (width_multiplier < 1 || width_multiplier > 8 || height_multiplier < 1 ||
      height_multiplier > 8) {
    throw EncodingError("GS ! size multipliers must be in 1..8");
  }
  if (size_width_ == width_multiplier && size_height_ == height_multiplier) {
    return *this;
  }
  size_width_ = width_multiplier;
  size_height_ = height_multiplier;
  // GS ! n: high nibble = width - 1, low nibble = height - 1.
  const auto n = static_cast<uint8_t>(((width_multiplier - 1) << 4) |
                                      (height_multiplier - 1));
  put({kGs, 0x21, n});
  return *this;
}

Encoder& Encoder::text(std::string_view utf8) {
  const Bytes encoded = transliterate(utf8, code_page_);
  buffer_.insert(buffer_.end(), encoded.begin(), encoded.end());
  return *this;
}

Encoder& Encoder::line(std::string_view utf8) {
  text(utf8);
  return feed();
}

Encoder& Encoder::feed() {
  buffer_.push_back(kLf);
  return *this;
}

Encoder& Encoder::feedLines(uint8_t lines) {
  put({kEsc, 0x64, lines});
  return *this;
}

Encoder& Encoder::feedDots(uint16_t dots) {
  // ESC J n: n is a single byte, so a request above 255 is chunked across several
  // commands rather than truncated.
  while (dots > 0) {
    const uint8_t chunk = dots > 0xFF ? 0xFF : static_cast<uint8_t>(dots);
    put({kEsc, 0x4A, chunk});
    dots = static_cast<uint16_t>(dots - chunk);
  }
  return *this;
}

Encoder& Encoder::useCutWithFeed(bool enabled, uint8_t feed_units) {
  cut_with_feed_ = enabled;
  cut_feed_units_ = feed_units;
  return *this;
}

Encoder& Encoder::cut(CutMode mode) {
  if (cut_with_feed_) {
    return cutWithFeed(mode, cut_feed_units_);
  }
  // GS V m: m = 0 full cut, m = 1 partial (one point left uncut).
  put({kGs, 0x56, static_cast<uint8_t>(mode == CutMode::Full ? 0x00 : 0x01)});
  return *this;
}

Encoder& Encoder::cutWithFeed(CutMode mode, uint8_t feed_units) {
  // GS V m n: m = 65 full, m = 66 partial. The printer feeds n vertical motion
  // units to the cut position before cutting.
  put({kGs, 0x56, static_cast<uint8_t>(mode == CutMode::Full ? 65 : 66), feed_units});
  return *this;
}

Encoder& Encoder::kickCashDrawer(uint8_t connector_pin, uint8_t on_time,
                                 uint8_t off_time) {
  if (connector_pin > 1) {
    throw EncodingError("ESC p connector pin must be 0 (pin 2) or 1 (pin 5)");
  }
  put({kEsc, 0x70, connector_pin, on_time, off_time});
  return *this;
}

Encoder& Encoder::asb(bool enabled, uint8_t mask) {
  put({kGs, 0x61, static_cast<uint8_t>(enabled ? mask : 0x00)});
  return *this;
}

Encoder& Encoder::processId(std::string_view token) {
  return raw(processIdMarker(token));
}

Encoder& Encoder::realtimeStatus(DleEotKind kind) { return raw(dleEot(kind)); }

Encoder& Encoder::queuedPaperStatus() { return raw(gsPaperStatus()); }

Encoder& Encoder::rasterPacked(const uint8_t* data, size_t size, uint32_t width_dots,
                               uint32_t height_dots, RasterScale scale,
                               uint32_t max_rows_per_band) {
  const uint32_t bytes_per_row = (width_dots + 7) / 8;
  if (width_dots == 0 || height_dots == 0) {
    throw EncodingError("GS v 0 raster must have non-zero width and height");
  }
  if (bytes_per_row > kMaxRasterBytesPerRow || height_dots > kMaxRasterRows) {
    throw EncodingError("GS v 0 raster exceeds the 16-bit width/height fields");
  }
  if (size != static_cast<size_t>(bytes_per_row) * height_dots) {
    throw EncodingError("packed raster size does not match width x height");
  }
  appendBandedRaster(buffer_, data, bytes_per_row, height_dots, scale,
                     max_rows_per_band);
  return *this;
}

PackedRaster packGrayscale(const uint8_t* gray, uint32_t width, uint32_t height,
                           uint32_t target_width_dots, Binarization binarization,
                           uint8_t threshold) {
  if (width == 0 || height == 0 || target_width_dots == 0) {
    throw EncodingError("grayscale raster must have non-zero width and height");
  }

  // Fit to width, preserving aspect ratio; at least one row survives.
  uint32_t dst_w = target_width_dots;
  auto dst_h = static_cast<uint32_t>(
      (static_cast<uint64_t>(height) * dst_w + width / 2) / width);
  if (dst_h == 0) {
    dst_h = 1;
  }
  if (dst_h > kMaxRasterRows) {
    throw EncodingError("scaled raster exceeds the 16-bit height field");
  }

  const std::vector<uint8_t> scaled = resampleGray(gray, width, height, dst_w, dst_h);

  PackedRaster result;
  result.width_dots = dst_w;
  result.height_dots = dst_h;
  result.bytes_per_row = (dst_w + 7) / 8;
  result.data.assign(static_cast<size_t>(result.bytes_per_row) * dst_h, 0);

  if (binarization == Binarization::FixedThreshold) {
    for (uint32_t y = 0; y < dst_h; ++y) {
      for (uint32_t x = 0; x < dst_w; ++x) {
        // 1 = dot printed = black; grayscale 0 is black.
        if (scaled[static_cast<size_t>(y) * dst_w + x] < threshold) {
          setDot(result.data, result.bytes_per_row, x, y);
        }
      }
    }
    return result;
  }

  // Floyd-Steinberg, left-to-right raster order, integer error accumulation.
  // Integer division truncates toward zero, which is deterministic in C++; the
  // fixed traversal order plus integer maths make the output reproducible.
  std::vector<int32_t> work(scaled.begin(), scaled.end());
  const auto at = [&](uint32_t x, uint32_t y) -> int32_t& {
    return work[static_cast<size_t>(y) * dst_w + x];
  };
  for (uint32_t y = 0; y < dst_h; ++y) {
    for (uint32_t x = 0; x < dst_w; ++x) {
      const int32_t old_value = std::clamp(at(x, y), 0, 255);
      const int32_t new_value = old_value < threshold ? 0 : 255;
      if (new_value == 0) {
        setDot(result.data, result.bytes_per_row, x, y);
      }
      const int32_t error = old_value - new_value;
      if (x + 1 < dst_w) {
        at(x + 1, y) += error * 7 / 16;
      }
      if (y + 1 < dst_h) {
        if (x > 0) {
          at(x - 1, y + 1) += error * 3 / 16;
        }
        at(x, y + 1) += error * 5 / 16;
        if (x + 1 < dst_w) {
          at(x + 1, y + 1) += error * 1 / 16;
        }
      }
    }
  }
  return result;
}

Encoder& Encoder::rasterGrayscale(const uint8_t* gray, uint32_t width, uint32_t height,
                                  uint32_t target_width_dots, Binarization binarization,
                                  uint8_t threshold, RasterScale scale,
                                  uint32_t max_rows_per_band) {
  const PackedRaster packed =
      packGrayscale(gray, width, height, target_width_dots, binarization, threshold);
  appendBandedRaster(buffer_, packed.data.data(), packed.bytes_per_row,
                     packed.height_dots, scale, max_rows_per_band);
  return *this;
}

Encoder& Encoder::qr(std::string_view data, uint8_t module_size,
                     QrErrorCorrection ec, QrModel model) {
  if (module_size < 1 || module_size > 16) {
    throw EncodingError("QR module size must be in 1..16");
  }
  // GS ( k function 180 carries fn + m + payload in its pL/pH length, so the
  // payload is limited to 0xFFFF - 3 bytes.
  if (data.size() > 0xFFFFu - 3u) {
    throw EncodingError("QR payload exceeds the GS ( k 16-bit length field");
  }

  // fn 165 (0xA5): select model. n2 is reserved and always 0.
  put({kGs, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, static_cast<uint8_t>(model), 0x00});
  // fn 167 (0xA7): module size in dots.
  put({kGs, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43, module_size});
  // fn 169 (0xA9): error correction level.
  put({kGs, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, static_cast<uint8_t>(ec)});
  // fn 180 (0xB4): store data in the symbol storage area.
  const auto length = static_cast<uint32_t>(data.size() + 3);
  put({kGs, 0x28, 0x6B, static_cast<uint8_t>(length & 0xFF),
       static_cast<uint8_t>((length >> 8) & 0xFF), 0x31, 0x50, 0x30});
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  // fn 181 (0xB5): print the stored symbol.
  put({kGs, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30});
  return *this;
}

Encoder& Encoder::raw(const uint8_t* data, size_t size) {
  buffer_.insert(buffer_.end(), data, data + size);
  return *this;
}

Encoder& Encoder::raw(const Bytes& data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  return *this;
}

}  // namespace pd::escpos
