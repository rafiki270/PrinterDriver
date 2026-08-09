#include "printerdriver/dsl/render.hpp"

#include <algorithm>
#include <cmath>

#include "printerdriver/dsl/barcode.hpp"
#include "printerdriver/dsl/text.hpp"

namespace pd::dsl {
namespace {

constexpr uint8_t kEsc = 0x1B;
constexpr uint8_t kGs = 0x1D;

escpos::Alignment toEncoderAlignment(Align align) noexcept {
  switch (align) {
    case Align::Left: return escpos::Alignment::Left;
    case Align::Center: return escpos::Alignment::Center;
    case Align::Right: return escpos::Alignment::Right;
  }
  return escpos::Alignment::Left;
}

escpos::QrErrorCorrection toEncoderEc(QrEc ec) noexcept {
  switch (ec) {
    case QrEc::L: return escpos::QrErrorCorrection::L;
    case QrEc::M: return escpos::QrErrorCorrection::M;
    case QrEc::Q: return escpos::QrErrorCorrection::Q;
    case QrEc::H: return escpos::QrErrorCorrection::H;
  }
  return escpos::QrErrorCorrection::M;
}

uint8_t underlineDots(Underline underline) noexcept {
  switch (underline) {
    case Underline::None: return 0;
    case Underline::Single: return 1;
    case Underline::Double: return 2;
  }
  return 0;
}

bool isPrinterFont(const std::string& font) noexcept {
  return font == "printerA" || font == "printerB";
}

std::string dividerUnit(const Block& block) {
  switch (block.divider) {
    case DividerKind::Solid: return "-";
    case DividerKind::Dashed: return "- ";
    case DividerKind::Double: return "=";
    case DividerKind::Char: return block.divider_char.empty() ? "-" : block.divider_char;
  }
  return "-";
}

// --- column geometry -------------------------------------------------------------------

struct ColumnPlan {
  std::vector<size_t> widths;
  bool truncated = false;
};

ColumnPlan planColumns(const std::vector<Cell>& cells, size_t line_columns,
                       uint32_t char_dots) {
  ColumnPlan plan;
  plan.widths.assign(cells.size(), 0);
  std::vector<size_t> flex_indices;
  size_t fixed_total = 0;

  for (size_t i = 0; i < cells.size(); ++i) {
    const CellWidth& width = cells[i].width;
    size_t requested = 0;
    switch (width.unit) {
      case CellWidth::Unit::Flex:
        flex_indices.push_back(i);
        continue;
      case CellWidth::Unit::Chars:
        requested = static_cast<size_t>(std::max(0, width.value));
        break;
      case CellWidth::Unit::Dots:
      case CellWidth::Unit::Percent: {
        // Dots resolve against the same character cell the rest of the layout uses;
        // percent is not a column concept and is treated as dots of the line.
        const uint32_t dots = static_cast<uint32_t>(std::max(0, width.value));
        requested = char_dots == 0 ? 0 : static_cast<size_t>(dots / char_dots);
        break;
      }
    }
    const size_t remaining = line_columns > fixed_total ? line_columns - fixed_total : 0;
    const size_t granted = std::min(requested, remaining);
    if (granted < requested) {
      plan.truncated = true;
    }
    plan.widths[i] = granted;
    fixed_total += granted;
  }

  if (flex_indices.empty()) {
    return plan;
  }
  size_t remaining = line_columns > fixed_total ? line_columns - fixed_total : 0;
  size_t total_weight = 0;
  for (const size_t index : flex_indices) {
    total_weight += static_cast<size_t>(std::max(1, cells[index].width.value));
  }
  for (size_t offset = 0; offset < flex_indices.size(); ++offset) {
    const size_t index = flex_indices[offset];
    const auto weight = static_cast<size_t>(std::max(1, cells[index].width.value));
    size_t granted;
    if (offset + 1 == flex_indices.size()) {
      granted = remaining;  // the last flex cell absorbs the rounding remainder
    } else {
      granted = total_weight == 0 ? 0 : (remaining * weight) / total_weight;
      granted = std::min(granted, remaining);
    }
    // A flex cell squeezed to nothing still gets one column, so the row cannot vanish.
    if (granted == 0 && remaining > 0) {
      granted = 1;
    }
    plan.widths[index] = granted;
    remaining -= std::min(granted, remaining);
  }
  return plan;
}

// --- the layout pass ---------------------------------------------------------------------

class Layout {
 public:
  Layout(const Document& document, const RenderOptions& options, LayoutOutput* out)
      : document_(document), options_(options), out_(out) {}

