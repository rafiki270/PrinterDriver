#include <string>
#include <vector>

#include "printerdriver/dsl/barcode.hpp"
#include "test_harness.hpp"

// The symbology encoders (docs/receipt-dsl.md, `barcode` block), checked against
// published article numbers rather than against themselves: a check-digit routine that
// agrees with its own arithmetic proves nothing, and a wrong check digit prints a
// perfectly scannable barcode for the wrong product.

using namespace pd::dsl;

namespace {

std::string hexOf(const std::vector<uint8_t>& bytes) { return ::pdtest::hex(bytes); }

}  // namespace

// --- Check digits -------------------------------------------------------------------

PD_TEST(ean13_check_digits_match_published_article_numbers) {
  // 4006381333931 — the EAN-13 in Wikipedia's worked example.
  CHECK_EQ(eanCheckDigit("400638133393"), 1);
  // 9780306406157 — Bookland EAN for ISBN 0-306-40615-2.
  CHECK_EQ(eanCheckDigit("978030640615"), 7);
  // 5901234123457 — the GS1 sample number.
  CHECK_EQ(eanCheckDigit("590123412345"), 7);
}

PD_TEST(ean8_check_digits_match_published_article_numbers) {
  CHECK_EQ(eanCheckDigit("9638507"), 4);   // 96385074
  CHECK_EQ(eanCheckDigit("5512345"), 7);   // 55123457
  CHECK_EQ(eanCheckDigit("7351353"), 7);   // 73513537
}

PD_TEST(a_non_numeric_payload_has_no_check_digit) {
  CHECK_EQ(eanCheckDigit("40063813339x"), -1);
  CHECK_EQ(eanCheckDigit(""), -1);
}

// --- EAN-13 / EAN-8 -----------------------------------------------------------------

PD_TEST(ean13_appends_the_check_digit_and_verifies_a_supplied_one) {
  const BarcodeEncoding computed = encodeEan13("400638133393");
  CHECK(computed.ok);
  CHECK_EQ(computed.text, std::string("4006381333931"));
  CHECK_EQ(computed.m, static_cast<uint8_t>(67));
  CHECK_EQ(computed.data.size(), static_cast<size_t>(13));

  const BarcodeEncoding supplied = encodeEan13("4006381333931");
  CHECK(supplied.ok);
  CHECK_EQ(supplied.text, std::string("4006381333931"));
  CHECK(supplied.data == computed.data);

  // A wrong check digit is refused rather than corrected: correcting it would print a
  // scannable symbol for an article the caller did not name.
  const BarcodeEncoding wrong = encodeEan13("4006381333932");
  CHECK(!wrong.ok);
  CHECK(wrong.error == BarcodeError::InvalidData);
  CHECK(wrong.message.find("expected 1") != std::string::npos);

  const BarcodeEncoding short_data = encodeEan13("40063813339");
  CHECK(!short_data.ok);
  const BarcodeEncoding letters = encodeEan13("40063813339a");
  CHECK(!letters.ok);
}

PD_TEST(ean8_appends_the_check_digit_and_verifies_a_supplied_one) {
  const BarcodeEncoding computed = encodeEan8("9638507");
  CHECK(computed.ok);
  CHECK_EQ(computed.text, std::string("96385074"));
  CHECK_EQ(computed.m, static_cast<uint8_t>(68));
  CHECK_EQ(computed.data.size(), static_cast<size_t>(8));

  const BarcodeEncoding supplied = encodeEan8("96385074");
  CHECK(supplied.ok);
  CHECK(supplied.data == computed.data);

  const BarcodeEncoding wrong = encodeEan8("96385070");
  CHECK(!wrong.ok);
  CHECK(wrong.error == BarcodeError::InvalidData);
}

PD_TEST(ean13_produces_the_full_gs_k_command) {
  const BarcodeEncoding encoding = encodeEan13("4006381333931");
  const std::vector<uint8_t> bytes =
      barcodeCommands(encoding, 64, 2, Hri::Below, nullptr);
  CHECK_BYTES(bytes,
              0x1D, 0x68, 0x40,              // GS h 64  — height in dots
              0x1D, 0x77, 0x02,              // GS w 2   — module width
              0x1D, 0x48, 0x02,              // GS H 2   — HRI below
              0x1D, 0x6B, 0x43, 0x0D,        // GS k 67 13 — EAN-13, function B
              '4', '0', '0', '6', '3', '8', '1', '3', '3', '3', '9', '3', '1');
}

PD_TEST(hri_position_reaches_gs_capital_h) {
  const BarcodeEncoding encoding = encodeEan8("96385074");
  CHECK_EQ(barcodeCommands(encoding, 64, 2, Hri::None)[8], static_cast<uint8_t>(0));
  CHECK_EQ(barcodeCommands(encoding, 64, 2, Hri::Above)[8], static_cast<uint8_t>(1));
  CHECK_EQ(barcodeCommands(encoding, 64, 2, Hri::Below)[8], static_cast<uint8_t>(2));
  CHECK_EQ(barcodeCommands(encoding, 64, 2, Hri::Both)[8], static_cast<uint8_t>(3));
}

PD_TEST(operand_ranges_are_clamped_and_reported) {
  const BarcodeEncoding encoding = encodeEan8("96385074");
  bool clamped = false;
  const std::vector<uint8_t> bytes = barcodeCommands(encoding, 4000, 12, Hri::None,
                                                     &clamped);
  CHECK(clamped);
  CHECK_EQ(bytes[2], static_cast<uint8_t>(255));  // GS h operand is one byte
  CHECK_EQ(bytes[5], static_cast<uint8_t>(6));    // GS w tops out at 6

  bool untouched = true;
  barcodeCommands(encoding, 64, 3, Hri::None, &untouched);
  CHECK(!untouched);
}

