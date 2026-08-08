#include <set>
#include <string>
#include <utility>
#include <vector>

#include "printerdriver/escpos_encoder.hpp"
#include "test_harness.hpp"

using namespace pd::escpos;

namespace {

// Codepoints are written numerically rather than as literal characters so the test
// does not depend on the source file's encoding surviving every editor and toolchain.
std::string utf8(uint32_t codepoint) {
  std::string out;
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

void checkMapping(const std::vector<std::pair<uint32_t, uint8_t>>& expectations) {
  for (const auto& [codepoint, expected] : expectations) {
    const Bytes encoded = transliterate(utf8(codepoint), CodePage::PC852);
    CHECK_EQ(encoded.size(), static_cast<size_t>(1));
    if (encoded.size() == 1) {
      CHECK_EQ(static_cast<unsigned>(encoded[0]), static_cast<unsigned>(expected));
    }
  }
}

}  // namespace

PD_TEST(ascii_passes_through_unchanged) {
  CHECK_BYTES(transliterate("Order 7F3A-92C1", CodePage::PC852), 'O', 'r', 'd', 'e',
              'r', ' ', '7', 'F', '3', 'A', '-', '9', '2', 'C', '1');
  CHECK_BYTES(transliterate("\t\n", CodePage::PC852), 0x09, 0x0A);
}

PD_TEST(cp852_czech_letters) {
  checkMapping({
      {0x011B, 0xD8},  // e-caron
      {0x0161, 0xE7},  // s-caron
      {0x010D, 0x9F},  // c-caron
      {0x0159, 0xFD},  // r-caron
      {0x017E, 0xA7},  // z-caron
      {0x00FD, 0xEC},  // y-acute
      {0x00E1, 0xA0},  // a-acute
      {0x00ED, 0xA1},  // i-acute
      {0x00E9, 0x82},  // e-acute
      {0x016F, 0x85},  // u-ring
      {0x00FA, 0xA3},  // u-acute
      {0x011A, 0xB7},  // E-caron
      {0x0160, 0xE6},  // S-caron
      {0x010C, 0xAC},  // C-caron
      {0x0158, 0xFC},  // R-caron
      {0x017D, 0xA6},  // Z-caron
      {0x00DD, 0xED},  // Y-acute
      {0x00C1, 0xB5},  // A-acute
      {0x00CD, 0xD6},  // I-acute
      {0x00C9, 0x90},  // E-acute
      {0x016E, 0xDE},  // U-ring
      {0x00DA, 0xE9},  // U-acute
      {0x010F, 0xD4},  // d-caron
      {0x0165, 0x9C},  // t-caron
      {0x0148, 0xE5},  // n-caron
      {0x00F3, 0xA2},  // o-acute
  });
}

PD_TEST(cp852_hungarian_double_acutes) {
  checkMapping({
      {0x0151, 0x8B},  // o-double-acute
      {0x0171, 0xFB},  // u-double-acute
      {0x0150, 0x8A},  // O-double-acute
      {0x0170, 0xEB},  // U-double-acute
  });
}

PD_TEST(cp852_polish_letters) {
  checkMapping({
      {0x0105, 0xA5},  // a-ogonek
      {0x0107, 0x86},  // c-acute
      {0x0119, 0xA9},  // e-ogonek
      {0x0142, 0x88},  // l-stroke
      {0x0144, 0xE4},  // n-acute
      {0x015B, 0x98},  // s-acute
      {0x017A, 0xAB},  // z-acute
      {0x017C, 0xBE},  // z-dot
      {0x0104, 0xA4},  // A-ogonek
      {0x0106, 0x8F},  // C-acute
      {0x0118, 0xA8},  // E-ogonek
      {0x0141, 0x9D},  // L-stroke
      {0x0143, 0xE3},  // N-acute
      {0x015A, 0x97},  // S-acute
      {0x0179, 0x8D},  // Z-acute
      {0x017B, 0xBD},  // Z-dot
  });
}

PD_TEST(cp852_umlauts) {
  checkMapping({
      {0x00F6, 0x94},  // o-diaeresis
      {0x00FC, 0x81},  // u-diaeresis
      {0x00D6, 0x99},  // O-diaeresis
      {0x00DC, 0x9A},  // U-diaeresis
      {0x00E4, 0x84},  // a-diaeresis
      {0x00C4, 0x8E},  // A-diaeresis
  });
}

PD_TEST(unmapped_codepoints_become_question_marks) {
  // Emoji: no CP852 representation, so lossy rather than an error.
  CHECK_BYTES(transliterate(utf8(0x1F600), CodePage::PC852), 0x3F);
  // Cyrillic and Greek are also outside CP852.
  CHECK_BYTES(transliterate(utf8(0x0416), CodePage::PC852), 0x3F);
  CHECK_BYTES(transliterate(utf8(0x03A9), CodePage::PC852), 0x3F);

  const std::string mixed = "A" + utf8(0x010D) + utf8(0x1F600) + "B";
  CHECK_BYTES(transliterate(mixed, CodePage::PC852), 'A', 0x9F, 0x3F, 'B');
}

PD_TEST(malformed_utf8_degrades_to_question_marks_and_resyncs) {
  // Lone continuation byte, then valid ASCII: one '?' and no desynchronisation.
  CHECK_BYTES(transliterate(std::string("\x80" "OK", 3), CodePage::PC852), 0x3F, 'O',
              'K');
  // Truncated two-byte sequence at end of input.
  CHECK_BYTES(transliterate(std::string("\xC4", 1), CodePage::PC852), 0x3F);
  // Truncated three-byte sequence followed by ASCII.
  CHECK_BYTES(transliterate(std::string("\xE2\x82" "Z", 3), CodePage::PC852), 0x3F,
              0x3F, 'Z');
  // Overlong encoding of '/'.
  CHECK_BYTES(transliterate(std::string("\xC0\xAF", 2), CodePage::PC852), 0x3F, 0x3F);
}

PD_TEST(cp852_high_half_is_a_bijection) {
  // Every byte 0x80..0xFF must be reachable from exactly one codepoint. This is the
  // property that makes the table a real code page rather than an ad-hoc list.
  std::set<uint8_t> produced;
  size_t mapped_codepoints = 0;
  for (uint32_t codepoint = 0x00A0; codepoint <= 0x2600; ++codepoint) {
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      continue;
    }
    const Bytes encoded = transliterate(utf8(codepoint), CodePage::PC852);
    if (encoded.size() != 1 || encoded[0] == 0x3F) {
      continue;
    }
    ++mapped_codepoints;
    CHECK(encoded[0] >= 0x80);
    produced.insert(encoded[0]);
  }
  CHECK_EQ(mapped_codepoints, static_cast<size_t>(128));
  CHECK_EQ(produced.size(), static_cast<size_t>(128));
  for (unsigned value = 0x80; value <= 0xFF; ++value) {
    CHECK(produced.count(static_cast<uint8_t>(value)) == 1);
  }
}

PD_TEST(non_cp852_code_pages_are_ascii_only_in_milestone_1) {
  CHECK_BYTES(transliterate("abc", CodePage::PC437), 'a', 'b', 'c');
  CHECK_BYTES(transliterate(utf8(0x010D), CodePage::PC437), 0x3F);
  CHECK_BYTES(transliterate(utf8(0x010D), CodePage::WPC1252), 0x3F);
}

PD_TEST(encoder_text_follows_the_selected_code_page) {
  Encoder encoder;
  // Default after ESC @ is PC437, which has no c-caron.
  encoder.text(utf8(0x010D));
  CHECK_BYTES(encoder.bytes(), 0x3F);

  encoder.clear();
  encoder.selectCodePage(CodePage::PC852).text(utf8(0x010D));
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x74, 0x12, 0x9F);
  CHECK_EQ(encoder.codePage(), CodePage::PC852);

  // Selecting the same page twice emits the command once.
  encoder.clear();
  encoder.selectCodePage(CodePage::PC852).selectCodePage(CodePage::PC852);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x74, 0x12);
}