  void run() {
    resolveMeta();
    for (size_t i = 0; i < document_.blocks.size(); ++i) {
      block(document_.blocks[i], "blocks[" + std::to_string(i) + "]",
            i + 1 == document_.blocks.size());
    }
  }

 private:
  void resolveMeta() {
    out_->requested_cut = document_.meta.cut;
    const Margins& margins = document_.meta.margins;
    const auto dots = [this](double mm) {
      return static_cast<uint32_t>(
          std::lround(mm * static_cast<double>(options_.profile.dpi) / 25.4));
    };
    if (margins.top_dots.has_value()) {
      out_->requested_margins.top_dots =
          static_cast<uint32_t>(std::max(0, *margins.top_dots));
    } else if (margins.top_mm.has_value()) {
      out_->requested_margins.top_dots = dots(std::max(0.0, *margins.top_mm));
    }
    if (margins.bottom_dots.has_value()) {
      out_->requested_margins.bottom_dots =
          static_cast<uint32_t>(std::max(0, *margins.bottom_dots));
    } else if (margins.bottom_mm.has_value()) {
      out_->requested_margins.bottom_dots = dots(std::max(0.0, *margins.bottom_mm));
    }
  }

  ResolvedStyle resolve(const StyleRef& ref, const std::string& where) {
    ResolvedStyle style = document_.resolve(ref, &out_->report, where);
    // Declared degradation, never silent (docs/receipt-dsl.md "Degradation rules").
    if (style.italic) {
      out_->report.add(ReportKind::UnsupportedStyle, where, "italic", "bold",
                       RenderPath::Hardware,
                       "no hardware italic; the raster path is not built yet");
      style.bold = true;
    }
    if (style.rotate90) {
      out_->report.add(ReportKind::UnsupportedStyle, where, "rotate90", "upright",
                       RenderPath::Hardware,
                       "rotated text changes the whole line geometry; not attempted");
      style.rotate90 = false;
    }
    if (!isPrinterFont(style.font)) {
      out_->report.add(ReportKind::UnsupportedStyle, where, "font:" + style.font,
                       "printerA", RenderPath::Hardware,
                       "named raster fonts need the raster path");
      style.font = "printerA";
    }
    return style;
  }

  void emitText(const std::string& line, const ResolvedStyle& style, uint32_t columns,
                bool pre_padded, const std::string& where) {
    LayoutOp op;
    op.kind = LayoutOp::Kind::Text;
    op.text = line;
    op.style = style;
    op.columns = columns;
    op.pre_padded = pre_padded;
    op.source = where;
    out_->ops.push_back(std::move(op));
  }

