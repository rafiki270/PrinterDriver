#include "printerdriver/dsl/text.hpp"

#include <algorithm>

namespace pd::dsl::text {
namespace {

constexpr uint32_t kReplacement = 0xFFFD;

bool isContinuation(unsigned char byte) noexcept { return (byte & 0xC0) == 0x80; }

}  // namespace

std::vector<uint32_t> decode(std::string_view utf8) {
  std::vector<uint32_t> out;
  out.reserve(utf8.size());
  size_t i = 0;
  while (i < utf8.size()) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    size_t length = 0;
    uint32_t codepoint = 0;
    if (lead < 0x80) {
      length = 1;
      codepoint = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
      codepoint = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      codepoint = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      codepoint = lead & 0x07u;
    } else {
      out.push_back(kReplacement);
      ++i;
      continue;
    }

    if (i + length > utf8.size()) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }
    bool valid = true;
    for (size_t k = 1; k < length; ++k) {
      const auto byte = static_cast<unsigned char>(utf8[i + k]);
      if (!isContinuation(byte)) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (byte & 0x3Fu);
    }
    // Overlong encodings and surrogates are malformed, not exotic.
    if (!valid || (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }
    out.push_back(codepoint);
    i += length;
  }
  return out;
}

std::string encode(uint32_t codepoint) {
  std::string out;
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    codepoint = kReplacement;
  }
  if (codepoint < 0x80) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return out;
}

std::string encode(const std::vector<uint32_t>& codepoints) {
  std::string out;
  out.reserve(codepoints.size());
  for (const uint32_t codepoint : codepoints) {
    out += encode(codepoint);
  }
  return out;
}

