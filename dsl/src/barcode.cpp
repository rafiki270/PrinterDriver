#include "printerdriver/dsl/barcode.hpp"

#include <algorithm>
#include <cctype>

namespace pd::dsl {
namespace {

constexpr uint8_t kGs = 0x1D;

// GS k function-B `m` operands.
constexpr uint8_t kEan13 = 67;
constexpr uint8_t kEan8 = 68;
constexpr uint8_t kCode128 = 73;

// `n` is a single byte.
constexpr size_t kMaxDataBytes = 255;

bool allDigits(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return false;
    }
  }
  return true;
}

BarcodeEncoding failure(BarcodeError error, std::string message) {
  BarcodeEncoding out;
  out.ok = false;
  out.error = error;
  out.message = std::move(message);
  return out;
}

size_t digitRun(std::string_view data, size_t from) noexcept {
  size_t run = 0;
  while (from + run < data.size() &&
         std::isdigit(static_cast<unsigned char>(data[from + run])) != 0) {
    ++run;
  }
  return run;
}

BarcodeEncoding encodeEan(std::string_view digits, size_t body, uint8_t m,
                          const char* name, int modules) {
  const size_t total = body + 1;
  if (!allDigits(digits)) {
    return failure(BarcodeError::InvalidData,
                   std::string(name) + " takes digits only");
  }
  std::string full;
  if (digits.size() == body) {
    // The printer would compute the check digit itself, but then nobody could report
    // what it is — and an operator comparing paper against an article number needs it.
    const int check = eanCheckDigit(digits);
    full = std::string(digits) + static_cast<char>('0' + check);
  } else if (digits.size() == total) {
    const std::string_view head = digits.substr(0, body);
    const int check = eanCheckDigit(head);
    if (digits[body] - '0' != check) {
      return failure(BarcodeError::InvalidData,
                     std::string(name) + " check digit is " +
                         std::to_string(digits[body] - '0') + ", expected " +
                         std::to_string(check));
    }
    full = std::string(digits);
  } else {
    return failure(BarcodeError::InvalidData,
                   std::string(name) + " takes " + std::to_string(body) + " or " +
                       std::to_string(total) + " digits, got " +
                       std::to_string(digits.size()));
  }

  BarcodeEncoding out;
  out.ok = true;
  out.m = m;
  out.data.assign(full.begin(), full.end());
  out.text = full;
  out.modules = modules;
  return out;
}

}  // namespace

const char* to_string(BarcodeError value) noexcept {
  switch (value) {
    case BarcodeError::None: return "none";
    case BarcodeError::UnsupportedSymbology: return "unsupported symbology";
    case BarcodeError::InvalidData: return "invalid data";
    case BarcodeError::TooLong: return "too long";
  }
  return "unknown";
}

bool isBarcodeSupported(Symbology symbology) noexcept {
  switch (symbology) {
    case Symbology::Code128:
    case Symbology::Ean13:
    case Symbology::Ean8:
      return true;
    case Symbology::UpcA:
    case Symbology::UpcE:
    case Symbology::Code39:
    case Symbology::Itf:
    case Symbology::Codabar:
    case Symbology::Code93:
    case Symbology::Gs1_128:
    case Symbology::Pdf417:
    case Symbology::DataMatrix:
      return false;
  }
  return false;
}

int eanCheckDigit(std::string_view digits) noexcept {
  if (!allDigits(digits)) {
    return -1;
  }
  int sum = 0;
  const size_t length = digits.size();
  for (size_t i = 0; i < length; ++i) {
    // Counting leftwards from the check position: the digit next to it weighs 3.
    const bool heavy = ((length - 1 - i) % 2) == 0;
    sum += (digits[i] - '0') * (heavy ? 3 : 1);
  }
  return (10 - (sum % 10)) % 10;
}

// EAN-13: 95 modules of symbol plus the specified 11-module left and 7-module right
// quiet zones. EAN-8: 67 plus 7 and 7.
BarcodeEncoding encodeEan13(std::string_view digits) {
  return encodeEan(digits, 12, kEan13, "ean13", 95 + 11 + 7);
}

BarcodeEncoding encodeEan8(std::string_view digits) {
  return encodeEan(digits, 7, kEan8, "ean8", 67 + 7 + 7);
}