  void block(const Block& block, const std::string& where, bool is_last) {
    switch (block.kind) {
      case Block::Kind::Text: {
        const ResolvedStyle style = resolve(block.style, where);
        const uint32_t columns = options_.profile.charsPerLine(style);
        for (const std::string& line : text::wrap(block.content, columns, style.wrap)) {
          emitText(line, style, columns, false, where);
        }
        return;
      }
      case Block::Kind::Columns:
        columnsRow(block, where);
        return;
      case Block::Kind::Divider: {
        const ResolvedStyle style = resolve(block.style, where);
        const uint32_t columns = options_.profile.charsPerLine(style);
        if (block.thickness_dots.has_value() && *block.thickness_dots > 0) {
          out_->report.add(ReportKind::UnsupportedStyle, where,
                           "thicknessDots:" + std::to_string(*block.thickness_dots),
                           "character rule", RenderPath::Hardware,
                           "a dot-height rule needs the raster path");
        }
        emitText(text::repeat(dividerUnit(block), columns), style, columns, true, where);
        return;
      }
      case Block::Kind::Feed: {
        LayoutOp op;
        op.kind = LayoutOp::Kind::Feed;
        op.source = where;
        if (block.feed_dots.has_value()) {
          // 0xFFFF dots (~8 m of paper at 203 dpi) matches the emission clamp; without
          // it a single literal like {"feed":{"dots":2147483647}} makes the text
          // preview allocate ~90 million lines.
          const int requested = std::max(0, *block.feed_dots);
          op.feed_dots = static_cast<uint32_t>(std::min(requested, 0xFFFF));
          if (requested > 0xFFFF) {
            out_->report.add(ReportKind::MalformedTemplate, where,
                             "feed of " + std::to_string(requested) + " dots",
                             "clamped to 65535", RenderPath::Hardware,
                             "beyond any physical roll");
          }
        } else {
          op.feed_lines = static_cast<uint32_t>(std::clamp(block.feed_lines, 0, 255));
        }
        out_->ops.push_back(std::move(op));
        return;
      }
      case Block::Kind::Cut: {
        if (!options_.profile.cutter) {
          out_->report.add(ReportKind::UnsupportedBlock, where,
                           std::string("cut:") +
                               (block.cut == CutKind::Full ? "full" : "partial"),
                           "omitted", RenderPath::NotRendered,
                           "this profile has no cutter");
          return;
        }
        if (is_last) {
          // The engine appends its own trailing cut; a document that also ends in one
          // would cut twice. Said out loud rather than quietly dropped.
          out_->report.add(ReportKind::Note, where, "cut as the final block",
                           "emitted in place", RenderPath::Hardware,
                           "the job's trailing cut still applies; consider meta.cut");
        }
        LayoutOp op;
        op.kind = LayoutOp::Kind::Cut;
        op.cut = block.cut;
        op.source = where;
        // The print head sits ahead of the blade, so content printed right up to a cut
        // is sliced by the mechanism meant to free it. The engine feeds
        // max(head_to_cutter_feed_dots, bottom_feed_dots) before its trailing cut
        // (docs/receipt-dsl.md "Margins"); a mid-document cut is the same geometry and
        // gets the same max(), with the caller's figure in place of the job's.
        op.cut_clearance_dots =
            std::max<uint32_t>(options_.profile.head_to_cutter_feed_dots,
                               options_.cut_clearance_dots);
        out_->ops.push_back(std::move(op));
        return;
      }
      case Block::Kind::DrawerKick: {
        if (!options_.profile.drawer_kick) {
          out_->report.add(ReportKind::UnsupportedBlock, where, "drawerKick", "omitted",
                           RenderPath::NotRendered, "this profile has no drawer port");
          return;
        }
        LayoutOp op;
        op.kind = LayoutOp::Kind::DrawerKick;
        op.drawer_pin = std::clamp(block.drawer_pin, 0, 1);
        op.drawer_pulse = std::clamp(block.drawer_pulse, 1, 255);
        op.source = where;
        out_->ops.push_back(std::move(op));
        return;
      }
      case Block::Kind::Raw: {
        for (size_t i = 0; i + 1 < block.raw.size(); ++i) {
          const bool cut_command = block.raw[i] == kGs && block.raw[i + 1] == 0x56;
          const bool realtime = block.raw[i] == 0x10 &&
                                (block.raw[i + 1] == 0x04 || block.raw[i + 1] == 0x05);
          if (cut_command || realtime) {
            out_->report.add(ReportKind::RawFramingRisk, where, "raw bytes",
                             "passed through", RenderPath::Hardware,
                             cut_command ? "contains GS V (cut): the core owns job framing"
                                         : "contains a realtime status command");
            break;
          }
        }
        LayoutOp op;
        op.kind = LayoutOp::Kind::Raw;
        op.raw = block.raw;
        op.source = where;
        out_->ops.push_back(std::move(op));
        return;
      }
      case Block::Kind::Qr: {
        if (!options_.profile.qr) {
          out_->report.add(ReportKind::UnsupportedBlock, where, "qr", "omitted",
                           RenderPath::NotRendered, "this profile has no QR support");
          return;
        }
        LayoutOp op;
        op.kind = LayoutOp::Kind::Qr;
        op.payload = block.content;
        op.qr_size = std::clamp(block.qr_size, 1, 16);
        op.qr_ec = block.qr_ec;
        op.style.align = block.align;
        op.source = where;
        if (op.qr_size != block.qr_size) {
          out_->report.add(ReportKind::Truncated, where,
                           "qr size " + std::to_string(block.qr_size),
                           "size " + std::to_string(op.qr_size), RenderPath::Hardware,
                           "GS ( k module size is 1..16");
        }
        out_->ops.push_back(std::move(op));
        return;
      }
      case Block::Kind::Barcode:
        barcode(block, where);
        return;
      case Block::Kind::Image:
        image(block, where);
        return;
      case Block::Kind::Each:
      case Block::Kind::If:
        // A control block reaching the renderer means the document was never bound.
        out_->report.add(ReportKind::UnsupportedBlock, where,
                         std::string(to_string(block.kind)) + ":" + block.path, "omitted",
                         RenderPath::NotRendered,
                         "template control block in an unbound document");
        return;
    }
  }