// --- Code 128 ------------------------------------------------------------------------

PD_TEST(code128_text_stays_in_subset_b) {
  const BarcodeEncoding encoding = encodeCode128("ORDER-7F3A");
  CHECK(encoding.ok);
  CHECK_EQ(encoding.m, static_cast<uint8_t>(73));
  CHECK_EQ(hexOf(encoding.data),
           std::string("7B 42 4F 52 44 45 52 2D 37 46 33 41"));  // {B O R D E R - 7 F 3 A
}

PD_TEST(code128_starts_in_subset_c_for_an_all_digit_payload) {
  // Twelve digits: {C plus six pair bytes, against {B plus twelve.
  const BarcodeEncoding encoding = encodeCode128("400638133393");
  CHECK(encoding.ok);
  CHECK_EQ(encoding.data.size(), static_cast<size_t>(8));
  CHECK_EQ(hexOf(encoding.data), std::string("7B 43 28 06 26 0D 21 5D"));
  //                                          {  C  40 06 38 13 33 93
}

PD_TEST(code128_switches_into_subset_c_for_a_long_digit_run_and_back_out) {
  const BarcodeEncoding encoding = encodeCode128("AB123456CD");
  CHECK(encoding.ok);
  // {B A B {C 12 34 56 {B C D
  CHECK_EQ(hexOf(encoding.data),
           std::string("7B 42 41 42 7B 43 0C 22 38 7B 42 43 44"));
}

PD_TEST(code128_leaves_a_short_digit_run_in_subset_b) {
  // Four digits mid-string cost two escapes to save two bytes: not worth the switch.
  const BarcodeEncoding encoding = encodeCode128("A1234B");
  CHECK(encoding.ok);
  CHECK_EQ(hexOf(encoding.data), std::string("7B 42 41 31 32 33 34 42"));
}

PD_TEST(code128_switches_for_four_digits_at_the_tail) {
  // At the end there is no switch back to pay for, so four digits already pay.
  const BarcodeEncoding encoding = encodeCode128("A1234");
  CHECK(encoding.ok);
  CHECK_EQ(hexOf(encoding.data), std::string("7B 42 41 7B 43 0C 22"));  // {B A {C 12 34
}

PD_TEST(code128_spends_one_digit_in_subset_b_to_reach_an_even_boundary) {
  // Seven digits: '1' in B, then {C for 234567 — an odd run cannot start a pair.
  const BarcodeEncoding encoding = encodeCode128("1234567");
  CHECK(encoding.ok);
  CHECK_EQ(hexOf(encoding.data), std::string("7B 42 31 7B 43 17 2D 43"));
  //                                          {  B  1  {  C  23 45 67
}

PD_TEST(code128_pairs_the_two_digit_special_case_in_subset_c) {
  const BarcodeEncoding encoding = encodeCode128("42");
  CHECK(encoding.ok);
  CHECK_EQ(hexOf(encoding.data), std::string("7B 43 2A"));  // {C 42
}

PD_TEST(code128_doubles_a_literal_brace) {
  const BarcodeEncoding encoding = encodeCode128("a{b");
  CHECK(encoding.ok);
  CHECK_EQ(hexOf(encoding.data), std::string("7B 42 61 7B 7B 62"));  // {B a {{ b
}

PD_TEST(code128_refuses_bytes_outside_subset_b) {
  const BarcodeEncoding control = encodeCode128(std::string("a\tb"));
  CHECK(!control.ok);
  CHECK(control.error == BarcodeError::InvalidData);
  CHECK(control.message.find("0x09") != std::string::npos);

  const BarcodeEncoding high = encodeCode128("caf\xC3\xA9");  // UTF-8 é
  CHECK(!high.ok);

  const BarcodeEncoding empty = encodeCode128("");
  CHECK(!empty.ok);
}

PD_TEST(code128_refuses_more_data_than_gs_k_can_carry) {
  const BarcodeEncoding long_text = encodeCode128(std::string(300, 'A'));
  CHECK(!long_text.ok);
  CHECK(long_text.error == BarcodeError::TooLong);

  // The same 300 characters as digits fit, because code set C halves them.
  const BarcodeEncoding digits = encodeCode128(std::string(300, '7'));
  CHECK(digits.ok);
  CHECK_EQ(digits.data.size(), static_cast<size_t>(152));  // {C plus 150 pairs
}

// --- Dispatch and support -------------------------------------------------------------

PD_TEST(only_the_implemented_symbologies_claim_support) {
  CHECK(isBarcodeSupported(Symbology::Code128));
  CHECK(isBarcodeSupported(Symbology::Ean13));
  CHECK(isBarcodeSupported(Symbology::Ean8));
  CHECK(!isBarcodeSupported(Symbology::UpcA));
  CHECK(!isBarcodeSupported(Symbology::Code39));
  CHECK(!isBarcodeSupported(Symbology::Pdf417));
  CHECK(!isBarcodeSupported(Symbology::DataMatrix));
  CHECK(!isBarcodeSupported(Symbology::Gs1_128));

  const BarcodeEncoding unsupported = encodeBarcode(Symbology::Pdf417, "x");
  CHECK(!unsupported.ok);
  CHECK(unsupported.error == BarcodeError::UnsupportedSymbology);
  CHECK(barcodeCommands(unsupported, 64, 2, Hri::None).empty());

  CHECK(encodeBarcode(Symbology::Ean13, "4006381333931").ok);
  CHECK(encodeBarcode(Symbology::Ean8, "96385074").ok);
  CHECK(encodeBarcode(Symbology::Code128, "abc").ok);
}
