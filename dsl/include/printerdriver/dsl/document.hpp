#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "printerdriver/dsl/json.hpp"
#include "printerdriver/dsl/report.hpp"
#include "printerdriver/dsl/text.hpp"

// The receipt document model of docs/receipt-dsl.md: named styles plus a vertical list
// of blocks, serializable to the canonical JSON that travels between a POS and an agent.
//
// Two model-shaping decisions, both deliberate:
//
// 1. **Style fields are optional, not defaulted.** A document that says nothing about
//    underline is not the same document as one that says underline is off — the first
//    inherits, the second overrides. Keeping std::optional is what makes named-style
//    inheritance and inline override expressible at all, and what makes the JSON
//    round-trip faithful instead of merely equivalent.
//
// 2. **A Block is a tagged aggregate, not a variant.** `each` and `if` carry a block
//    *list* (docs/receipt-dsl.md "Repeating groups"), so the type is recursive; a plain
//    struct with a kind tag stays copyable, assignable and trivially serializable, which
//    a recursive variant does not.
//
// Parsing is strict about structure and forgiving about vocabulary: an unknown key is
// reported through the optional warnings vector rather than rejected, so a template
// written against a later revision of the spec still prints what this build understands.

namespace pd::dsl {

class DocumentError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class Underline { None, Single, Double };
enum class DividerKind { Solid, Dashed, Double, Char };
enum class Dither { FloydSteinberg, Threshold, None };
enum class Hri { None, Above, Below, Both };
enum class QrEc { L, M, Q, H };
enum class CutKind { Partial, Full };

// docs/receipt-dsl.md "Cut control": JobOptions > document meta > printer profile.
enum class CutRequest { Profile, Partial, Full, None };

// docs/receipt-dsl.md block table, barcode row.
enum class Symbology {
  UpcA, UpcE, Ean13, Ean8, Code39, Itf, Codabar, Code93, Code128, Gs1_128, Pdf417,
  DataMatrix,
};

// --- Styles ---------------------------------------------------------------------

// docs/receipt-dsl.md "TextStyle — the full parameter set". Every field is optional:
// absence means "inherit", presence means "override".
struct TextStyle {
  std::optional<std::string> extends;  // named-style inheritance
  std::optional<std::string> font;     // printerA | printerB | a raster font name
  std::optional<int> width_scale;      // 1-8
  std::optional<int> height_scale;     // 1-8
  std::optional<bool> bold;
  std::optional<bool> italic;
  std::optional<bool> inverse;
  std::optional<bool> upside_down;
  std::optional<bool> rotate90;
  std::optional<Underline> underline;
  std::optional<Align> align;
  std::optional<int> line_spacing_dots;
  std::optional<int> letter_spacing_dots;
  std::optional<Wrap> wrap;
  std::optional<int> indent_dots;
  std::optional<int> margin_left_dots;
  std::optional<int> margin_right_dots;

  bool empty() const noexcept;
  // Fields present in `over` win; fields absent there keep this style's value.
  void overlay(const TextStyle& over);
};

// The same parameters with every question answered, which is what a renderer needs.
struct ResolvedStyle {
  std::string font = "printerA";
  int width_scale = 1;
  int height_scale = 1;
  bool bold = false;
  bool italic = false;
  bool inverse = false;
  bool upside_down = false;
  bool rotate90 = false;
  Underline underline = Underline::None;
  Align align = Align::Left;
  int line_spacing_dots = 0;    // 0 = the printer's own default (ESC 2)
  int letter_spacing_dots = 0;
  Wrap wrap = Wrap::Word;
  int indent_dots = 0;
  int margin_left_dots = 0;
  int margin_right_dots = 0;

  bool operator==(const ResolvedStyle& other) const noexcept;
  bool operator!=(const ResolvedStyle& other) const noexcept { return !(*this == other); }
};

// docs/receipt-dsl.md: "blocks reference by name and may override inline". A reference
// is a name, an inline style, or both — `{"style": {"extends": "h1", "bold": true}}`.
struct StyleRef {
  std::optional<std::string> name;
  TextStyle inline_style;