  void columnsRow(const Block& block, const std::string& where) {
    if (block.cells.empty()) {
      return;
    }
    // docs/receipt-dsl.md: the hardware path has no column command, it has characters
    // per line — so a row carries one style, taken from the first cell.
    const ResolvedStyle style = resolve(block.cells.front().style, where + ".cells[0]");
    const uint32_t line_columns = options_.profile.charsPerLine(style);
    const ColumnPlan plan =
        planColumns(block.cells, line_columns, options_.profile.charDots(style));
    if (plan.truncated) {
      out_->report.add(ReportKind::Truncated, where, "fixed cell widths",
                       std::to_string(line_columns) + " columns", RenderPath::Hardware,
                       "the fixed widths do not fit the media");
    }

    std::vector<std::vector<std::string>> cell_lines(block.cells.size());
    size_t rows = 1;
    for (size_t i = 0; i < block.cells.size(); ++i) {
      const Cell& cell = block.cells[i];
      const std::string cell_where = where + ".cells[" + std::to_string(i) + "]";
      if (i != 0 && !cell.style.empty()) {
        const ResolvedStyle cell_style = document_.resolve(cell.style, nullptr);
        if (cell_style != style) {
          out_->report.add(ReportKind::UnsupportedStyle, cell_where, "per-cell style",
                           "the row style", RenderPath::Hardware,
                           "one style per row: the printer styles whole lines");
        }
      }
      const Wrap wrap = cell.wrap.value_or(style.wrap);
      cell_lines[i] = text::wrap(cell.content, plan.widths[i], wrap);
      rows = std::max(rows, cell_lines[i].size());
    }

    for (size_t row = 0; row < rows; ++row) {
      std::string line;
      for (size_t i = 0; i < block.cells.size(); ++i) {
        const std::string& content =
            row < cell_lines[i].size() ? cell_lines[i][row] : std::string();
        line += text::pad(content, plan.widths[i],
                          block.cells[i].align.value_or(Align::Left));
      }
      emitText(text::pad(line, line_columns, Align::Left), style, line_columns, true,
               where);
    }
  }

