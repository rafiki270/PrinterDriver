#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ESC/POS byte-buffer builder. Deliberately a pure encoder: it owns no socket, no
// timing and no state machine, so every byte it produces is testable as a golden
// buffer. Command semantics follow docs/techspec.md §3 and Epson's ESC/POS
// command reference; the conservative subset named in docs/sdk-spec.md §3.

namespace pd::escpos {

using Bytes = std::vector<uint8_t>;

class EncodingError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class Alignment : uint8_t { Left = 0, Center = 1, Right = 2 };

enum class CutMode { Partial, Full };

// Values are the `n` operand of ESC t n (select character code table).
enum class CodePage : uint8_t {
  PC437 = 0,
  PC850 = 2,
  WPC1252 = 16,
  PC852 = 18,
  PC858 = 19,
};

// Values are the `n` operand of DLE EOT n (docs/techspec.md §3). Real-time:
// answered immediately, out of order with buffered print data — never a
// completion fence (docs/techspec.md §3.3).
enum class DleEotKind : uint8_t {
  PrinterStatus = 1,
  OfflineCause = 2,
  ErrorCause = 3,
  PaperSensor = 4,
};

// Values are the `n` operand of GS I n (printer identification). 1-3 answer with a
// single ID byte; 65-68 answer with a text field. Text answers are framed
// Header(0x5F) + data + NUL on Epson and Epson-compatible firmware, but the whole
// point of docs/capability-profiles.md is that the *content* of these fields cannot be
// trusted — Rongta's manual documents returning "EPOSN" / "TM-T88V".
enum class PrinterInfoKind : uint8_t {
  ModelId = 1,
  TypeId = 2,
  RomVersionId = 3,
  FirmwareVersion = 65,
  MakerName = 66,
  ModelName = 67,
  SerialNumber = 68,
};

// Values are the `n1` operand of GS ( k function 165.
enum class QrModel : uint8_t { Model1 = 49, Model2 = 50, MicroQr = 51 };

// Values are the `n` operand of GS ( k function 169.
enum class QrErrorCorrection : uint8_t { L = 48, M = 49, Q = 50, H = 51 };

// `m` operand of GS v 0.
enum class RasterScale : uint8_t {
  Normal = 0,
  DoubleWidth = 1,
  DoubleHeight = 2,
  Quadruple = 3,
};

enum class Binarization { FixedThreshold, FloydSteinberg };

// The dot widths in use across the deployed fleet (docs/sdk-spec.md §9):
// 58 mm and 80 mm paper at 203 dpi / 8 dots per mm.
inline constexpr uint32_t kWidth58mm = 384;
inline constexpr uint32_t kWidth76mm = 504;
inline constexpr uint32_t kWidth80mm = 576;
// 104 mm of printable area at 203 dpi, which is what the 112 mm wide-media class
// actually prints (docs/device-database.md: never infer raster width from roll width).
inline constexpr uint32_t kWidth104mm = 832;

inline constexpr uint8_t kAsbAllEnabled = 0x0E;  // docs/techspec.md §3.4
inline constexpr uint8_t kUnmappedCodepointFallback = 0x3F;  // '?'

// --- Standalone command builders ------------------------------------------------
// These are the fence/status primitives the job state machine (M2) drives directly,
// so they are free functions rather than Encoder members.

// DLE EOT n — 10 04 n.
Bytes dleEot(DleEotKind kind);

// GS r 1 — 1D 72 01. Queued: the response arrives only after preceding print data
// has been processed, which is what makes it usable as a completion fence
// (docs/techspec.md §3.2).
Bytes gsPaperStatus();

// GS ( H fn 48 — 1D 28 48 06 00 30 30 d1 d2 d3 d4 (docs/techspec.md §3.1).
// The token must be exactly four printable-ASCII bytes (0x20..0x7E); anything else
// throws EncodingError. The printer echoes 37 22 <token> 00 once the preceding
// print data has physically printed.
Bytes processIdMarker(std::string_view token);
bool isValidProcessIdToken(std::string_view token) noexcept;

// GS a n — 1D 61 n. n = 0x0E enables online/offline, error and paper notifications.
Bytes asbEnable(uint8_t mask = kAsbAllEnabled);
Bytes asbDisable();

// GS I n — 1D 49 n. Read-only identification, safe in an automatic probe.
Bytes gsIdentity(PrinterInfoKind kind);
Bytes gsIdentity(uint8_t n);

// --- Deliberate operator commands -------------------------------------------------
// None of these are ever emitted by the engine or by the automatic capability probe.
// DLE ENQ resumes or discards a partly printed ticket and DLE DC4 can power the device
// off; on a kitchen printer that is a duplicate or a lost ticket, so they exist only
// behind an explicit operator action (docs/capability-profiles.md §5).

// DLE ENQ n — 10 05 n. 1 = recover and reprint from the line the error occurred on,
// 2 = recover while clearing the receive and print buffers.
Bytes dleEnq(uint8_t n);
inline constexpr uint8_t kDleEnqResume = 1;
inline constexpr uint8_t kDleEnqClearBuffers = 2;

// GS g 2 — 1D 67 32 00 nL nH. Reads a maintenance counter; the counter numbering is
// model-specific, so callers pass the number and report the raw answer.
Bytes gsMaintenanceCounter(uint16_t counter);

// GS ( A — 1D 28 41 02 00 n m. Built-in test print: n selects the paper, m the
// pattern (2 = printer status sheet).
Bytes gsTestPrint(uint8_t paper = 0, uint8_t pattern = 2);

// GS ( E fn 4 / fn 6 — settings readback. Reading is non-destructive, but it sits with
// the operator commands because the same command family writes settings.
Bytes gsReadMemorySwitch(uint8_t switch_number);
Bytes gsReadCustomizedSetting(uint8_t setting_number);

// --- Text encoding --------------------------------------------------------------

// UTF-8 in, single-byte code page out. Unmapped codepoints and malformed UTF-8
// become '?' — lossy by design and never an error, because a receipt with a '?'
// still prints while a thrown exception loses the ticket.
// Only PC852 has a full mapping table in this milestone; for the other code pages
// ASCII passes through and everything above 0x7F becomes '?'.
Bytes transliterate(std::string_view utf8, CodePage code_page);

// --- Builder --------------------------------------------------------------------

class Encoder {
 public:
  Encoder() = default;

