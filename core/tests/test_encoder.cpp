#include <string>

#include "printerdriver/escpos_encoder.hpp"
#include "test_harness.hpp"

using namespace pd::escpos;

PD_TEST(process_id_marker_matches_the_probe_bytes) {
  // docs/techspec.md §3.1 and the probe in docs/testing-plan.md: token P001.
  CHECK_BYTES(processIdMarker("P001"), 0x1D, 0x28, 0x48, 0x06, 0x00, 0x30, 0x30, 0x50,
              0x30, 0x30, 0x31);
  // docs/techspec.md §3.1 worked example, token P123.
  CHECK_BYTES(processIdMarker("P123"), 0x1D, 0x28, 0x48, 0x06, 0x00, 0x30, 0x30, 0x50,
              0x31, 0x32, 0x33);
}

PD_TEST(process_id_marker_rejects_invalid_tokens) {
  CHECK(isValidProcessIdToken("P001"));
  CHECK(isValidProcessIdToken("    "));   // 0x20 is the lower bound, and valid
  CHECK(isValidProcessIdToken("~~~~"));   // 0x7E is the upper bound, and valid

  CHECK(!isValidProcessIdToken(""));
  CHECK(!isValidProcessIdToken("P00"));
  CHECK(!isValidProcessIdToken("P0011"));
  CHECK(!isValidProcessIdToken(std::string("P0\x00""1", 4)));
  CHECK(!isValidProcessIdToken(std::string("P0\x1F""1", 4)));
  CHECK(!isValidProcessIdToken(std::string("P0\x7F""1", 4)));
  CHECK(!isValidProcessIdToken(std::string("P0\xC4""1", 4)));

  CHECK_THROWS(processIdMarker("P00"), EncodingError);
  CHECK_THROWS(processIdMarker("P0011"), EncodingError);
  CHECK_THROWS(processIdMarker(std::string("P0\x00""1", 4)), EncodingError);
  CHECK_THROWS(processIdMarker(std::string("ab\x80""d", 4)), EncodingError);
}

PD_TEST(realtime_and_queued_status_commands) {
  CHECK_BYTES(dleEot(DleEotKind::PrinterStatus), 0x10, 0x04, 0x01);
  CHECK_BYTES(dleEot(DleEotKind::OfflineCause), 0x10, 0x04, 0x02);
  CHECK_BYTES(dleEot(DleEotKind::ErrorCause), 0x10, 0x04, 0x03);
  CHECK_BYTES(dleEot(DleEotKind::PaperSensor), 0x10, 0x04, 0x04);
  CHECK_BYTES(gsPaperStatus(), 0x1D, 0x72, 0x01);
}

PD_TEST(asb_enable_and_disable) {
  CHECK_BYTES(asbEnable(), 0x1D, 0x61, 0x0E);
  CHECK_BYTES(asbEnable(0x0E), 0x1D, 0x61, 0x0E);
  CHECK_BYTES(asbDisable(), 0x1D, 0x61, 0x00);

  Encoder encoder;
  encoder.asb(true);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x61, 0x0E);
  encoder.clear();
  encoder.asb(false);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x61, 0x00);
}

PD_TEST(small_receipt_golden_buffer) {
  Encoder encoder;
  encoder.initialize()
      .selectCodePage(CodePage::PC852)
      .align(Alignment::Center)
      .bold(true)
      .line("HELLO")
      .bold(false)
      .feedLines(3)
      .cut(CutMode::Partial);

  CHECK_BYTES(encoder.bytes(),
              0x1B, 0x40,                          // ESC @
              0x1B, 0x74, 0x12,                    // ESC t 18 (PC852)
              0x1B, 0x61, 0x01,                    // ESC a 1 (centre)
              0x1B, 0x45, 0x01,                    // ESC E 1 (bold on)
              'H', 'E', 'L', 'L', 'O', 0x0A,       // text + LF
              0x1B, 0x45, 0x00,                    // ESC E 0 (bold off)
              0x1B, 0x64, 0x03,                    // ESC d 3
              0x1D, 0x56, 0x01);                   // GS V 1 (partial cut)
}