BarcodeEncoding encodeCode128(std::string_view data) {
  if (data.empty()) {
    return failure(BarcodeError::InvalidData, "code128 needs at least one character");
  }
  for (const char c : data) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte > 0x7E) {
      return failure(BarcodeError::InvalidData,
                     "code128 subset B covers 0x20..0x7E; byte 0x" +
                         std::string(1, "0123456789ABCDEF"[(byte >> 4) & 0x0F]) +
                         std::string(1, "0123456789ABCDEF"[byte & 0x0F]) +
                         " is outside it");
    }
  }

  BarcodeEncoding out;
  out.m = kCode128;
  out.text = std::string(data);

  // How many Code 128 symbols the data costs, for the width estimate: start, one per
  // encoded value, checksum, stop. The code-set escapes are symbols too.
  int symbols = 0;
  auto pushEscape = [&](char set) {
    out.data.push_back(0x7B);  // '{'
    out.data.push_back(static_cast<uint8_t>(set));
    ++symbols;
  };
  auto pushSubsetB = [&](char c) {
    if (c == '{') {
      out.data.push_back(0x7B);  // a literal brace is doubled
    }
    out.data.push_back(static_cast<uint8_t>(c));
    ++symbols;
  };

  // Start in C only when the leading digit run pays for the switch on its own: four or
  // more digits, and an even count, so the first pair lands on a boundary. An odd run
  // starts in B, and the loop below spends one digit there to make it even.
  const size_t lead = digitRun(data, 0);
  bool in_c = (lead % 2 == 0) && (lead >= 4 || (data.size() == 2 && lead == 2));
  pushEscape(in_c ? 'C' : 'B');

  size_t pos = 0;
  while (pos < data.size()) {
    if (in_c) {
      if (digitRun(data, pos) >= 2) {
        const int value = (data[pos] - '0') * 10 + (data[pos + 1] - '0');
        out.data.push_back(static_cast<uint8_t>(value));
        ++symbols;
        pos += 2;
        continue;
      }
      pushEscape('B');
      in_c = false;
      continue;
    }

    const size_t run = digitRun(data, pos);
    const bool to_end = pos + run == data.size();
    // Six digits mid-string, or four at the tail, is where code set C stops costing
    // more in escapes than it saves in pairs.
    if (run >= 6 || (to_end && run >= 4)) {
      if (run % 2 == 1) {
        pushSubsetB(data[pos]);
        ++pos;
      } else {
        pushEscape('C');
        in_c = true;
      }
      continue;
    }
    pushSubsetB(data[pos]);
    ++pos;
  }

  if (out.data.size() > kMaxDataBytes) {
    return failure(BarcodeError::TooLong,
                   "code128 encodes to " + std::to_string(out.data.size()) +
                       " bytes; GS k carries at most " + std::to_string(kMaxDataBytes));
  }

  out.ok = true;
  // 11 modules per symbol, 13 for the stop pattern, plus the checksum symbol the
  // printer appends and 10-module quiet zones either side.
  out.modules = 11 * (symbols + 1) + 13 + 20;
  return out;
}

BarcodeEncoding encodeBarcode(Symbology symbology, std::string_view data) {
  switch (symbology) {
    case Symbology::Code128:
      return encodeCode128(data);
    case Symbology::Ean13:
      return encodeEan13(data);
    case Symbology::Ean8:
      return encodeEan8(data);
    default:
      break;
  }
  return failure(BarcodeError::UnsupportedSymbology,
                 std::string(to_string(symbology)) +
                     " is not implemented on the hardware path");
}

std::vector<uint8_t> barcodeCommands(const BarcodeEncoding& encoding, int height_dots,
                                     int module_width, Hri hri, bool* clamped) {
  std::vector<uint8_t> out;
  if (!encoding.ok || encoding.data.empty() ||
      encoding.data.size() > kMaxDataBytes) {
    if (clamped != nullptr) {
      *clamped = false;
    }
    return out;
  }
  const int height = std::clamp(height_dots, 1, 255);
  const int width = std::clamp(module_width, 1, 6);
  if (clamped != nullptr) {
    *clamped = height != height_dots || width != module_width;
  }

  out.push_back(kGs);  // GS h n — barcode height in dots
  out.push_back(0x68);
  out.push_back(static_cast<uint8_t>(height));

  out.push_back(kGs);  // GS w n — module width
  out.push_back(0x77);
  out.push_back(static_cast<uint8_t>(width));

  uint8_t hri_position = 0;
  switch (hri) {
    case Hri::None: hri_position = 0; break;
    case Hri::Above: hri_position = 1; break;
    case Hri::Below: hri_position = 2; break;
    case Hri::Both: hri_position = 3; break;
  }
  out.push_back(kGs);  // GS H n — human readable interpretation position
  out.push_back(0x48);
  out.push_back(hri_position);

  out.push_back(kGs);  // GS k m n d1..dn — function B
  out.push_back(0x6B);
  out.push_back(encoding.m);
  out.push_back(static_cast<uint8_t>(encoding.data.size()));
  out.insert(out.end(), encoding.data.begin(), encoding.data.end());
  return out;
}

}  // namespace pd::dsl
