#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// UTF-8 text measurement and line fitting for the receipt DSL
// (docs/receipt-dsl.md, TextStyle `wrap` and the `columns` block).
//
// A thermal receipt is laid out in *character cells*, not pixels, so every layout
// decision in this library is taken in columns. Multi-byte UTF-8 sequences count as
// one column (two for the East Asian wide ranges), because counting bytes is what
// makes a "48 character" line come out at 40 the moment somebody types "2× Pilsner".
//
// Alignment and wrapping live here rather than in document.hpp: they are properties of
// fitting text to a width, and both the encoder path and the text preview need them.

namespace pd::dsl {

enum class Align { Left, Center, Right };

// docs/receipt-dsl.md TextStyle: wrap: word|char|clip|ellipsis.
enum class Wrap { Word, Char, Clip, Ellipsis };

namespace text {

// Decodes UTF-8, replacing every malformed byte with U+FFFD. Never throws and never
// loses length: a mangled model value still prints something.
std::vector<uint32_t> decode(std::string_view utf8);
std::string encode(uint32_t codepoint);
std::string encode(const std::vector<uint32_t>& codepoints);

// 0 for combining marks, 2 for the East Asian wide ranges, 1 for everything else.
size_t columnWidth(uint32_t codepoint) noexcept;
size_t width(std::string_view utf8);

// Hard cut at `columns` display columns. A wide glyph that would straddle the edge is
// dropped rather than half-printed.
std::string clip(std::string_view utf8, size_t columns);
// Same, but the last three columns become "..." when anything was cut. ASCII rather
// than U+2026 because the ellipsis codepoint does not survive a single-byte code page.
std::string ellipsize(std::string_view utf8, size_t columns);
// Pads (or clips) to exactly `columns` columns.
std::string pad(std::string_view utf8, size_t columns, Align align);

// Line breaking. '\n' is always an explicit break, in every mode. Word mode breaks on
// spaces and hard-breaks any single token longer than the line; Char mode breaks at the
// column edge; Clip and Ellipsis produce one line per paragraph.
std::vector<std::string> wrap(std::string_view utf8, size_t columns, Wrap mode);

// Repeats `unit` until exactly `columns` columns are filled, clipping the last unit.
std::string repeat(std::string_view unit, size_t columns);

// ASCII, Latin-1 Supplement and Latin Extended-A case mapping — enough for the Czech,
// Slovak, Polish and Western European receipts this SDK targets, and no ICU dependency.
std::string upper(std::string_view utf8);
std::string lower(std::string_view utf8);

// Removes C0 control bytes (keeping '\n' and '\t') and DEL. Applied to every value
// substituted into a template, so a model field can never inject ESC/POS commands into
// a receipt built from a trusted layout.
std::string sanitize(std::string_view utf8);

}  // namespace text
}  // namespace pd::dsl