  // Style setters emit their command only when the value actually changes,
  // tracking the documented power-on/ESC @ defaults (left aligned, no emphasis,
  // no underline, 1x1, PC437). Calling a setter twice with the same value emits
  // nothing; initialize() resets the tracked state to those defaults.
  Encoder& initialize();
  Encoder& selectCodePage(CodePage code_page);
  Encoder& align(Alignment alignment);
  Encoder& bold(bool enabled);
  Encoder& underline(uint8_t dots);        // 0 = off, 1 = 1-dot, 2 = 2-dot
  Encoder& textSize(uint8_t width_multiplier, uint8_t height_multiplier);  // 1..8

  Encoder& text(std::string_view utf8);
  Encoder& line(std::string_view utf8 = {});
  Encoder& feed();                          // LF
  Encoder& feedLines(uint8_t lines);        // ESC d n
  // ESC J n — feeds n dots without printing, e.g. to clear the head-to-cutter gap
  // before a cut (docs/testing-plan.md). n is a single byte; requests above 255 are
  // split across as many ESC J commands as needed.
  Encoder& feedDots(uint16_t dots);

  // GS V m (m = 0 full, 1 partial). When cut-with-feed is enabled the GS V 65/66 n
  // variant is emitted instead: the printer feeds n vertical motion units to the
  // cut position first. Some clones cut too early without it (docs/sdk-spec.md §9,
  // the Rongta quirk); it is a flag rather than a default because the extra feed
  // wastes paper on printers that do not need it.
  Encoder& useCutWithFeed(bool enabled, uint8_t feed_units = 0);
  Encoder& cut(CutMode mode = CutMode::Partial);
  Encoder& cutWithFeed(CutMode mode, uint8_t feed_units);

