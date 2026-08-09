#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/capability_profile.hpp"
#include "printerdriver/dsl/document.hpp"
#include "printerdriver/dsl/report.hpp"
#include "printerdriver/escpos_encoder.hpp"

// The hardware-path renderer: a bound document in, ESC/POS bytes plus a render report
// out (docs/receipt-dsl.md "Implementation phasing" step 1).
//
// The renderer is a pure function of (document, profile). It owns no socket, no job and
// no state machine, and it deliberately does **not** emit the things the engine owns:
// no ESC @ initialise, no trailing cut, no completion fence, no final feed. `meta.cut`
// and `meta.margins` are *returned* in the RenderOutput for the caller to fold into
// JobOptions — the document declares them, the engine applies them, and the
// blade-clearance floor in the capability profile stays unconditional either way.
//
// Layout happens once. `layoutDocument` produces the visual lines and the non-text ops;
// `render` turns those into bytes and `renderText` prints the same lines as an on-screen
// approximation. A preview that ran its own layout would eventually disagree with the
// paper, which is the one thing a preview must never do.

namespace pd::dsl {

// Media facts the layout needs. Font A is 12 dots wide and font B is 9 at 203 dpi, so
// an 80 mm/576 dot printer is 48 columns and a 58 mm/384 dot one is 32
// (docs/api.md §13: the DSL renderer consumes `media`).
struct RenderProfile {
  uint32_t width_dots = escpos::kWidth80mm;
  uint16_t dpi = 203;
  uint32_t font_a_char_dots = 12;
  uint32_t font_b_char_dots = 9;
  escpos::CodePage code_page = escpos::CodePage::PC437;
  bool qr = true;
  bool images = true;
  bool cutter = true;
  bool drawer_kick = true;
  bool barcodes = true;

  // The blade-clearance floor of docs/receipt-dsl.md "Margins", taken from the
  // capability profile's media. Every cut this renderer emits is preceded by at least
  // this much feed, exactly as the engine does for the trailing cut — a mid-document
  // cut clips content just as happily as a final one.
  uint16_t head_to_cutter_feed_dots = 120;

  uint32_t charDots(const ResolvedStyle& style) const noexcept;
  uint32_t charsPerLine(const ResolvedStyle& style) const noexcept;
  uint32_t dotsPerMm() const noexcept;

  static RenderProfile forWidth(uint32_t width_dots);
  // Media, code page and cutter facts taken from a capability profile; width_dots 0
  // means "use the profile's printable width".
  static RenderProfile from(const CapabilityProfile& profile, uint32_t width_dots = 0);
};

// A named grayscale asset an `image` block can reference (8-bit, row-major, 0 = black).
struct ImageAsset {
  std::string name;
  std::vector<uint8_t> gray;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct RenderOptions {
  RenderProfile profile;
  std::vector<ImageAsset> images;
  // Off by default: the engine sends ESC @ and owns the code page for banner text.
  bool emit_initialize = false;
  bool emit_code_page = true;
  uint32_t max_rows_per_band = 1024;

  // A caller's own whitespace before a mid-document cut, in the same relationship to
  // the profile as JobOptions::bottom_feed_dots has to the engine's trailing cut: the
  // renderer feeds max(profile.head_to_cutter_feed_dots, cut_clearance_dots). More is
  // always granted, less is never — no setting can reintroduce a clipped ticket.
  uint16_t cut_clearance_dots = 0;
};

// One laid-out operation. Text ops carry a finished visual line: columns arrive already
// padded, plain text does not (the printer's own ESC a does the alignment).
struct LayoutOp {
  enum class Kind { Text, Feed, Qr, Image, Barcode, DrawerKick, Raw, Cut };

  Kind kind = Kind::Text;
  std::string source;  // "blocks[3]"
  ResolvedStyle style;
  uint32_t columns = 0;  // the line width this op was laid out against

  std::string text;       // Text
  bool pre_padded = false;  // true for a columns row: do not align it again

  uint32_t feed_lines = 0;
  uint32_t feed_dots = 0;

  std::string payload;  // Qr
  int qr_size = 6;
  QrEc qr_ec = QrEc::M;

  std::vector<uint8_t> gray;  // Image
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t target_width_dots = 0;
  Dither dither = Dither::FloydSteinberg;

  // Barcode. `barcode_command` is the finished GS h / GS w / GS H / GS k block, so the
  // symbology encoder runs once, during layout, and both the byte path and the preview
  // read the same answer.
  Symbology symbology = Symbology::Code128;
  int barcode_height_dots = 64;
  int barcode_module_width = 2;
  Hri hri = Hri::None;
  std::vector<uint8_t> barcode_command;
  std::string barcode_text;  // what the symbol says, check digit included

  int drawer_pin = 0;
  int drawer_pulse = 25;

  std::vector<uint8_t> raw;
  CutKind cut = CutKind::Partial;
  // max(profile.head_to_cutter_feed_dots, options.cut_clearance_dots), fed immediately
  // before the cut command.
  uint32_t cut_clearance_dots = 0;
};

// docs/receipt-dsl.md "Margins": resolved to dots at the profile's dpi. `topDots` wins
// over `topMm` when a document carries both.
struct ResolvedMargins {
  std::optional<uint32_t> top_dots;
  std::optional<uint32_t> bottom_dots;
  bool any() const noexcept { return top_dots.has_value() || bottom_dots.has_value(); }
};

struct LayoutOutput {
  std::vector<LayoutOp> ops;
  RenderReport report;
  std::optional<CutRequest> requested_cut;
  ResolvedMargins requested_margins;
};

struct RenderOutput {
  escpos::Encoder ops;
  RenderReport report;
  std::optional<CutRequest> requested_cut;
  ResolvedMargins requested_margins;

  const escpos::Bytes& bytes() const noexcept { return ops.bytes(); }
  escpos::CodePage codePage() const noexcept { return ops.codePage(); }
};

struct TextPreview {
  std::vector<std::string> lines;
  RenderReport report;
  std::optional<CutRequest> requested_cut;
  ResolvedMargins requested_margins;

  std::string text() const;  // lines joined with '\n'
};

LayoutOutput layoutDocument(const Document& document, const RenderOptions& options = {});
RenderOutput render(const Document& document, const RenderOptions& options = {});
// The same layout as characters, with bracketed markers for the ops that have no text
// form. For previews and for `pdctl render`; never for a printer.
TextPreview renderText(const Document& document, const RenderOptions& options = {});

}  // namespace pd::dsl