  bool empty() const noexcept { return !name.has_value() && inline_style.empty(); }
};

// --- Blocks ---------------------------------------------------------------------

// docs/receipt-dsl.md columns: width: chars | dots | flex(n). Also used for image
// widths, where Percent applies instead of Chars.
struct CellWidth {
  enum class Unit { Flex, Chars, Dots, Percent };
  Unit unit = Unit::Flex;
  int value = 1;  // flex weight, character count, dot count or percentage
};

struct Cell {
  std::string content;
  CellWidth width;
  std::optional<Align> align;
  std::optional<Wrap> wrap;
  StyleRef style;
};

struct Block;
using BlockList = std::vector<Block>;

struct Block {
  enum class Kind {
    Text, Columns, Image, Qr, Barcode, Divider, Feed, Cut, DrawerKick, Raw, Each, If,
  };

  Kind kind = Kind::Text;

  // Text
  std::string content;
  StyleRef style;

  // Columns
  std::vector<Cell> cells;

  // Divider
  DividerKind divider = DividerKind::Solid;
  std::string divider_char;  // when divider == Char
  std::optional<int> thickness_dots;

  // Feed: lines by default, dots when set.
  int feed_lines = 1;
  std::optional<int> feed_dots;

  // Cut
  CutKind cut = CutKind::Partial;

  // Drawer kick: ESC p pin (0 or 1) and pulse in 2 ms units.
  int drawer_pin = 0;
  int drawer_pulse = 25;

  // QR
  int qr_size = 6;
  QrEc qr_ec = QrEc::M;
  Align align = Align::Left;

  // Barcode
  Symbology symbology = Symbology::Code128;
  int barcode_height_dots = 64;
  int barcode_module_width = 2;
  Hri hri = Hri::None;

  // Image: either a named asset supplied at render time, or inline 8-bit grayscale.
  std::string image_ref;
  std::vector<uint8_t> image_gray;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  CellWidth image_target;  // Dots or Percent
  Dither dither = Dither::FloydSteinberg;

  // Raw escape hatch.
  std::vector<uint8_t> raw;

  // Control flow (Each / If). `negated` is `unless`; `body_is_single` remembers whether
  // the source said "block" or "blocks", so the round-trip keeps the author's shape.
  std::string path;
  bool negated = false;
  bool body_is_single = false;
  BlockList body;
};

// --- Document -------------------------------------------------------------------

// docs/receipt-dsl.md "Margins": dots, or millimetres converted at the profile's dpi.
struct Margins {
  std::optional<int> top_dots;
  std::optional<int> bottom_dots;
  std::optional<double> top_mm;
  std::optional<double> bottom_mm;

  bool any() const noexcept {
    return top_dots || bottom_dots || top_mm || bottom_mm;
  }
};

struct Meta {
  std::string locale;    // "cs-CZ"; empty → the renderer's default
  std::string currency;  // "CZK"
  std::string tz;        // fixed offset ("+02:00") or an IANA name (see format.hpp)
  std::optional<CutRequest> cut;
  Margins margins;
};

struct Document {
  int version = 1;
  bool is_template = false;
  Meta meta;
  // Insertion-ordered so serialization is stable.
  std::vector<std::pair<std::string, TextStyle>> styles;
  BlockList blocks;

  const TextStyle* style(std::string_view name) const noexcept;
  void setStyle(std::string name, TextStyle style);

  // Resolves default ← extends chain ← named style ← inline override. An unknown name
  // or an inheritance cycle is reported and falls back rather than throwing:
  // a mistyped style name must not lose a ticket.
  ResolvedStyle resolve(const StyleRef& ref, RenderReport* report = nullptr,
                        const std::string& where = {}) const;
};

// Structural failures (a block with no recognised key, a style that is not an object)
// throw DocumentError. Unknown keys are appended to `warnings` when supplied.
Document parseDocument(const Json& json, std::vector<std::string>* warnings = nullptr);
Document parseDocument(std::string_view json, std::vector<std::string>* warnings = nullptr);

Json documentToJson(const Document& document);
std::string serializeDocument(const Document& document, bool pretty = false);

const char* to_string(Align value) noexcept;
const char* to_string(Wrap value) noexcept;
const char* to_string(Underline value) noexcept;
const char* to_string(DividerKind value) noexcept;
const char* to_string(Dither value) noexcept;
const char* to_string(Hri value) noexcept;
const char* to_string(QrEc value) noexcept;
const char* to_string(CutRequest value) noexcept;
const char* to_string(Symbology value) noexcept;
const char* to_string(Block::Kind value) noexcept;

}  // namespace pd::dsl