PD_TEST(fenced_receipt_sequence_from_techspec) {
  // docs/techspec.md §5.2: content, final feed, marker, then cut behind its own
  // marker. The encoder only lays down bytes; waiting between them is M2's job.
  Encoder encoder;
  encoder.initialize().line("TICKET").feedLines(2).processId("P7K2");
  const size_t before_cut = encoder.size();
  encoder.cut(CutMode::Partial).processId("C7K2").realtimeStatus(DleEotKind::ErrorCause);

  const Bytes bytes = encoder.bytes();
  const Bytes tail(bytes.begin() + static_cast<long>(before_cut), bytes.end());
  CHECK_BYTES(tail,
              0x1D, 0x56, 0x01,
              0x1D, 0x28, 0x48, 0x06, 0x00, 0x30, 0x30, 'C', '7', 'K', '2',
              0x10, 0x04, 0x03);
}

PD_TEST(style_commands_are_emitted_only_on_change) {
  Encoder encoder;
  encoder.bold(true).bold(true).bold(true);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x45, 0x01);

  encoder.clear();
  encoder.bold(false);  // already the ESC @ default
  CHECK_EQ(encoder.size(), static_cast<size_t>(0));

  encoder.clear();
  encoder.align(Alignment::Right).align(Alignment::Right).align(Alignment::Left);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x61, 0x02, 0x1B, 0x61, 0x00);

  // ESC @ resets the printer, so the tracked state resets with it and the next
  // setter call must emit again.
  encoder.clear();
  encoder.bold(true).initialize().bold(true);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x45, 0x01, 0x1B, 0x40, 0x1B, 0x45, 0x01);
}

PD_TEST(underline_and_size_and_alignment) {
  Encoder encoder;
  encoder.underline(1);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x2D, 0x01);
  encoder.clear();
  encoder.underline(2);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x2D, 0x02);
  CHECK_THROWS(encoder.underline(3), EncodingError);

  encoder.clear();
  encoder.textSize(2, 2);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x21, 0x11);
  encoder.clear();
  encoder.textSize(3, 1);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x21, 0x20);
  encoder.clear();
  encoder.textSize(8, 8);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x21, 0x77);
  CHECK_THROWS(encoder.textSize(0, 1), EncodingError);
  CHECK_THROWS(encoder.textSize(1, 9), EncodingError);

  encoder.clear();
  encoder.align(Alignment::Center);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x61, 0x01);
}

PD_TEST(cut_variants) {
  Encoder encoder;
  encoder.cut(CutMode::Partial);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x56, 0x01);

  encoder.clear();
  encoder.cut(CutMode::Full);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x56, 0x00);

  encoder.clear();
  encoder.cutWithFeed(CutMode::Partial, 0x50);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x56, 0x42, 0x50);

  encoder.clear();
  encoder.cutWithFeed(CutMode::Full, 0x30);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x56, 0x41, 0x30);

  // With the flag set, cut() routes to the feed variant.
  encoder.clear();
  encoder.useCutWithFeed(true, 0x64).cut(CutMode::Partial);
  CHECK_BYTES(encoder.bytes(), 0x1D, 0x56, 0x42, 0x64);
}

PD_TEST(cash_drawer_and_feeds) {
  Encoder encoder;
  encoder.kickCashDrawer(0, 25, 25);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x70, 0x00, 0x19, 0x19);

  encoder.clear();
  encoder.kickCashDrawer(1, 50, 50);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x70, 0x01, 0x32, 0x32);
  CHECK_THROWS(encoder.kickCashDrawer(2), EncodingError);

  encoder.clear();
  encoder.feed().feedLines(4);
  CHECK_BYTES(encoder.bytes(), 0x0A, 0x1B, 0x64, 0x04);
}