  // docs/receipt-dsl.md degradation rules: nothing silently disappears. A symbology
  // this build cannot emit, and data the symbology refuses, both leave a declared
  // entry and no bytes — never a half-drawn symbol, which would scan as something.
  void barcode(const Block& block, const std::string& where) {
    const std::string requested = std::string("barcode:") +
                                  to_string(block.symbology) + " \"" + block.content +
                                  "\"";
    if (!options_.profile.barcodes) {
      out_->report.add(ReportKind::UnsupportedBlock, where, requested, "omitted",
                       RenderPath::NotRendered, "this profile has no barcode support");
      return;
    }
    if (!isBarcodeSupported(block.symbology)) {
      out_->report.add(ReportKind::UnsupportedBlock, where, requested, "omitted",
                       RenderPath::NotRendered,
                       "the hardware path implements code128, ean13 and ean8");
      return;
    }
    const BarcodeEncoding encoding = encodeBarcode(block.symbology, block.content);
    if (!encoding.ok) {
      out_->report.add(ReportKind::UnsupportedBlock, where, requested, "omitted",
                       RenderPath::NotRendered, encoding.message);
      return;
    }

    bool clamped = false;
    std::vector<uint8_t> command =
        barcodeCommands(encoding, block.barcode_height_dots, block.barcode_module_width,
                        block.hri, &clamped);
    if (command.empty()) {
      out_->report.add(ReportKind::UnsupportedBlock, where, requested, "omitted",
                       RenderPath::NotRendered, "the symbol produced no command bytes");
      return;
    }
    if (clamped) {
      out_->report.add(ReportKind::Truncated, where,
                       "height " + std::to_string(block.barcode_height_dots) +
                           " dots, module width " +
                           std::to_string(block.barcode_module_width),
                       "clamped to the GS h / GS w operand ranges",
                       RenderPath::Hardware, "height 1..255, module width 1..6");
    }

    const int module_width = std::clamp(block.barcode_module_width, 1, 6);
    const int64_t symbol_dots = static_cast<int64_t>(encoding.modules) * module_width;
    if (symbol_dots > static_cast<int64_t>(options_.profile.width_dots)) {
      // Still emitted: the printer prints what it can, and an operator who can see the
      // clipped symbol and read this line knows to lower the module width.
      out_->report.add(ReportKind::Note, where, requested, "emitted", RenderPath::Hardware,
                       "about " + std::to_string(symbol_dots) + " dots wide at module " +
                           std::to_string(module_width) + ", wider than the " +
                           std::to_string(options_.profile.width_dots) + " dot media");
    }
    if (encoding.text != block.content) {
      // EAN with the check digit computed for the caller.
      out_->report.add(ReportKind::Note, where, requested, encoding.text,
                       RenderPath::Hardware, "check digit computed");
    }

    LayoutOp op;
    op.kind = LayoutOp::Kind::Barcode;
    op.source = where;
    op.style.align = block.align;
    op.symbology = block.symbology;
    op.barcode_height_dots = std::clamp(block.barcode_height_dots, 1, 255);
    op.barcode_module_width = module_width;
    op.hri = block.hri;
    op.barcode_command = std::move(command);
    op.barcode_text = encoding.text;
    out_->ops.push_back(std::move(op));
  }

  void image(const Block& block, const std::string& where) {
    if (!options_.profile.images) {
      out_->report.add(ReportKind::UnsupportedBlock, where, "image", "omitted",
                       RenderPath::NotRendered, "this profile has no raster support");
      return;
    }
    const std::vector<uint8_t>* gray = nullptr;
    uint32_t width = block.image_width;
    uint32_t height = block.image_height;
    if (!block.image_gray.empty()) {
      gray = &block.image_gray;
    } else if (!block.image_ref.empty()) {
      for (const ImageAsset& asset : options_.images) {
        if (asset.name == block.image_ref) {
          gray = &asset.gray;
          width = asset.width;
          height = asset.height;
          break;
        }
      }
    }
    if (gray == nullptr || width == 0 || height == 0 ||
        gray->size() < static_cast<size_t>(width) * height) {
      out_->report.add(ReportKind::MissingImage, where,
                       "image:" + (block.image_ref.empty() ? std::string("inline")
                                                           : block.image_ref),
                       "omitted", RenderPath::NotRendered,
                       gray == nullptr ? "no asset with that name"
                                       : "the asset dimensions do not match its data");
      return;
    }

    uint32_t target = width;
    switch (block.image_target.unit) {
      case CellWidth::Unit::Dots:
        target = static_cast<uint32_t>(std::max(1, block.image_target.value));
        break;
      case CellWidth::Unit::Percent:
        target = static_cast<uint32_t>(
            std::max<int64_t>(1, static_cast<int64_t>(options_.profile.width_dots) *
                                     std::max(1, block.image_target.value) / 100));
        break;
      case CellWidth::Unit::Chars:
        target = static_cast<uint32_t>(std::max(1, block.image_target.value)) *
                 options_.profile.font_a_char_dots;
        break;
      case CellWidth::Unit::Flex:
        break;
    }
    if (target > options_.profile.width_dots) {
      out_->report.add(ReportKind::Truncated, where, std::to_string(target) + " dots wide",
                       std::to_string(options_.profile.width_dots) + " dots",
                       RenderPath::Hardware, "wider than the media");
      target = options_.profile.width_dots;
    }

    LayoutOp op;
    op.kind = LayoutOp::Kind::Image;
    op.gray = *gray;
    op.image_width = width;
    op.image_height = height;
    op.target_width_dots = target;
    op.dither = block.dither;
    op.style.align = block.align;
    op.source = where;
    out_->ops.push_back(std::move(op));
  }