  // ESC p m t1 t2. t1/t2 are pulse on/off times in 2 ms units.
  Encoder& kickCashDrawer(uint8_t connector_pin = 0, uint8_t on_time = 25,
                          uint8_t off_time = 25);

  Encoder& asb(bool enabled, uint8_t mask = kAsbAllEnabled);

  // GS ( H marker / DLE EOT / GS r 1, appended to this buffer.
  Encoder& processId(std::string_view token);
  Encoder& realtimeStatus(DleEotKind kind);
  Encoder& queuedPaperStatus();

  // GS v 0 m xL xH yL yH d1..dk from already-packed rows, MSB-first, one bit per
  // dot, 1 = dot printed. `data` must hold exactly ceil(width/8) * height bytes.
  // max_rows_per_band > 0 splits the image into several GS v 0 commands; tall
  // rasters must be banded because some Epson models reject very tall images
  // (docs/api.md §3 tier 1, the 1024 px split).
  Encoder& rasterPacked(const uint8_t* data, size_t size, uint32_t width_dots,
                        uint32_t height_dots, RasterScale scale = RasterScale::Normal,
                        uint32_t max_rows_per_band = 0);

  // 8-bit grayscale (0 = black, 255 = white), row-major, no padding. Scaled to
  // target_width_dots by integer box filter, then binarized. Fully integer
  // arithmetic, so the same input always produces byte-identical output.
  Encoder& rasterGrayscale(const uint8_t* gray, uint32_t width, uint32_t height,
                           uint32_t target_width_dots, Binarization binarization,
                           uint8_t threshold = 128,
                           RasterScale scale = RasterScale::Normal,
                           uint32_t max_rows_per_band = 0);

  // GS ( k functions 165 (model), 167 (module size), 169 (error correction),
  // 180 (store data), 181 (print). Data is stored as raw bytes: no code page
  // transliteration, because QR payloads are byte strings, not display text.
  Encoder& qr(std::string_view data, uint8_t module_size = 4,
              QrErrorCorrection ec = QrErrorCorrection::M,
              QrModel model = QrModel::Model2);

  Encoder& raw(const uint8_t* data, size_t size);
  Encoder& raw(const Bytes& data);

  const Bytes& bytes() const noexcept { return buffer_; }
  Bytes take() noexcept {
    Bytes out = std::move(buffer_);
    buffer_.clear();
    return out;
  }
  void clear() noexcept;
  size_t size() const noexcept { return buffer_.size(); }

  CodePage codePage() const noexcept { return code_page_; }

 private:
  void put(std::initializer_list<uint8_t> data);
  void resetStyleState() noexcept;

  Bytes buffer_;
  CodePage code_page_ = CodePage::PC437;
  Alignment alignment_ = Alignment::Left;
  bool bold_ = false;
  uint8_t underline_ = 0;
  uint8_t size_width_ = 1;
  uint8_t size_height_ = 1;
  bool cut_with_feed_ = false;
  uint8_t cut_feed_units_ = 0;
};

// Exposed for tests and for the M2 raster pipeline: packs a grayscale buffer into
// 1bpp rows using the same deterministic scaling and binarization as
// Encoder::rasterGrayscale, without emitting any commands.
struct PackedRaster {
  Bytes data;
  uint32_t width_dots = 0;
  uint32_t height_dots = 0;
  uint32_t bytes_per_row = 0;
};

PackedRaster packGrayscale(const uint8_t* gray, uint32_t width, uint32_t height,
                           uint32_t target_width_dots, Binarization binarization,
                           uint8_t threshold = 128);

}  // namespace pd::escpos