PD_TEST(feed_dots_emits_esc_j_and_chunks_above_255) {
  Encoder encoder;
  encoder.feedDots(120);
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x4A, 0x78);  // 120 dots, one command

  encoder.clear();
  encoder.feedDots(0);
  CHECK_EQ(encoder.size(), static_cast<size_t>(0));  // nothing to feed, nothing sent

  encoder.clear();
  encoder.feedDots(300);  // above the single-byte operand: 255 + 45
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x4A, 0xFF, 0x1B, 0x4A, 0x2D);

  encoder.clear();
  encoder.feedDots(510);  // an exact multiple of the 255-dot chunk
  CHECK_BYTES(encoder.bytes(), 0x1B, 0x4A, 0xFF, 0x1B, 0x4A, 0xFF);
}

PD_TEST(raw_passthrough_and_take) {
  Encoder encoder;
  const Bytes payload{0xDE, 0xAD};
  encoder.raw(payload).raw(payload.data(), payload.size());
  CHECK_BYTES(encoder.bytes(), 0xDE, 0xAD, 0xDE, 0xAD);

  const Bytes taken = encoder.take();
  CHECK_BYTES(taken, 0xDE, 0xAD, 0xDE, 0xAD);
  CHECK_EQ(encoder.size(), static_cast<size_t>(0));
}

PD_TEST(printer_identification_commands) {
  CHECK_BYTES(gsIdentity(PrinterInfoKind::ModelId), 0x1D, 0x49, 0x01);
  CHECK_BYTES(gsIdentity(PrinterInfoKind::TypeId), 0x1D, 0x49, 0x02);
  CHECK_BYTES(gsIdentity(PrinterInfoKind::RomVersionId), 0x1D, 0x49, 0x03);
  CHECK_BYTES(gsIdentity(PrinterInfoKind::FirmwareVersion), 0x1D, 0x49, 0x41);
  CHECK_BYTES(gsIdentity(PrinterInfoKind::MakerName), 0x1D, 0x49, 0x42);
  CHECK_BYTES(gsIdentity(PrinterInfoKind::ModelName), 0x1D, 0x49, 0x43);
  CHECK_BYTES(gsIdentity(PrinterInfoKind::SerialNumber), 0x1D, 0x49, 0x44);
  CHECK_BYTES(gsIdentity(uint8_t{69}), 0x1D, 0x49, 0x45);
}

PD_TEST(operator_only_commands_are_built_but_never_by_the_engine) {
  // DLE ENQ 1 resumes from the line the error happened on, DLE ENQ 2 resumes while
  // discarding the buffers. Both are duplicate-or-lost-ticket risks, so they exist
  // as builders for a deliberate operator action and nowhere else.
  CHECK_BYTES(dleEnq(kDleEnqResume), 0x10, 0x05, 0x01);
  CHECK_BYTES(dleEnq(kDleEnqClearBuffers), 0x10, 0x05, 0x02);

  CHECK_BYTES(gsMaintenanceCounter(10), 0x1D, 0x67, 0x32, 0x00, 0x0A, 0x00);
  CHECK_BYTES(gsMaintenanceCounter(300), 0x1D, 0x67, 0x32, 0x00, 0x2C, 0x01);

  CHECK_BYTES(gsTestPrint(), 0x1D, 0x28, 0x41, 0x02, 0x00, 0x00, 0x02);
  CHECK_BYTES(gsTestPrint(1, 3), 0x1D, 0x28, 0x41, 0x02, 0x00, 0x01, 0x03);

  CHECK_BYTES(gsReadMemorySwitch(1), 0x1D, 0x28, 0x45, 0x02, 0x00, 0x04, 0x01);
  CHECK_BYTES(gsReadCustomizedSetting(5), 0x1D, 0x28, 0x45, 0x02, 0x00, 0x06, 0x05);
}