  const Document& document_;
  const RenderOptions& options_;
  LayoutOutput* out_;
};

// --- the byte emitter ----------------------------------------------------------------

// The Encoder tracks alignment, emphasis, underline and size itself. Everything else the
// styles need — font select, inverse, upside-down, line and letter spacing, left margin
// and print-area width — has no Encoder method, so it is emitted as raw command bytes
// and tracked here. Nothing in the core changes to support the DSL.
class Emitter {
 public:
  Emitter(escpos::Encoder* encoder, const RenderProfile& profile)
      : encoder_(encoder),
        profile_(profile),
        // The tracked state starts at the printer's power-on defaults, so a document
        // that asks for no margins emits no margin commands at all.
        area_width_(static_cast<int>(profile.width_dots)) {}

  void applyStyle(const ResolvedStyle& style) {
    const bool font_b = style.font == "printerB";
    if (font_b != font_b_) {
      font_b_ = font_b;
      put({kEsc, 0x4D, static_cast<uint8_t>(font_b ? 1 : 0)});  // ESC M n
    }
    if (style.inverse != inverse_) {
      inverse_ = style.inverse;
      put({kGs, 0x42, static_cast<uint8_t>(inverse_ ? 1 : 0)});  // GS B n
    }
    if (style.upside_down != upside_down_) {
      upside_down_ = style.upside_down;
      put({kEsc, 0x7B, static_cast<uint8_t>(upside_down_ ? 1 : 0)});  // ESC { n
    }
    const int line_spacing = std::clamp(style.line_spacing_dots, 0, 255);
    if (line_spacing != line_spacing_) {
      line_spacing_ = line_spacing;
      if (line_spacing == 0) {
        put({kEsc, 0x32});  // ESC 2 — back to the printer's default
      } else {
        put({kEsc, 0x33, static_cast<uint8_t>(line_spacing)});  // ESC 3 n
      }
    }
    const int letter_spacing = std::clamp(style.letter_spacing_dots, 0, 255);
    if (letter_spacing != letter_spacing_) {
      letter_spacing_ = letter_spacing;
      put({kEsc, 0x20, static_cast<uint8_t>(letter_spacing)});  // ESC SP n
    }
    const int left_margin = std::max(0, style.margin_left_dots + style.indent_dots);
    if (left_margin != left_margin_) {
      left_margin_ = left_margin;
      putWord(kGs, 0x4C, static_cast<uint32_t>(left_margin));  // GS L nL nH
    }
    const int area = static_cast<int>(profile_.width_dots) - left_margin -
                     std::max(0, style.margin_right_dots);
    const int width = std::max(static_cast<int>(profile_.charDots(style)), area);
    if (width != area_width_) {
      area_width_ = width;
      putWord(kGs, 0x57, static_cast<uint32_t>(width));  // GS W nL nH
    }
    encoder_->align(toEncoderAlignment(style.align));
    encoder_->bold(style.bold);
    encoder_->underline(underlineDots(style.underline));
    encoder_->textSize(static_cast<uint8_t>(std::clamp(style.width_scale, 1, 8)),
                       static_cast<uint8_t>(std::clamp(style.height_scale, 1, 8)));
  }

 private:
  void put(std::initializer_list<uint8_t> bytes) {
    encoder_->raw(escpos::Bytes(bytes));
  }
  void putWord(uint8_t a, uint8_t b, uint32_t value) {
    const uint32_t clamped = std::min<uint32_t>(value, 0xFFFF);
    encoder_->raw(escpos::Bytes{a, b, static_cast<uint8_t>(clamped & 0xFF),
                                static_cast<uint8_t>((clamped >> 8) & 0xFF)});
  }

