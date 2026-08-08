#include <vector>

#include "printerdriver/escpos_encoder.hpp"
#include "test_harness.hpp"

using namespace pd::escpos;

namespace {

// 8x8 checkerboard, already packed: 1 = dot printed, MSB is the leftmost dot.
const std::vector<uint8_t> kCheckerboard{0xAA, 0x55, 0xAA, 0x55,
                                         0xAA, 0x55, 0xAA, 0x55};

std::vector<uint8_t> halfBlackHalfWhite(uint32_t width, uint32_t height) {
  std::vector<uint8_t> gray(static_cast<size_t>(width) * height, 0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      gray[static_cast<size_t>(y) * width + x] = (x < width / 2) ? 0 : 255;
    }
  }
  return gray;
}

}  // namespace

PD_TEST(packed_checkerboard_golden_bytes) {
  Encoder encoder;
  encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 8, 8);
  CHECK_BYTES(encoder.bytes(),
              0x1D, 0x76, 0x30, 0x00,  // GS v 0, m = 0 (normal)
              0x01, 0x00,              // xL xH = 1 byte per row
              0x08, 0x00,              // yL yH = 8 rows
              0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55);
}

PD_TEST(packed_raster_scale_operand) {
  Encoder encoder;
  encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 8, 8,
                       RasterScale::Quadruple);
  const Bytes bytes = encoder.bytes();
  CHECK_EQ(bytes.size(), static_cast<size_t>(16));
  CHECK_EQ(static_cast<unsigned>(bytes[3]), 0x03u);
}

PD_TEST(packed_raster_validates_dimensions) {
  Encoder encoder;
  CHECK_THROWS(encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 8, 7),
               EncodingError);
  CHECK_THROWS(encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 0, 8),
               EncodingError);
  CHECK_THROWS(encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 8, 0),
               EncodingError);
  CHECK_EQ(encoder.size(), static_cast<size_t>(0));

  // Widths that are not a multiple of 8 pad to whole bytes.
  const std::vector<uint8_t> padded(2 * 4, 0xFF);
  encoder.rasterPacked(padded.data(), padded.size(), 12, 4);
  const Bytes bytes = encoder.bytes();
  CHECK_EQ(static_cast<unsigned>(bytes[4]), 0x02u);
  CHECK_EQ(static_cast<unsigned>(bytes[6]), 0x04u);
}

PD_TEST(raster_banding_splits_into_several_commands) {
  Encoder encoder;
  encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 8, 8,
                       RasterScale::Normal, 4);
  CHECK_BYTES(encoder.bytes(),
              0x1D, 0x76, 0x30, 0x00, 0x01, 0x00, 0x04, 0x00,
              0xAA, 0x55, 0xAA, 0x55,
              0x1D, 0x76, 0x30, 0x00, 0x01, 0x00, 0x04, 0x00,
              0xAA, 0x55, 0xAA, 0x55);

  // A final short band is emitted with its real row count, not padded.
  encoder.clear();
  encoder.rasterPacked(kCheckerboard.data(), kCheckerboard.size(), 8, 8,
                       RasterScale::Normal, 3);
  const Bytes bytes = encoder.bytes();
  CHECK_EQ(bytes.size(), static_cast<size_t>(3 * 8 + 8));
  CHECK_EQ(static_cast<unsigned>(bytes[6]), 0x03u);
  CHECK_EQ(static_cast<unsigned>(bytes[11 + 6]), 0x03u);
  CHECK_EQ(static_cast<unsigned>(bytes[22 + 6]), 0x02u);
}

PD_TEST(grayscale_fixed_threshold_golden_bytes) {
  const std::vector<uint8_t> gray = halfBlackHalfWhite(8, 8);
  Encoder encoder;
  encoder.rasterGrayscale(gray.data(), 8, 8, 8, Binarization::FixedThreshold);
  CHECK_BYTES(encoder.bytes(),
              0x1D, 0x76, 0x30, 0x00, 0x01, 0x00, 0x08, 0x00,
              0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0);
}

