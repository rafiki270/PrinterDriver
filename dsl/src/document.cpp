#include "printerdriver/dsl/document.hpp"

#include <algorithm>
#include <set>

namespace pd::dsl {
namespace {

// --- base64, for inline rasters and raw byte blocks --------------------------------

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < bytes.size()) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    out.push_back(kBase64Alphabet[triple & 0x3F]);
    i += 3;
  }
  const size_t remaining = bytes.size() - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out += "==";
  } else if (remaining == 2) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64Decode(const std::string& text, std::vector<uint8_t>* out) {
  uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    const int value = base64Value(c);
    if (value < 0) {
      return false;
    }
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return true;
}

// --- enum vocabulary ---------------------------------------------------------------

std::string lowered(std::string_view value) {
  std::string out(value);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

template <typename T>
bool lookup(std::string_view value, std::initializer_list<std::pair<const char*, T>> table,
            T* out) {
  const std::string key = lowered(value);
  for (const auto& entry : table) {
    if (key == entry.first) {
      *out = entry.second;
      return true;
    }
  }
  return false;
}

bool parseAlign(std::string_view value, Align* out) {
  return lookup<Align>(value,
                       {{"left", Align::Left},
                        {"center", Align::Center},
                        {"centre", Align::Center},
                        {"right", Align::Right}},
                       out);
}

bool parseWrap(std::string_view value, Wrap* out) {
  return lookup<Wrap>(value,
                      {{"word", Wrap::Word},
                       {"char", Wrap::Char},
                       {"clip", Wrap::Clip},
                       {"ellipsis", Wrap::Ellipsis}},
                      out);
}

bool parseUnderline(std::string_view value, Underline* out) {
  return lookup<Underline>(value,
                           {{"none", Underline::None},
                            {"single", Underline::Single},
                            {"double", Underline::Double}},
                           out);
}

bool parseDither(std::string_view value, Dither* out) {
  return lookup<Dither>(value,
                        {{"fs", Dither::FloydSteinberg},
                         {"floydsteinberg", Dither::FloydSteinberg},
                         {"threshold", Dither::Threshold},
                         {"none", Dither::None}},
                        out);
}

bool parseHri(std::string_view value, Hri* out) {
  return lookup<Hri>(value,
                     {{"none", Hri::None},
                      {"above", Hri::Above},
                      {"below", Hri::Below},
                      {"both", Hri::Both}},
                     out);
}

bool parseQrEc(std::string_view value, QrEc* out) {
  return lookup<QrEc>(value, {{"l", QrEc::L}, {"m", QrEc::M}, {"q", QrEc::Q}, {"h", QrEc::H}},
                      out);
}

bool parseSymbology(std::string_view value, Symbology* out) {
  return lookup<Symbology>(value,
                           {{"upca", Symbology::UpcA},
                            {"upce", Symbology::UpcE},
                            {"ean13", Symbology::Ean13},
                            {"ean8", Symbology::Ean8},
                            {"code39", Symbology::Code39},
                            {"itf", Symbology::Itf},
                            {"codabar", Symbology::Codabar},
                            {"code93", Symbology::Code93},
                            {"code128", Symbology::Code128},
                            {"gs1_128", Symbology::Gs1_128},
                            {"gs1-128", Symbology::Gs1_128},
                            {"pdf417", Symbology::Pdf417},
                            {"datamatrix", Symbology::DataMatrix}},
                           out);
}

bool parseCutRequest(std::string_view value, CutRequest* out) {
  return lookup<CutRequest>(value,
                            {{"profile", CutRequest::Profile},
                             {"partial", CutRequest::Partial},
                             {"full", CutRequest::Full},
                             {"none", CutRequest::None}},
                            out);
}

// --- reader ------------------------------------------------------------------------

// Tracks which keys were consumed so anything left over can be reported rather than
// silently ignored: a typo in a template is a defect worth naming.
class Reader {
 public:
  Reader(const Json& value, std::string where, std::vector<std::string>* warnings)
      : value_(value), where_(std::move(where)), warnings_(warnings) {}

  const Json* take(std::string_view key) {
    seen_.insert(std::string(key));
    return value_.find(key);
  }

  void ignore(std::string_view key) { seen_.insert(std::string(key)); }

  void finish() {
    if (warnings_ == nullptr || !value_.isObject()) {
      return;
    }
    for (const Json::Member& member : value_.asObject()) {
      if (seen_.find(member.first) == seen_.end()) {
        warnings_->push_back(where_ + ": unknown key \"" + member.first + "\" ignored");
      }
    }
  }

  const std::string& where() const noexcept { return where_; }
  std::vector<std::string>* warnings() const noexcept { return warnings_; }

  void warn(const std::string& message) {
    if (warnings_ != nullptr) {
      warnings_->push_back(where_ + ": " + message);
    }
  }

 private:
  const Json& value_;
  std::string where_;
  std::vector<std::string>* warnings_;
  std::set<std::string> seen_;
};

std::optional<int> readInt(Reader* reader, std::string_view key) {
  const Json* value = reader->take(key);
  if (value == nullptr || !value->isNumber()) {
    return std::nullopt;
  }
  return static_cast<int>(value->asInt());
}

std::optional<bool> readBool(Reader* reader, std::string_view key) {
  const Json* value = reader->take(key);
  if (value == nullptr || !value->isBool()) {
    return std::nullopt;
  }
  return value->asBool();
}

std::string readString(Reader* reader, std::string_view key) {
  const Json* value = reader->take(key);
  return value != nullptr && value->isString() ? value->asString() : std::string();
}

TextStyle parseTextStyle(const Json& json, const std::string& where,
                         std::vector<std::string>* warnings) {
  TextStyle style;
  if (!json.isObject()) {
    throw DocumentError(where + ": a style must be a JSON object");
  }
  Reader reader(json, where, warnings);

  if (const Json* value = reader.take("extends"); value != nullptr && value->isString()) {
    style.extends = value->asString();
  }
  if (const Json* value = reader.take("font"); value != nullptr && value->isString()) {
    style.font = value->asString();
  }
  style.width_scale = readInt(&reader, "widthScale");
  style.height_scale = readInt(&reader, "heightScale");
  style.bold = readBool(&reader, "bold");
  style.italic = readBool(&reader, "italic");
  style.inverse = readBool(&reader, "inverse");
  style.upside_down = readBool(&reader, "upsideDown");
  style.rotate90 = readBool(&reader, "rotate90");
  style.line_spacing_dots = readInt(&reader, "lineSpacingDots");
  style.letter_spacing_dots = readInt(&reader, "letterSpacingDots");
  style.indent_dots = readInt(&reader, "indentDots");
  style.margin_left_dots = readInt(&reader, "marginLeftDots");
  style.margin_right_dots = readInt(&reader, "marginRightDots");

  if (const Json* value = reader.take("underline"); value != nullptr) {
    Underline underline = Underline::None;
    if (value->isString() && parseUnderline(value->asString(), &underline)) {
      style.underline = underline;
    } else if (value->isBool()) {
      style.underline = value->asBool() ? Underline::Single : Underline::None;
    } else if (value->isNumber()) {
      const long long dots = value->asInt();
      style.underline = dots >= 2 ? Underline::Double
                                  : (dots == 1 ? Underline::Single : Underline::None);
    } else {
      reader.warn("unknown underline value ignored");
    }
  }
  if (const Json* value = reader.take("align"); value != nullptr && value->isString()) {
    Align align = Align::Left;
    if (parseAlign(value->asString(), &align)) {
      style.align = align;
    } else {
      reader.warn("unknown align \"" + value->asString() + "\" ignored");
    }
  }
  if (const Json* value = reader.take("wrap"); value != nullptr && value->isString()) {
    Wrap wrap = Wrap::Word;
    if (parseWrap(value->asString(), &wrap)) {
      style.wrap = wrap;
    } else {
      reader.warn("unknown wrap \"" + value->asString() + "\" ignored");
    }
  }
  reader.finish();
  return style;
}

StyleRef parseStyleRef(const Json* json, const std::string& where,
                       std::vector<std::string>* warnings) {
  StyleRef ref;
  if (json == nullptr) {
    return ref;
  }
  if (json->isString()) {
    ref.name = json->asString();
    return ref;
  }
  if (json->isObject()) {
    ref.inline_style = parseTextStyle(*json, where, warnings);
    return ref;
  }
  throw DocumentError(where + ": style must be a name or an inline style object");
}

CellWidth parseCellWidth(const Json* json, const std::string& where, bool allow_percent) {
  CellWidth width;
  if (json == nullptr) {
    return width;
  }
  if (json->isString()) {
    if (lowered(json->asString()) == "flex") {
      width.unit = CellWidth::Unit::Flex;
      width.value = 1;
      return width;
    }
    throw DocumentError(where + ": unknown width \"" + json->asString() + "\"");
  }
  if (json->isNumber()) {
    width.unit = allow_percent ? CellWidth::Unit::Dots : CellWidth::Unit::Chars;
    width.value = static_cast<int>(json->asInt());
    return width;
  }
  if (json->isObject()) {
    if (const Json* value = json->find("chars"); value != nullptr) {
      width.unit = CellWidth::Unit::Chars;
      width.value = static_cast<int>(value->asInt());
      return width;
    }
    if (const Json* value = json->find("dots"); value != nullptr) {
      width.unit = CellWidth::Unit::Dots;
      width.value = static_cast<int>(value->asInt());
      return width;
    }
    if (allow_percent) {
      if (const Json* value = json->find("percent"); value != nullptr) {
        width.unit = CellWidth::Unit::Percent;
        width.value = static_cast<int>(value->asInt());
        return width;
      }
    }
    if (const Json* value = json->find("flex"); value != nullptr) {
      width.unit = CellWidth::Unit::Flex;
      width.value = std::max(1, static_cast<int>(value->asInt()));
      return width;
    }
  }
  throw DocumentError(where + ": width must be \"flex\", {chars|dots|flex: n}" +
                      (allow_percent ? " or {percent: n}" : ""));
}

std::vector<uint8_t> parseBytes(const Json& json, const std::string& where) {
  std::vector<uint8_t> bytes;
  if (json.isString()) {
    if (!base64Decode(json.asString(), &bytes)) {
      throw DocumentError(where + ": not valid base64");
    }
    return bytes;
  }
  if (json.isArray()) {
    for (const Json& item : json.asArray()) {
      if (!item.isNumber()) {
        throw DocumentError(where + ": byte arrays must contain numbers only");
      }
      const long long value = item.asInt();
      if (value < 0 || value > 255) {
        throw DocumentError(where + ": byte out of range");
      }
      bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
  }
  throw DocumentError(where + ": expected base64 or an array of bytes");
}

Block parseBlock(const Json& json, const std::string& where,
                 std::vector<std::string>* warnings, size_t depth,
                 const ::pd::Registrations* registrations);

BlockList parseBody(Reader* reader, Block* block, const std::string& where,
                    std::vector<std::string>* warnings, size_t depth,
                    const ::pd::Registrations* registrations) {
  BlockList body;
  const Json* single = reader->take("block");
  const Json* many = reader->take("blocks");
  if (single != nullptr) {
    block->body_is_single = true;
    body.push_back(
        parseBlock(*single, where + ".block", warnings, depth + 1, registrations));
    return body;
  }
  if (many != nullptr) {
    if (!many->isArray()) {
      throw DocumentError(where + ".blocks: expected an array");
    }
    size_t index = 0;
    for (const Json& item : many->asArray()) {
      body.push_back(parseBlock(item, where + ".blocks[" + std::to_string(index) + "]",
                                warnings, depth + 1, registrations));
      ++index;
    }
    return body;
  }
  throw DocumentError(where + ": needs \"block\" or \"blocks\"");
}

Block parseBlock(const Json& json, const std::string& where,
                 std::vector<std::string>* warnings, size_t depth,
                 const ::pd::Registrations* registrations) {
  if (depth > 32) {
    throw DocumentError(where + ": blocks nested deeper than 32 levels");
  }
  if (!json.isObject()) {
    throw DocumentError(where + ": a block must be a JSON object");
  }
  Block block;
  Reader reader(json, where, warnings);

  // Control blocks first: `each`/`if`/`unless` wrap other blocks and are the only
  // shapes whose discriminating key is not the payload itself.
  if (const Json* value = reader.take("each"); value != nullptr) {
    block.kind = Block::Kind::Each;
    block.path = value->asString();
    block.body = parseBody(&reader, &block, where, warnings, depth, registrations);
    reader.finish();
    return block;
  }
  const Json* if_path = reader.take("if");
  const Json* unless_path = reader.take("unless");
  if (if_path != nullptr || unless_path != nullptr) {
    block.kind = Block::Kind::If;
    block.negated = unless_path != nullptr;
    block.path = block.negated ? unless_path->asString() : if_path->asString();
    block.body = parseBody(&reader, &block, where, warnings, depth, registrations);
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("text"); value != nullptr) {
    block.kind = Block::Kind::Text;
    block.content = value->asString();
    block.style = parseStyleRef(reader.take("style"), where + ".style", warnings);
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("columns"); value != nullptr) {
    block.kind = Block::Kind::Columns;
    if (!value->isArray()) {
      throw DocumentError(where + ".columns: expected an array of cells");
    }
    size_t index = 0;
    for (const Json& item : value->asArray()) {
      const std::string cell_where = where + ".cells[" + std::to_string(index) + "]";
      if (!item.isObject()) {
        throw DocumentError(cell_where + ": a cell must be a JSON object");
      }
      Reader cell_reader(item, cell_where, warnings);
      Cell cell;
      cell.content = readString(&cell_reader, "content");
      cell.width = parseCellWidth(cell_reader.take("width"), cell_where + ".width", false);
      if (const Json* align = cell_reader.take("align");
          align != nullptr && align->isString()) {
        Align parsed = Align::Left;
        if (parseAlign(align->asString(), &parsed)) {
          cell.align = parsed;
        } else {
          cell_reader.warn("unknown align \"" + align->asString() + "\" ignored");
        }
      }
      if (const Json* wrap = cell_reader.take("wrap"); wrap != nullptr && wrap->isString()) {
        Wrap parsed = Wrap::Word;
        if (parseWrap(wrap->asString(), &parsed)) {
          cell.wrap = parsed;
        } else {
          cell_reader.warn("unknown wrap \"" + wrap->asString() + "\" ignored");
        }
      }
      cell.style = parseStyleRef(cell_reader.take("style"), cell_where + ".style", warnings);
      cell_reader.finish();
      block.cells.push_back(std::move(cell));
      ++index;
    }
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("divider"); value != nullptr) {
    block.kind = Block::Kind::Divider;
    if (value->isString()) {
      const std::string& name = value->asString();
      DividerKind kind = DividerKind::Solid;
      if (lookup<DividerKind>(name,
                              {{"solid", DividerKind::Solid},
                               {"dashed", DividerKind::Dashed},
                               {"double", DividerKind::Double}},
                              &kind)) {
        block.divider = kind;
      } else if (text::width(name) == 1) {
        block.divider = DividerKind::Char;
        block.divider_char = name;
      } else {
        reader.warn("unknown divider \"" + name + "\", using solid");
      }
    } else if (value->isObject()) {
      Reader divider_reader(*value, where + ".divider", warnings);
      if (const Json* style = divider_reader.take("style");
          style != nullptr && style->isString()) {
        DividerKind kind = DividerKind::Solid;
        if (lookup<DividerKind>(style->asString(),
                                {{"solid", DividerKind::Solid},
                                 {"dashed", DividerKind::Dashed},
                                 {"double", DividerKind::Double},
                                 {"char", DividerKind::Char}},
                                &kind)) {
          block.divider = kind;
        } else {
          divider_reader.warn("unknown divider style, using solid");
        }
      }
      if (const Json* fill = divider_reader.take("char"); fill != nullptr && fill->isString()) {
        block.divider = DividerKind::Char;
        block.divider_char = fill->asString();
      }
      block.thickness_dots = readInt(&divider_reader, "thicknessDots");
      divider_reader.finish();
    } else {
      throw DocumentError(where + ".divider: expected a name or an object");
    }
    block.style = parseStyleRef(reader.take("style"), where + ".style", warnings);
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("feed"); value != nullptr) {
    block.kind = Block::Kind::Feed;
    if (value->isNumber()) {
      block.feed_lines = static_cast<int>(value->asInt());
    } else if (value->isObject()) {
      if (const Json* lines = value->find("lines"); lines != nullptr) {
        block.feed_lines = static_cast<int>(lines->asInt());
      }
      if (const Json* dots = value->find("dots"); dots != nullptr) {
        block.feed_dots = static_cast<int>(dots->asInt());
      }
    } else {
      throw DocumentError(where + ".feed: expected a line count or {lines|dots: n}");
    }
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("cut"); value != nullptr) {
    block.kind = Block::Kind::Cut;
    if (value->isString()) {
      block.cut = lowered(value->asString()) == "full" ? CutKind::Full : CutKind::Partial;
    } else if (value->isBool()) {
      block.cut = CutKind::Partial;
    } else {
      throw DocumentError(where + ".cut: expected \"partial\" or \"full\"");
    }
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("drawerKick"); value != nullptr) {
    block.kind = Block::Kind::DrawerKick;
    if (value->isObject()) {
      if (const Json* pin = value->find("pin"); pin != nullptr) {
        block.drawer_pin = static_cast<int>(pin->asInt());
      }
      if (const Json* pulse = value->find("pulse"); pulse != nullptr) {
        block.drawer_pulse = static_cast<int>(pulse->asInt());
      }
    }
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("raw"); value != nullptr) {
    block.kind = Block::Kind::Raw;
    block.raw = parseBytes(*value, where + ".raw");
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("qr"); value != nullptr) {
    block.kind = Block::Kind::Qr;
    block.content = value->asString();
    if (const std::optional<int> size = readInt(&reader, "size"); size.has_value()) {
      block.qr_size = *size;
    }
    if (const Json* ec = reader.take("ec"); ec != nullptr && ec->isString()) {
      QrEc parsed = QrEc::M;
      if (parseQrEc(ec->asString(), &parsed)) {
        block.qr_ec = parsed;
      } else {
        reader.warn("unknown QR error correction \"" + ec->asString() + "\", using M");
      }
    }
    if (const Json* align = reader.take("align"); align != nullptr && align->isString()) {
      Align parsed = Align::Left;
      if (parseAlign(align->asString(), &parsed)) {
        block.align = parsed;
      }
    }
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("barcode"); value != nullptr) {
    block.kind = Block::Kind::Barcode;
    block.content = value->asString();
    if (const Json* symbology = reader.take("symbology");
        symbology != nullptr && symbology->isString()) {
      Symbology parsed = Symbology::Code128;
      if (parseSymbology(symbology->asString(), &parsed)) {
        block.symbology = parsed;
      } else {
        reader.warn("unknown symbology \"" + symbology->asString() + "\", using code128");
      }
    }
    if (const std::optional<int> height = readInt(&reader, "height"); height.has_value()) {
      block.barcode_height_dots = *height;
    }
    if (const std::optional<int> module = readInt(&reader, "moduleWidth");
        module.has_value()) {
      block.barcode_module_width = *module;
    }
    if (const Json* hri = reader.take("hri"); hri != nullptr && hri->isString()) {
      Hri parsed = Hri::None;
      if (parseHri(hri->asString(), &parsed)) {
        block.hri = parsed;
      }
    }
    if (const Json* align = reader.take("align"); align != nullptr && align->isString()) {
      Align parsed = Align::Left;
      if (parseAlign(align->asString(), &parsed)) {
        block.align = parsed;
      }
    }
    reader.finish();
    return block;
  }

  if (const Json* value = reader.take("image"); value != nullptr) {
    block.kind = Block::Kind::Image;
    if (value->isString()) {
      block.image_ref = value->asString();
    } else if (value->isObject()) {
      Reader image_reader(*value, where + ".image", warnings);
      block.image_ref = readString(&image_reader, "ref");
      if (const Json* gray = image_reader.take("gray"); gray != nullptr) {
        block.image_gray = parseBytes(*gray, where + ".image.gray");
      }
      if (const std::optional<int> w = readInt(&image_reader, "width"); w.has_value()) {
        block.image_width = static_cast<uint32_t>(std::max(0, *w));
      }
      if (const std::optional<int> h = readInt(&image_reader, "height"); h.has_value()) {
        block.image_height = static_cast<uint32_t>(std::max(0, *h));
      }
      image_reader.finish();
    } else {
      throw DocumentError(where + ".image: expected an asset name or an object");
    }
    block.image_target = parseCellWidth(reader.take("width"), where + ".width", true);
    if (const Json* align = reader.take("align"); align != nullptr && align->isString()) {
      Align parsed = Align::Left;
      if (parseAlign(align->asString(), &parsed)) {
        block.align = parsed;
      }
    }
    if (const Json* dither = reader.take("dither"); dither != nullptr && dither->isString()) {
      Dither parsed = Dither::FloydSteinberg;
      if (parseDither(dither->asString(), &parsed)) {
        block.dither = parsed;
      }
    }
    reader.finish();
    return block;
  }

  // M16 (docs/api.md §16). Before failing an unrecognised block, offer its key to the
  // driver's registered block handlers. A block like {"loyaltyStamp": {...}} whose key is
  // registered becomes a Custom block carrying the whole object as JSON, to be rendered by
  // the handler at render time (which is where the profile it needs is available). With no
  // registry, or no handler for the key, the strict parse failure below still stands.
  if (registrations != nullptr) {
    for (const Json::Member& member : json.asObject()) {
      if (registrations->hasBlockHandler(member.first)) {
        block.kind = Block::Kind::Custom;
        block.custom_kind = member.first;
        block.custom_payload = json;
        return block;
      }
    }
  }

  throw DocumentError(where + ": block carries no recognised key");
}

// --- serialization -------------------------------------------------------------------

Json styleToJson(const TextStyle& style) {
  Json out = Json::object({});
  if (style.extends) out.set("extends", Json::string(*style.extends));
  if (style.font) out.set("font", Json::string(*style.font));
  if (style.width_scale) out.set("widthScale", Json::number(*style.width_scale));
  if (style.height_scale) out.set("heightScale", Json::number(*style.height_scale));
  if (style.bold) out.set("bold", Json::boolean(*style.bold));
  if (style.italic) out.set("italic", Json::boolean(*style.italic));
  if (style.underline) out.set("underline", Json::string(to_string(*style.underline)));
  if (style.inverse) out.set("inverse", Json::boolean(*style.inverse));
  if (style.upside_down) out.set("upsideDown", Json::boolean(*style.upside_down));
  if (style.rotate90) out.set("rotate90", Json::boolean(*style.rotate90));
  if (style.align) out.set("align", Json::string(to_string(*style.align)));
  if (style.wrap) out.set("wrap", Json::string(to_string(*style.wrap)));
  if (style.line_spacing_dots)
    out.set("lineSpacingDots", Json::number(*style.line_spacing_dots));
  if (style.letter_spacing_dots)
    out.set("letterSpacingDots", Json::number(*style.letter_spacing_dots));
  if (style.indent_dots) out.set("indentDots", Json::number(*style.indent_dots));
  if (style.margin_left_dots) out.set("marginLeftDots", Json::number(*style.margin_left_dots));
  if (style.margin_right_dots)
    out.set("marginRightDots", Json::number(*style.margin_right_dots));
  return out;
}

void writeStyleRef(Json* out, const StyleRef& ref) {
  if (ref.name.has_value() && ref.inline_style.empty()) {
    out->set("style", Json::string(*ref.name));
    return;
  }
  if (!ref.inline_style.empty()) {
    Json inline_style = styleToJson(ref.inline_style);
    if (ref.name.has_value()) {
      // A name plus an override serializes as the extends form, which is the shape
      // that reads back as the same reference.
      Json merged = Json::object({});
      merged.set("extends", Json::string(*ref.name));
      for (const Json::Member& member : inline_style.asObject()) {
        merged.set(member.first, member.second);
      }
      inline_style = std::move(merged);
    }
    out->set("style", std::move(inline_style));
  }
}

Json cellWidthToJson(const CellWidth& width) {
  switch (width.unit) {
    case CellWidth::Unit::Flex:
      if (width.value <= 1) {
        return Json::string("flex");
      }
      return Json::object({{"flex", Json::number(width.value)}});
    case CellWidth::Unit::Chars:
      return Json::object({{"chars", Json::number(width.value)}});
    case CellWidth::Unit::Dots:
      return Json::object({{"dots", Json::number(width.value)}});
    case CellWidth::Unit::Percent:
      return Json::object({{"percent", Json::number(width.value)}});
  }
  return Json::string("flex");
}

Json blockToJson(const Block& block);

Json blockListToJson(const BlockList& blocks) {
  Json::Array items;
  items.reserve(blocks.size());
  for (const Block& block : blocks) {
    items.push_back(blockToJson(block));
  }
  return Json::array(std::move(items));
}

Json blockToJson(const Block& block) {
  Json out = Json::object({});
  switch (block.kind) {
    case Block::Kind::Text:
      out.set("text", Json::string(block.content));
      writeStyleRef(&out, block.style);
      return out;
    case Block::Kind::Columns: {
      Json::Array cells;
      cells.reserve(block.cells.size());
      for (const Cell& cell : block.cells) {
        Json item = Json::object({});
        item.set("content", Json::string(cell.content));
        item.set("width", cellWidthToJson(cell.width));
        if (cell.align) item.set("align", Json::string(to_string(*cell.align)));
        if (cell.wrap) item.set("wrap", Json::string(to_string(*cell.wrap)));
        writeStyleRef(&item, cell.style);
        cells.push_back(std::move(item));
      }
      out.set("columns", Json::array(std::move(cells)));
      return out;
    }
    case Block::Kind::Divider:
      if (block.divider == DividerKind::Char) {
        Json divider = Json::object({});
        divider.set("style", Json::string("char"));
        divider.set("char", Json::string(block.divider_char));
        if (block.thickness_dots)
          divider.set("thicknessDots", Json::number(*block.thickness_dots));
        out.set("divider", std::move(divider));
      } else if (block.thickness_dots) {
        Json divider = Json::object({});
        divider.set("style", Json::string(to_string(block.divider)));
        divider.set("thicknessDots", Json::number(*block.thickness_dots));
        out.set("divider", std::move(divider));
      } else {
        out.set("divider", Json::string(to_string(block.divider)));
      }
      writeStyleRef(&out, block.style);
      return out;
    case Block::Kind::Feed:
      if (block.feed_dots) {
        out.set("feed", Json::object({{"dots", Json::number(*block.feed_dots)}}));
      } else {
        out.set("feed", Json::number(block.feed_lines));
      }
      return out;
    case Block::Kind::Cut:
      out.set("cut", Json::string(block.cut == CutKind::Full ? "full" : "partial"));
      return out;
    case Block::Kind::DrawerKick: {
      Json kick = Json::object({});
      kick.set("pin", Json::number(block.drawer_pin));
      kick.set("pulse", Json::number(block.drawer_pulse));
      out.set("drawerKick", std::move(kick));
      return out;
    }
    case Block::Kind::Raw:
      out.set("raw", Json::string(base64Encode(block.raw)));
      return out;
    case Block::Kind::Qr:
      out.set("qr", Json::string(block.content));
      out.set("size", Json::number(block.qr_size));
      out.set("ec", Json::string(to_string(block.qr_ec)));
      if (block.align != Align::Left) {
        out.set("align", Json::string(to_string(block.align)));
      }
      return out;
    case Block::Kind::Barcode:
      out.set("barcode", Json::string(block.content));
      out.set("symbology", Json::string(to_string(block.symbology)));
      out.set("height", Json::number(block.barcode_height_dots));
      out.set("moduleWidth", Json::number(block.barcode_module_width));
      out.set("hri", Json::string(to_string(block.hri)));
      if (block.align != Align::Left) {
        out.set("align", Json::string(to_string(block.align)));
      }
      return out;
    case Block::Kind::Image: {
      if (block.image_gray.empty() && !block.image_ref.empty()) {
        out.set("image", Json::string(block.image_ref));
      } else {
        Json image = Json::object({});
        if (!block.image_ref.empty()) image.set("ref", Json::string(block.image_ref));
        if (!block.image_gray.empty()) {
          image.set("gray", Json::string(base64Encode(block.image_gray)));
          image.set("width", Json::number(block.image_width));
          image.set("height", Json::number(block.image_height));
        }
        out.set("image", std::move(image));
      }
      if (block.image_target.unit != CellWidth::Unit::Flex) {
        out.set("width", cellWidthToJson(block.image_target));
      }
      if (block.align != Align::Left) {
        out.set("align", Json::string(to_string(block.align)));
      }
      out.set("dither", Json::string(to_string(block.dither)));
      return out;
    }
    case Block::Kind::Each:
      out.set("each", Json::string(block.path));
      if (block.body_is_single && block.body.size() == 1) {
        out.set("block", blockToJson(block.body.front()));
      } else {
        out.set("blocks", blockListToJson(block.body));
      }
      return out;
    case Block::Kind::If:
      out.set(block.negated ? "unless" : "if", Json::string(block.path));
      if (block.body_is_single && block.body.size() == 1) {
        out.set("block", blockToJson(block.body.front()));
      } else {
        out.set("blocks", blockListToJson(block.body));
      }
      return out;
    case Block::Kind::Custom:
      // M16. Round-trips as the original block object the handler was handed.
      return block.custom_payload.isObject() ? block.custom_payload : out;
  }
  return out;
}

}  // namespace

// --- TextStyle ---------------------------------------------------------------------

bool TextStyle::empty() const noexcept {
  return !extends && !font && !width_scale && !height_scale && !bold && !italic &&
         !inverse && !upside_down && !rotate90 && !underline && !align &&
         !line_spacing_dots && !letter_spacing_dots && !wrap && !indent_dots &&
         !margin_left_dots && !margin_right_dots;
}

void TextStyle::overlay(const TextStyle& over) {
  if (over.font) font = over.font;
  if (over.width_scale) width_scale = over.width_scale;
  if (over.height_scale) height_scale = over.height_scale;
  if (over.bold) bold = over.bold;
  if (over.italic) italic = over.italic;
  if (over.inverse) inverse = over.inverse;
  if (over.upside_down) upside_down = over.upside_down;
  if (over.rotate90) rotate90 = over.rotate90;
  if (over.underline) underline = over.underline;
  if (over.align) align = over.align;
  if (over.line_spacing_dots) line_spacing_dots = over.line_spacing_dots;
  if (over.letter_spacing_dots) letter_spacing_dots = over.letter_spacing_dots;
  if (over.wrap) wrap = over.wrap;
  if (over.indent_dots) indent_dots = over.indent_dots;
  if (over.margin_left_dots) margin_left_dots = over.margin_left_dots;
  if (over.margin_right_dots) margin_right_dots = over.margin_right_dots;
  // `extends` is deliberately not inherited: it describes where this style came from,
  // not what it looks like.
}

bool ResolvedStyle::operator==(const ResolvedStyle& other) const noexcept {
  return font == other.font && width_scale == other.width_scale &&
         height_scale == other.height_scale && bold == other.bold &&
         italic == other.italic && inverse == other.inverse &&
         upside_down == other.upside_down && rotate90 == other.rotate90 &&
         underline == other.underline && align == other.align &&
         line_spacing_dots == other.line_spacing_dots &&
         letter_spacing_dots == other.letter_spacing_dots && wrap == other.wrap &&
         indent_dots == other.indent_dots && margin_left_dots == other.margin_left_dots &&
         margin_right_dots == other.margin_right_dots;
}

// --- Document ------------------------------------------------------------------------

const TextStyle* Document::style(std::string_view name) const noexcept {
  for (const auto& entry : styles) {
    if (entry.first == name) {
      return &entry.second;
    }
  }
  return nullptr;
}

void Document::setStyle(std::string name, TextStyle value) {
  for (auto& entry : styles) {
    if (entry.first == name) {
      entry.second = std::move(value);
      return;
    }
  }
  styles.emplace_back(std::move(name), std::move(value));
}

namespace {

void applyChain(const Document& document, const std::string& name, TextStyle* into,
                std::set<std::string>* visited, RenderReport* report,
                const std::string& where) {
  const TextStyle* named = document.style(name);
  if (named == nullptr) {
    if (report != nullptr) {
      report->add(ReportKind::UnknownStyle, where, "style:" + name, "default",
                  RenderPath::Hardware, "no named style with that name");
    }
    return;
  }
  if (!visited->insert(name).second) {
    if (report != nullptr) {
      report->add(ReportKind::StyleCycle, where, "extends:" + name, "chain broken",
                  RenderPath::Hardware, "named style inheritance loops");
    }
    return;
  }
  if (named->extends.has_value()) {
    applyChain(document, *named->extends, into, visited, report, where);
  }
  into->overlay(*named);
}

int clampScale(int value, RenderReport* report, const std::string& where,
               const char* what) {
  if (value >= 1 && value <= 8) {
    return value;
  }
  const int clamped = value < 1 ? 1 : 8;
  if (report != nullptr) {
    report->add(ReportKind::UnsupportedStyle, where,
                std::string(what) + ":" + std::to_string(value),
                std::string(what) + ":" + std::to_string(clamped), RenderPath::Hardware,
                "GS ! multipliers are 1..8");
  }
  return clamped;
}

}  // namespace

ResolvedStyle Document::resolve(const StyleRef& ref, RenderReport* report,
                                const std::string& where) const {
  TextStyle merged;
  std::set<std::string> visited;
  if (const TextStyle* fallback = style("default"); fallback != nullptr) {
    visited.insert("default");
    if (fallback->extends.has_value()) {
      applyChain(*this, *fallback->extends, &merged, &visited, report, where);
    }
    merged.overlay(*fallback);
  }
  if (ref.name.has_value()) {
    applyChain(*this, *ref.name, &merged, &visited, report, where);
  }
  if (ref.inline_style.extends.has_value()) {
    applyChain(*this, *ref.inline_style.extends, &merged, &visited, report, where);
  }
  merged.overlay(ref.inline_style);

  ResolvedStyle resolved;
  if (merged.font) resolved.font = *merged.font;
  if (merged.width_scale)
    resolved.width_scale = clampScale(*merged.width_scale, report, where, "widthScale");
  if (merged.height_scale)
    resolved.height_scale = clampScale(*merged.height_scale, report, where, "heightScale");
  if (merged.bold) resolved.bold = *merged.bold;
  if (merged.italic) resolved.italic = *merged.italic;
  if (merged.inverse) resolved.inverse = *merged.inverse;
  if (merged.upside_down) resolved.upside_down = *merged.upside_down;
  if (merged.rotate90) resolved.rotate90 = *merged.rotate90;
  if (merged.underline) resolved.underline = *merged.underline;
  if (merged.align) resolved.align = *merged.align;
  if (merged.line_spacing_dots) resolved.line_spacing_dots = *merged.line_spacing_dots;
  if (merged.letter_spacing_dots) resolved.letter_spacing_dots = *merged.letter_spacing_dots;
  if (merged.wrap) resolved.wrap = *merged.wrap;
  if (merged.indent_dots) resolved.indent_dots = *merged.indent_dots;
  if (merged.margin_left_dots) resolved.margin_left_dots = *merged.margin_left_dots;
  if (merged.margin_right_dots) resolved.margin_right_dots = *merged.margin_right_dots;
  return resolved;
}

// --- parse / serialize -----------------------------------------------------------------

Document parseDocument(const Json& json, std::vector<std::string>* warnings,
                       const ::pd::Registrations* registrations) {
  if (!json.isObject()) {
    throw DocumentError("document: expected a JSON object");
  }
  Document document;
  Reader reader(json, "document", warnings);

  if (const Json* value = reader.take("v"); value != nullptr && value->isNumber()) {
    document.version = static_cast<int>(value->asInt());
  }
  if (const Json* value = reader.take("template"); value != nullptr) {
    document.is_template = value->truthy();
  }

  if (const Json* meta = reader.take("meta"); meta != nullptr) {
    if (!meta->isObject()) {
      throw DocumentError("document.meta: expected an object");
    }
    Reader meta_reader(*meta, "document.meta", warnings);
    document.meta.locale = readString(&meta_reader, "locale");
    document.meta.currency = readString(&meta_reader, "currency");
    document.meta.tz = readString(&meta_reader, "tz");
    if (const Json* cut = meta_reader.take("cut"); cut != nullptr) {
      if (cut->isBool()) {
        // docs/receipt-dsl.md: `"cut": false` is how a kitchen-summary template carries
        // "do not cut" itself; true means "whatever the profile does".
        document.meta.cut = cut->asBool() ? CutRequest::Profile : CutRequest::None;
      } else if (cut->isString()) {
        CutRequest request = CutRequest::Profile;
        if (parseCutRequest(cut->asString(), &request)) {
          document.meta.cut = request;
        } else {
          meta_reader.warn("unknown cut \"" + cut->asString() + "\" ignored");
        }
      } else {
        meta_reader.warn("cut must be a boolean or a name");
      }
    }
    if (const Json* margins = meta_reader.take("margins"); margins != nullptr) {
      if (!margins->isObject()) {
        throw DocumentError("document.meta.margins: expected an object");
      }
      Reader margin_reader(*margins, "document.meta.margins", warnings);
      document.meta.margins.top_dots = readInt(&margin_reader, "topDots");
      document.meta.margins.bottom_dots = readInt(&margin_reader, "bottomDots");
      if (const Json* value = margin_reader.take("topMm");
          value != nullptr && value->isNumber()) {
        document.meta.margins.top_mm = value->asNumber();
      }
      if (const Json* value = margin_reader.take("bottomMm");
          value != nullptr && value->isNumber()) {
        document.meta.margins.bottom_mm = value->asNumber();
      }
      margin_reader.finish();
    }
    meta_reader.finish();
  }

  if (const Json* styles = reader.take("styles"); styles != nullptr) {
    if (!styles->isObject()) {
      throw DocumentError("document.styles: expected an object of named styles");
    }
    for (const Json::Member& member : styles->asObject()) {
      document.styles.emplace_back(
          member.first,
          parseTextStyle(member.second, "styles." + member.first, warnings));
    }
  }

  if (const Json* blocks = reader.take("blocks"); blocks != nullptr) {
    if (!blocks->isArray()) {
      throw DocumentError("document.blocks: expected an array");
    }
    size_t index = 0;
    for (const Json& item : blocks->asArray()) {
      document.blocks.push_back(parseBlock(
          item, "blocks[" + std::to_string(index) + "]", warnings, 0, registrations));
      ++index;
    }
  }

  reader.finish();
  return document;
}

Document parseDocument(std::string_view json, std::vector<std::string>* warnings,
                       const ::pd::Registrations* registrations) {
  return parseDocument(parseJson(json), warnings, registrations);
}

Json documentToJson(const Document& document) {
  Json out = Json::object({});
  out.set("v", Json::number(document.version));
  if (document.is_template) {
    out.set("template", Json::boolean(true));
  }

  Json meta = Json::object({});
  if (!document.meta.locale.empty()) meta.set("locale", Json::string(document.meta.locale));
  if (!document.meta.currency.empty())
    meta.set("currency", Json::string(document.meta.currency));
  if (!document.meta.tz.empty()) meta.set("tz", Json::string(document.meta.tz));
  if (document.meta.cut.has_value()) {
    meta.set("cut", Json::string(to_string(*document.meta.cut)));
  }
  if (document.meta.margins.any()) {
    Json margins = Json::object({});
    if (document.meta.margins.top_dots)
      margins.set("topDots", Json::number(*document.meta.margins.top_dots));
    if (document.meta.margins.bottom_dots)
      margins.set("bottomDots", Json::number(*document.meta.margins.bottom_dots));
    if (document.meta.margins.top_mm)
      margins.set("topMm", Json::number(*document.meta.margins.top_mm));
    if (document.meta.margins.bottom_mm)
      margins.set("bottomMm", Json::number(*document.meta.margins.bottom_mm));
    meta.set("margins", std::move(margins));
  }
  if (meta.size() != 0) {
    out.set("meta", std::move(meta));
  }

  if (!document.styles.empty()) {
    Json styles = Json::object({});
    for (const auto& entry : document.styles) {
      styles.set(entry.first, styleToJson(entry.second));
    }
    out.set("styles", std::move(styles));
  }

  out.set("blocks", blockListToJson(document.blocks));
  return out;
}

std::string serializeDocument(const Document& document, bool pretty) {
  return toJson(documentToJson(document), pretty);
}

// --- enum names -------------------------------------------------------------------------

const char* to_string(Align value) noexcept {
  switch (value) {
    case Align::Left: return "left";
    case Align::Center: return "center";
    case Align::Right: return "right";
  }
  return "left";
}

const char* to_string(Wrap value) noexcept {
  switch (value) {
    case Wrap::Word: return "word";
    case Wrap::Char: return "char";
    case Wrap::Clip: return "clip";
    case Wrap::Ellipsis: return "ellipsis";
  }
  return "word";
}

const char* to_string(Underline value) noexcept {
  switch (value) {
    case Underline::None: return "none";
    case Underline::Single: return "single";
    case Underline::Double: return "double";
  }
  return "none";
}

const char* to_string(DividerKind value) noexcept {
  switch (value) {
    case DividerKind::Solid: return "solid";
    case DividerKind::Dashed: return "dashed";
    case DividerKind::Double: return "double";
    case DividerKind::Char: return "char";
  }
  return "solid";
}

const char* to_string(Dither value) noexcept {
  switch (value) {
    case Dither::FloydSteinberg: return "fs";
    case Dither::Threshold: return "threshold";
    case Dither::None: return "none";
  }
  return "fs";
}

const char* to_string(Hri value) noexcept {
  switch (value) {
    case Hri::None: return "none";
    case Hri::Above: return "above";
    case Hri::Below: return "below";
    case Hri::Both: return "both";
  }
  return "none";
}

const char* to_string(QrEc value) noexcept {
  switch (value) {
    case QrEc::L: return "L";
    case QrEc::M: return "M";
    case QrEc::Q: return "Q";
    case QrEc::H: return "H";
  }
  return "M";
}

const char* to_string(CutRequest value) noexcept {
  switch (value) {
    case CutRequest::Profile: return "profile";
    case CutRequest::Partial: return "partial";
    case CutRequest::Full: return "full";
    case CutRequest::None: return "none";
  }
  return "profile";
}

const char* to_string(Symbology value) noexcept {
  switch (value) {
    case Symbology::UpcA: return "upcA";
    case Symbology::UpcE: return "upcE";
    case Symbology::Ean13: return "ean13";
    case Symbology::Ean8: return "ean8";
    case Symbology::Code39: return "code39";
    case Symbology::Itf: return "itf";
    case Symbology::Codabar: return "codabar";
    case Symbology::Code93: return "code93";
    case Symbology::Code128: return "code128";
    case Symbology::Gs1_128: return "gs1_128";
    case Symbology::Pdf417: return "pdf417";
    case Symbology::DataMatrix: return "datamatrix";
  }
  return "code128";
}

const char* to_string(Block::Kind value) noexcept {
  switch (value) {
    case Block::Kind::Text: return "text";
    case Block::Kind::Columns: return "columns";
    case Block::Kind::Image: return "image";
    case Block::Kind::Qr: return "qr";
    case Block::Kind::Barcode: return "barcode";
    case Block::Kind::Divider: return "divider";
    case Block::Kind::Feed: return "feed";
    case Block::Kind::Cut: return "cut";
    case Block::Kind::DrawerKick: return "drawerKick";
    case Block::Kind::Raw: return "raw";
    case Block::Kind::Each: return "each";
    case Block::Kind::If: return "if";
    case Block::Kind::Custom: return "custom";
  }
  return "text";
}

}  // namespace pd::dsl