  escpos::Encoder* encoder_;
  RenderProfile profile_;
  bool font_b_ = false;
  bool inverse_ = false;
  bool upside_down_ = false;
  int line_spacing_ = 0;
  int letter_spacing_ = 0;
  int left_margin_ = 0;
  int area_width_ = 0;
};

}  // namespace

// --- RenderProfile ------------------------------------------------------------------------

uint32_t RenderProfile::charDots(const ResolvedStyle& style) const noexcept {
  const uint32_t base = style.font == "printerB" ? font_b_char_dots : font_a_char_dots;
  const auto letter = static_cast<uint32_t>(std::max(0, style.letter_spacing_dots));
  const auto scale = static_cast<uint32_t>(std::clamp(style.width_scale, 1, 8));
  return std::max<uint32_t>(1, (base + letter) * scale);
}

uint32_t RenderProfile::charsPerLine(const ResolvedStyle& style) const noexcept {
  const uint32_t cell = charDots(style);
  const int64_t usable = static_cast<int64_t>(width_dots) -
                         std::max(0, style.margin_left_dots) -
                         std::max(0, style.margin_right_dots) -
                         std::max(0, style.indent_dots);
  if (usable < static_cast<int64_t>(cell)) {
    return 1;
  }
  return std::max<uint32_t>(1, static_cast<uint32_t>(usable) / cell);
}

uint32_t RenderProfile::dotsPerMm() const noexcept {
  return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(dpi / 25.4)));
}

RenderProfile RenderProfile::forWidth(uint32_t width_dots) {
  RenderProfile profile;
  profile.width_dots = width_dots == 0 ? escpos::kWidth80mm : width_dots;
  return profile;
}

RenderProfile RenderProfile::from(const CapabilityProfile& capability,
                                  uint32_t width_dots) {
  RenderProfile profile;
  profile.width_dots =
      width_dots != 0 ? width_dots : capability.media.printable_width_dots;
  profile.dpi = capability.media.dpi;
  profile.code_page = capability.code_page;
  profile.cutter = capability.media.cutter;
  profile.head_to_cutter_feed_dots = capability.media.head_to_cutter_feed_dots;
  // M15. Two independent questions, both of which have to be YES: does the firmware
  // implement the drawing command (CapabilityProfile::render), and is this path one
  // where an ESC/POS drawing command means anything at all? Star line mode has neither
  // GS k nor GS ( k, so a barcode block on a Star profile is a declared degradation
  // rather than a stream of literal bytes across half a receipt.
  const bool escpos_path = capability.language == CommandLanguage::EscPos;
  profile.barcodes = capability.render.barcode_gs_k && escpos_path;
  profile.qr = capability.render.qr_gs_paren_k && escpos_path;
  profile.images = capability.render.raster_gs_v;
  // `drawer_kick` is deliberately left alone: a `drawerKick` block is a document asking
  // for a pulse, and whether that pulse is *allowed* is docs/cash-drawer.md's question,
  // answered by Printer::openDrawer with the whole electrical classification behind it.
  // Mirroring it here would put half of that rule in a renderer.
  return profile;
}

// --- layout / render / preview --------------------------------------------------------

LayoutOutput layoutDocument(const Document& document, const RenderOptions& options) {
  LayoutOutput out;
  Layout layout(document, options, &out);
  layout.run();
  return out;
}