PD_TEST(grayscale_width_fit_scaling_is_deterministic) {
  const std::vector<uint8_t> gray = halfBlackHalfWhite(16, 16);
  const PackedRaster packed =
      packGrayscale(gray.data(), 16, 16, 8, Binarization::FixedThreshold);
  CHECK_EQ(packed.width_dots, 8u);
  CHECK_EQ(packed.height_dots, 8u);
  CHECK_EQ(packed.bytes_per_row, 1u);
  CHECK_BYTES(packed.data, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0);

  // Aspect ratio is preserved when fitting to a real printer width.
  const std::vector<uint8_t> wide = halfBlackHalfWhite(100, 50);
  const PackedRaster fitted =
      packGrayscale(wide.data(), 100, 50, kWidth80mm, Binarization::FixedThreshold);
  CHECK_EQ(fitted.width_dots, 576u);
  CHECK_EQ(fitted.height_dots, 288u);
  CHECK_EQ(fitted.bytes_per_row, 72u);

  // Same input, same output, every time.
  const PackedRaster again =
      packGrayscale(wide.data(), 100, 50, kWidth80mm, Binarization::FixedThreshold);
  CHECK(fitted.data == again.data);
}

PD_TEST(floyd_steinberg_golden_row) {
  // Four mid-grey pixels, threshold 128. Hand-computed from the integer error
  // diffusion in packGrayscale (7/16 to the right, remainder to the row below,
  // which does not exist here): 128 -> white (err -127), 73 -> black (err +73),
  // 159 -> white (err -96), 86 -> black. Dots land at x = 1 and x = 3 -> 0x50.
  const std::vector<uint8_t> gray(4, 128);
  const PackedRaster packed =
      packGrayscale(gray.data(), 4, 1, 4, Binarization::FloydSteinberg);
  CHECK_EQ(packed.width_dots, 4u);
  CHECK_EQ(packed.height_dots, 1u);
  CHECK_BYTES(packed.data, 0x50);

  Encoder encoder;
  encoder.rasterGrayscale(gray.data(), 4, 1, 4, Binarization::FloydSteinberg);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x76, 0x30, 0x00, 0x01, 0x00, 0x01, 0x00, 0x50);
}

PD_TEST(floyd_steinberg_is_deterministic_and_differs_from_thresholding) {
  const std::vector<uint8_t> gray(32 * 32, 128);
  const PackedRaster first =
      packGrayscale(gray.data(), 32, 32, 32, Binarization::FloydSteinberg);
  const PackedRaster second =
      packGrayscale(gray.data(), 32, 32, 32, Binarization::FloydSteinberg);
  CHECK(first.data == second.data);

  const PackedRaster thresholded =
      packGrayscale(gray.data(), 32, 32, 32, Binarization::FixedThreshold);
  // Flat mid-grey thresholds to nothing at all; dithering must produce structure.
  CHECK(thresholded.data != first.data);
  bool any_dot = false;
  for (const uint8_t byte : thresholded.data) {
    any_dot = any_dot || byte != 0;
  }
  CHECK(!any_dot);
  for (const uint8_t byte : first.data) {
    any_dot = any_dot || byte != 0;
  }
  CHECK(any_dot);
}

PD_TEST(grayscale_validates_dimensions) {
  const std::vector<uint8_t> gray(4, 128);
  CHECK_THROWS(packGrayscale(gray.data(), 0, 1, 4, Binarization::FixedThreshold),
               EncodingError);
  CHECK_THROWS(packGrayscale(gray.data(), 4, 0, 4, Binarization::FixedThreshold),
               EncodingError);
  CHECK_THROWS(packGrayscale(gray.data(), 4, 1, 0, Binarization::FixedThreshold),
               EncodingError);
}
