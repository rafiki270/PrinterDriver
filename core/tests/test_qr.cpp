#include <string>

#include "printerdriver/escpos_encoder.hpp"
#include "test_harness.hpp"

using namespace pd::escpos;

PD_TEST(qr_golden_bytes_for_short_payload) {
  Encoder encoder;
  encoder.qr("P001");
  CHECK_BYTES(encoder.bytes(),
              // fn 165: model 2
              0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00,
              // fn 167: module size 4
              0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43, 0x04,
              // fn 169: error correction M
              0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, 0x31,
              // fn 180: store "P001" (pL pH = payload + 3)
              0x1D, 0x28, 0x6B, 0x07, 0x00, 0x31, 0x50, 0x30, 'P', '0', '0', '1',
              // fn 181: print
              0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30);
}

PD_TEST(qr_parameters_reach_the_wire) {
  Encoder encoder;
  encoder.qr("7F3A-92C1", 8, QrErrorCorrection::H, QrModel::Model1);
  const Bytes bytes = encoder.bytes();
  CHECK_EQ(static_cast<unsigned>(bytes[7]), 0x31u);   // model 1
  CHECK_EQ(static_cast<unsigned>(bytes[16]), 0x08u);  // module size
  CHECK_EQ(static_cast<unsigned>(bytes[24]), 0x33u);  // EC level H
  CHECK_EQ(static_cast<unsigned>(bytes[28]), 0x0Cu);  // pL = 9 + 3
  CHECK_EQ(static_cast<unsigned>(bytes[29]), 0x00u);  // pH
}

PD_TEST(qr_payload_is_not_transliterated) {
  // QR data is a byte string, so a UTF-8 payload must survive intact even though
  // the encoder's text path would map it into a single-byte code page.
  const std::string payload = "\xC4\x8D";  // U+010D in UTF-8
  Encoder encoder;
  encoder.selectCodePage(CodePage::PC852).qr(payload);
  const Bytes bytes = encoder.bytes();
  CHECK_EQ(static_cast<unsigned>(bytes[bytes.size() - 10]), 0xC4u);
  CHECK_EQ(static_cast<unsigned>(bytes[bytes.size() - 9]), 0x8Du);
}

PD_TEST(qr_validates_module_size) {
  Encoder encoder;
  CHECK_THROWS(encoder.qr("x", 0), EncodingError);
  CHECK_THROWS(encoder.qr("x", 17), EncodingError);
}