RenderOutput render(const Document& document, const RenderOptions& options) {
  LayoutOutput laid = layoutDocument(document, options);
  RenderOutput out;
  out.report = std::move(laid.report);
  out.requested_cut = laid.requested_cut;
  out.requested_margins = laid.requested_margins;

  if (options.emit_initialize) {
    out.ops.initialize();
  }
  if (options.emit_code_page) {
    out.ops.selectCodePage(options.profile.code_page);
  }
  Emitter emitter(&out.ops, options.profile);

  for (const LayoutOp& op : laid.ops) {
    switch (op.kind) {
      case LayoutOp::Kind::Text: {
        ResolvedStyle style = op.style;
        if (op.pre_padded) {
          // A padded row that the printer then centres would be centred twice.
          style.align = Align::Left;
        }
        emitter.applyStyle(style);
        out.ops.line(op.text);
        break;
      }
      case LayoutOp::Kind::Feed:
        if (op.feed_dots > 0) {
          out.ops.feedDots(static_cast<uint16_t>(std::min<uint32_t>(op.feed_dots, 0xFFFF)));
        } else if (op.feed_lines > 0) {
          out.ops.feedLines(static_cast<uint8_t>(op.feed_lines));
        }
        break;
      case LayoutOp::Kind::Qr:
        out.ops.align(toEncoderAlignment(op.style.align));
        out.ops.qr(op.payload, static_cast<uint8_t>(op.qr_size), toEncoderEc(op.qr_ec));
        break;
      case LayoutOp::Kind::Image:
        out.ops.align(toEncoderAlignment(op.style.align));
        out.ops.rasterGrayscale(op.gray.data(), op.image_width, op.image_height,
                                op.target_width_dots,
                                op.dither == Dither::FloydSteinberg
                                    ? escpos::Binarization::FloydSteinberg
                                    : escpos::Binarization::FixedThreshold,
                                128, escpos::RasterScale::Normal,
                                options.max_rows_per_band);
        break;
      case LayoutOp::Kind::DrawerKick:
        out.ops.kickCashDrawer(static_cast<uint8_t>(op.drawer_pin),
                               static_cast<uint8_t>(op.drawer_pulse),
                               static_cast<uint8_t>(op.drawer_pulse));
        break;
      case LayoutOp::Kind::Barcode:
        out.ops.align(toEncoderAlignment(op.style.align));
        out.ops.raw(op.barcode_command);
        break;
      case LayoutOp::Kind::Raw:
        out.ops.raw(op.raw);
        break;
      case LayoutOp::Kind::Cut:
        if (op.cut_clearance_dots > 0) {
          out.ops.feedDots(
              static_cast<uint16_t>(std::min<uint32_t>(op.cut_clearance_dots, 0xFFFF)));
        }
        out.ops.cut(op.cut == CutKind::Full ? escpos::CutMode::Full
                                            : escpos::CutMode::Partial);
        break;
    }
  }
  return out;
}

TextPreview renderText(const Document& document, const RenderOptions& options) {
  LayoutOutput laid = layoutDocument(document, options);
  TextPreview preview;
  preview.report = std::move(laid.report);
  preview.requested_cut = laid.requested_cut;
  preview.requested_margins = laid.requested_margins;

  for (const LayoutOp& op : laid.ops) {
    switch (op.kind) {
      case LayoutOp::Kind::Text:
        preview.lines.push_back(
            op.pre_padded ? op.text : text::pad(op.text, op.columns, op.style.align));
        break;
      case LayoutOp::Kind::Feed: {
        const uint32_t lines =
            op.feed_dots > 0 ? std::max<uint32_t>(1, op.feed_dots / 24) : op.feed_lines;
        for (uint32_t i = 0; i < lines; ++i) {
          preview.lines.emplace_back();
        }
        break;
      }
      case LayoutOp::Kind::Qr:
        preview.lines.push_back("[qr " + std::to_string(op.qr_size) + "/" +
                                to_string(op.qr_ec) + " \"" + op.payload + "\"]");
        break;
      case LayoutOp::Kind::Image:
        preview.lines.push_back("[image " + std::to_string(op.image_width) + "x" +
                                std::to_string(op.image_height) + " -> " +
                                std::to_string(op.target_width_dots) + " dots]");
        break;
      case LayoutOp::Kind::DrawerKick:
        preview.lines.push_back("[drawer kick pin " + std::to_string(op.drawer_pin) + "]");
        break;
      case LayoutOp::Kind::Barcode:
        preview.lines.push_back(std::string("[barcode ") + to_string(op.symbology) +
                                " \"" + op.barcode_text + "\" hri:" +
                                to_string(op.hri) + "]");
        break;
      case LayoutOp::Kind::Raw:
        preview.lines.push_back("[raw " + std::to_string(op.raw.size()) + " bytes]");
        break;
      case LayoutOp::Kind::Cut:
        // The clearance is part of what the cut does, so it rides on the cut's own
        // marker rather than becoming blank lines the paper will not actually show.
        preview.lines.push_back(
            std::string(op.cut == CutKind::Full ? "[cut full" : "[cut partial") +
            " after " + std::to_string(op.cut_clearance_dots) + " dots clearance]");
        break;
    }
  }
  return preview;
}

std::string TextPreview::text() const {
  std::string out;
  for (const std::string& line : lines) {
    out += line;
    out.push_back('\n');
  }
  return out;
}

}  // namespace pd::dsl