size_t columnWidth(uint32_t codepoint) noexcept {
  // Combining marks sit on the previous cell.
  if ((codepoint >= 0x0300 && codepoint <= 0x036F) ||
      (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
      (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
      (codepoint >= 0xFE20 && codepoint <= 0xFE2F)) {
    return 0;
  }
  // The East Asian wide/fullwidth blocks a receipt can realistically carry.
  if ((codepoint >= 0x1100 && codepoint <= 0x115F) ||
      (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
      (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
      (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
      (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
      (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
      (codepoint >= 0xFFE0 && codepoint <= 0xFFE6)) {
    return 2;
  }
  return 1;
}

size_t width(std::string_view utf8) {
  size_t total = 0;
  for (const uint32_t codepoint : decode(utf8)) {
    total += columnWidth(codepoint);
  }
  return total;
}

std::string clip(std::string_view utf8, size_t columns) {
  if (columns == 0) {
    return std::string();
  }
  std::string out;
  size_t used = 0;
  for (const uint32_t codepoint : decode(utf8)) {
    const size_t w = columnWidth(codepoint);
    if (used + w > columns) {
      break;
    }
    used += w;
    out += encode(codepoint);
  }
  return out;
}

std::string ellipsize(std::string_view utf8, size_t columns) {
  if (width(utf8) <= columns) {
    return std::string(utf8);
  }
  if (columns <= 3) {
    return clip(utf8, columns);
  }
  return clip(utf8, columns - 3) + "...";
}

std::string pad(std::string_view utf8, size_t columns, Align align) {
  if (columns == 0) {
    return std::string();
  }
  std::string clipped = clip(utf8, columns);
  const size_t slack = columns - width(clipped);
  if (slack == 0) {
    return clipped;
  }
  switch (align) {
    case Align::Left:
      return clipped + std::string(slack, ' ');
    case Align::Right:
      return std::string(slack, ' ') + clipped;
    case Align::Center: {
      const size_t leading = slack / 2;
      return std::string(leading, ' ') + clipped + std::string(slack - leading, ' ');
    }
  }
  return clipped;
}

namespace {

// One paragraph (no '\n' inside), broken to `columns`.
void wrapParagraph(const std::string& paragraph, size_t columns, Wrap mode,
                   std::vector<std::string>* out) {
  if (mode == Wrap::Clip) {
    out->push_back(clip(paragraph, columns));
    return;
  }
  if (mode == Wrap::Ellipsis) {
    out->push_back(ellipsize(paragraph, columns));
    return;
  }
  if (paragraph.empty()) {
    out->push_back(std::string());
    return;
  }
  if (mode == Wrap::Char) {
    std::string current;
    size_t used = 0;
    for (const uint32_t codepoint : decode(paragraph)) {
      const size_t w = columnWidth(codepoint);
      if (used + w > columns && !current.empty()) {
        out->push_back(current);
        current.clear();
        used = 0;
      }
      current += encode(codepoint);
      used += w;
    }
    out->push_back(current);
    return;
  }

  // Word mode: greedy fill, hard-breaking any single token that cannot fit.
  //
  // Leading spaces are an indent, not a separator: the spec's own nested-group example
  // writes "  + {{name}}" to indent a modifier under its item, so the run is preserved
  // and every continuation line of that paragraph keeps it (a hanging indent).
  size_t indent = 0;
  while (indent < paragraph.size() && paragraph[indent] == ' ') {
    ++indent;
  }
  if (indent > 0 && indent < columns) {
    const std::string prefix(indent, ' ');
    std::vector<std::string> inner;
    wrapParagraph(paragraph.substr(indent), columns - indent, mode, &inner);
    for (const std::string& line : inner) {
      out->push_back(prefix + line);
    }
    return;
  }

  std::string current;
  size_t used = 0;
  size_t position = 0;
  while (position <= paragraph.size()) {
    const size_t space = paragraph.find(' ', position);
    std::string token = paragraph.substr(
        position, space == std::string::npos ? std::string::npos : space - position);
    position = space == std::string::npos ? paragraph.size() + 1 : space + 1;
    if (token.empty()) {
      continue;
    }
    size_t token_width = width(token);
    if (token_width > columns) {
      if (!current.empty()) {
        out->push_back(current);
        current.clear();
        used = 0;
      }
      while (width(token) > columns) {
        const std::string head = clip(token, columns);
        if (head.empty()) {
          break;  // A single glyph wider than the line: nothing can be emitted.
        }
        out->push_back(head);
        token = token.substr(head.size());
      }
      current = token;
      used = width(token);
      continue;
    }
    if (current.empty()) {
      current = token;
      used = token_width;
    } else if (used + 1 + token_width <= columns) {
      current += ' ';
      current += token;
      used += 1 + token_width;
    } else {
      out->push_back(current);
      current = token;
      used = token_width;
    }
  }
  out->push_back(current);
}

}  // namespace

std::vector<std::string> wrap(std::string_view utf8, size_t columns, Wrap mode) {
  std::vector<std::string> lines;
  if (columns == 0) {
    lines.emplace_back();
    return lines;
  }
  size_t start = 0;
  while (true) {
    const size_t newline = utf8.find('\n', start);
    const std::string paragraph(
        utf8.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                            : newline - start));
    wrapParagraph(paragraph, columns, mode, &lines);
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

std::string repeat(std::string_view unit, size_t columns) {
  if (columns == 0 || unit.empty()) {
    return std::string();
  }
  const size_t unit_width = std::max<size_t>(1, width(unit));
  std::string out;
  while (width(out) + unit_width <= columns) {
    out += std::string(unit);
  }
  if (width(out) < columns) {
    out += clip(unit, columns - width(out));
  }
  return out;
}

namespace {

uint32_t upperCodepoint(uint32_t c) noexcept {
  if (c >= 'a' && c <= 'z') {
    return c - 32;
  }
  // Latin-1 Supplement: 0xE0..0xFE minus the division sign.
  if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) {
    return c - 32;
  }
  if (c == 0x00FF) {
    return 0x0178;
  }
  // Latin Extended-A: even/odd upper/lower pairs, with the documented exceptions.
  if (c >= 0x0100 && c <= 0x0137) {
    return (c % 2 == 1) ? c - 1 : c;
  }
  if (c >= 0x0139 && c <= 0x0148) {
    return (c % 2 == 0) ? c - 1 : c;
  }
  if (c >= 0x014A && c <= 0x0177) {
    return (c % 2 == 1) ? c - 1 : c;
  }
  if (c >= 0x0179 && c <= 0x017E) {
    return (c % 2 == 0) ? c - 1 : c;
  }
  if (c == 0x017F) {
    return 'S';
  }
  return c;
}

uint32_t lowerCodepoint(uint32_t c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return c + 32;
  }
  if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) {
    return c + 32;
  }
  if (c == 0x0178) {
    return 0x00FF;
  }
  if (c >= 0x0100 && c <= 0x0137) {
    return (c % 2 == 0) ? c + 1 : c;
  }
  if (c >= 0x0139 && c <= 0x0148) {
    return (c % 2 == 1) ? c + 1 : c;
  }
  if (c >= 0x014A && c <= 0x0177) {
    return (c % 2 == 0) ? c + 1 : c;
  }
  if (c >= 0x0179 && c <= 0x017E) {
    return (c % 2 == 1) ? c + 1 : c;
  }
  return c;
}

}  // namespace

std::string upper(std::string_view utf8) {
  std::vector<uint32_t> codepoints = decode(utf8);
  for (uint32_t& codepoint : codepoints) {
    codepoint = upperCodepoint(codepoint);
  }
  return encode(codepoints);
}

std::string lower(std::string_view utf8) {
  std::vector<uint32_t> codepoints = decode(utf8);
  for (uint32_t& codepoint : codepoints) {
    codepoint = lowerCodepoint(codepoint);
  }
  return encode(codepoints);
}

std::string sanitize(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (const char raw : utf8) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte == '\n' || byte == '\t') {
      out.push_back(raw);
      continue;
    }
    if (byte < 0x20 || byte == 0x7F) {
      continue;
    }
    out.push_back(raw);
  }
  return out;
}

}  // namespace pd::dsl::text
