#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "printerdriver/dsl/document.hpp"

// Barcode symbologies for the hardware path (docs/receipt-dsl.md, `barcode` block).
//
// Everything here is a pure function of the block: data in, `GS k` bytes out. No
// printer, no socket, no state — so every symbology is testable as a golden buffer, and
// the check digits are testable against published article numbers.
//
// -- Which ESC/POS form, and why -------------------------------------------------------
//
// `GS k m n d1..dn` (function B, explicit length) rather than `GS k m d1..dk NUL`
// (function A, NUL-terminated). Code 128 in code set C encodes a digit *pair* as one
// byte with the numeric value 0..99, and 0x00 is a legal value in that range: a
// NUL-terminated form would truncate "00" mid-barcode. Function B carries a length, so
// it cannot.
//
// -- Code 128 subset selection ----------------------------------------------------------
//
// ESC/POS carries the code set in the data itself: `{A`, `{B`, `{C` select it and `{{`
// is a literal brace. Code set C halves the byte count of digit runs, so the encoder
// switches into it where that pays and back out where it does not — the classic rule,
// spelled out in encodeCode128 so it can be read and tested rather than guessed at.
//
// Code set A (control characters) is deliberately not used: a receipt DSL block carries
// text, and a control byte in a barcode payload is a bug in the caller's data, reported
// as such instead of silently encoded.

namespace pd::dsl {

enum class BarcodeError {
  None,
  UnsupportedSymbology,  // this milestone implements code128, ean13 and ean8
  InvalidData,           // wrong length, a non-digit in a numeric symbology, bad check
  TooLong,               // more than 255 data bytes: the `n` operand is one byte
};

const char* to_string(BarcodeError value) noexcept;

struct BarcodeEncoding {
  bool ok = false;
  BarcodeError error = BarcodeError::None;
  // Why it failed, in the words the render report will carry.
  std::string message;

  // `m` operand of GS k (function B): 67 EAN-13, 68 EAN-8, 73 Code 128.
  uint8_t m = 0;
  // The bytes between `n` and the end of the command.
  std::vector<uint8_t> data;
  // What the symbol actually says, check digit included — the string an operator would
  // read off the paper, and what the preview prints.
  std::string text;
  // Symbol width in modules including quiet zones, for the does-it-fit check. An
  // estimate for Code 128 in the sense that the symbol count is exact and the quiet
  // zones are the specified minimum.
  int modules = 0;
};

// True for the symbologies this build can emit; everything else keeps the declared
// degradation of docs/receipt-dsl.md rather than being half-drawn.
bool isBarcodeSupported(Symbology symbology) noexcept;

// EAN check digits. Weight 3 on every second digit counting leftwards from the check
// position, which is why EAN-13 starts at weight 1 and EAN-8 at weight 3.
// Returns -1 when `digits` is not all digits.
int eanCheckDigit(std::string_view digits) noexcept;

// 12 or 13 digits in; a 13-digit symbol out. A supplied 13th digit is verified rather
// than trusted, because a wrong one prints a scannable barcode for the wrong article.
BarcodeEncoding encodeEan13(std::string_view digits);
// 7 or 8 digits in, an 8-digit symbol out.
BarcodeEncoding encodeEan8(std::string_view digits);
// Printable ASCII (0x20..0x7E) in; `{B`/`{C`-framed ESC/POS Code 128 data out.
BarcodeEncoding encodeCode128(std::string_view data);

BarcodeEncoding encodeBarcode(Symbology symbology, std::string_view data);

// GS h (height) · GS w (module width) · GS H (HRI position) · GS k (the symbol), in
// that order, as one buffer. Height is clamped to 1..255 and module width to 1..6, the
// ESC/POS operand ranges; `clamped` reports whether either was out of range so the
// caller can declare it.
std::vector<uint8_t> barcodeCommands(const BarcodeEncoding& encoding, int height_dots,
                                     int module_width, Hri hri, bool* clamped = nullptr);

}  // namespace pd::dsl
