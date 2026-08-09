#include <chrono>
#include <string>
#include <vector>

#include "fake_printer.hpp"
#include "fake_star_printer.hpp"
#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/star.hpp"
#include "test_harness.hpp"

// M13b. Star without the SDK (docs/wire-protocols.md §2).
//
// Three things are being proved here, in rising order of how much they matter:
//   1. the byte-level primitives are the documented ones (golden buffers, decode tables);
//   2. a Star profile that used to be refused with Unsupported now prints and earns a
//      grade A claim from a real fence;
//   3. **the SDK cannot be fooled by somebody else's receipt.** The ETB counter arrives
//      in an ASB frame that TCP 9100 broadcasts to every connected host, so the guard
//      that refuses to read another host's completion as our own is the single most
//      important test in this file.

using namespace pd;

namespace {

CapabilityProfile fastStarProfile(CompletionMechanism mechanism) {
  CapabilityProfile profile = devices::star_tsp100iv();
  profile.completion = mechanism;
  profile.completion_timeout_ms = 300;
  profile.preflight_timeout_ms = 200;
  profile.final_feed_lines = 2;
  profile.chunk_bytes = 0;
  profile.inter_chunk_delay_ms = 0;
  if (mechanism == CompletionMechanism::StarEtb) {
    profile.star.exclusive_single_session = true;
  }
  return profile;
}

struct Rig {
  pdfake::StarMockLink link;
  std::unique_ptr<PrinterDriver> driver;
  std::shared_ptr<Printer> printer;

  explicit Rig(const CapabilityProfile& profile) {
    StorageConfig storage;  // in-memory
    driver.reset(new PrinterDriver(storage));
    PrinterConfig config;
    config.id = "star-under-test";
    config.transport = link.factory();
    config.profile = profile;
    config.width_dots = escpos::kWidth80mm;
    printer = driver->addPrinter(config);
  }
};

Payload textPayload(const std::string& text) {
  escpos::Encoder encoder;
  encoder.line(text);
  return Payload::document(encoder.take(), escpos::CodePage::PC437);
}

}  // namespace

// --- Primitives -----------------------------------------------------------------------

PD_TEST(star_fence_primitives_are_the_documented_bytes) {
  CHECK_BYTES(star::etbFence(), 0x17);
  CHECK_BYTES(star::asbEnable(), 0x1B, 0x1E, 0x61, 0x01);
  CHECK_BYTES(star::asbDisable(), 0x1B, 0x1E, 0x61, 0x00);
  CHECK_BYTES(star::asbRequestNow(), 0x1B, 0x06, 0x01);
  CHECK_BYTES(star::clearEtbCounter(), 0x1B, 0x1E, 0x45, 0x00);
  CHECK_BYTES(star::escGsEtxFence(0x12, 0x34), 0x1B, 0x1D, 0x03, 0x01, 0x12, 0x34);
}

PD_TEST(star_etb_counter_unpacks_the_documented_non_contiguous_bits) {
  // ASB bit6 -> counter bit4, bit5 -> 3, bit3 -> 2, bit2 -> 1, bit1 -> 0. Each bit is
  // checked on its own, because a shift-and-mask that happens to work for small values is
  // exactly the bug this packing invites.
  CHECK_EQ(static_cast<int>(star::etbCounter(0x00)), 0);
  CHECK_EQ(static_cast<int>(star::etbCounter(0x40)), 16);  // bit6 -> 4
  CHECK_EQ(static_cast<int>(star::etbCounter(0x20)), 8);   // bit5 -> 3
  CHECK_EQ(static_cast<int>(star::etbCounter(0x08)), 4);   // bit3 -> 2
  CHECK_EQ(static_cast<int>(star::etbCounter(0x04)), 2);   // bit2 -> 1
  CHECK_EQ(static_cast<int>(star::etbCounter(0x02)), 1);   // bit1 -> 0
  CHECK_EQ(static_cast<int>(star::etbCounter(0x6E)), 31);  // every counter bit set

  // Bits 7, 4 and 0 carry other status and must be ignored entirely.
  CHECK_EQ(static_cast<int>(star::etbCounter(0x91)), 0);

  // Round trip against an independently written packer, over the whole range.
  for (int i = 0; i < 32; ++i) {
    const uint8_t packed = pdfake::packEtbCounter(static_cast<uint8_t>(i));
    CHECK_EQ(static_cast<int>(star::etbCounter(packed)), i);
  }
}

PD_TEST(star_etb_counter_wraps_31_to_0) {
  CHECK_EQ(static_cast<int>(star::nextEtbCounter(0)), 1);
  CHECK_EQ(static_cast<int>(star::nextEtbCounter(30)), 31);
  // The wrap. An implementation that keeps counting in an int stops confirming here, once
  // every 32 receipts.
  CHECK_EQ(static_cast<int>(star::nextEtbCounter(31)), 0);
}

PD_TEST(star_parser_reads_the_etx_echo_and_survives_a_split_read) {
  star::ResponseParser parser;
  parser.setAsbBlockBytes(0);  // ESC GS ETX only

  const std::vector<uint8_t> frame{0x1B, 0x1D, 0x03, 0x01, 0xAB, 0xCD, 0x2A, 0x00};
  std::vector<star::Event> events = parser.feed(frame.data(), 3);
  CHECK(events.empty());  // held, not shredded
  events = parser.feed(frame.data() + 3, frame.size() - 3);
  CHECK_EQ(events.size(), static_cast<size_t>(1));
  CHECK_EQ(events[0].kind, star::EventKind::EtxAck);
  CHECK_EQ(static_cast<int>(events[0].n1), 0xAB);
  CHECK_EQ(static_cast<int>(events[0].n2), 0xCD);
  CHECK_EQ(static_cast<int>(events[0].counter), 0x2A);
  CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(0));
}

PD_TEST(star_parser_decodes_the_counter_out_of_an_asb_block) {
  star::ResponseParser parser;
  parser.setAsbBlockBytes(8);
  std::vector<uint8_t> block(8, 0x00);
  block[7] = pdfake::packEtbCounter(19);
  const std::vector<star::Event> events = parser.feed(block.data(), block.size());
  CHECK_EQ(events.size(), static_cast<size_t>(1));
  CHECK_EQ(events[0].kind, star::EventKind::AsbStatus);
  CHECK_EQ(static_cast<int>(events[0].counter), 19);
  CHECK_EQ(events[0].frame.size(), static_cast<size_t>(8));
}

PD_TEST(star_parser_does_not_wedge_on_a_stray_escape) {
  star::ResponseParser parser;
  parser.setAsbBlockBytes(0);
  // ESC followed by something that is not the ETX prefix: the byte is emitted and the
  // scan resumes, rather than the parser waiting forever for a frame that never comes.
  const std::vector<uint8_t> noise{0x1B, 0x41, 0x42};
  const std::vector<star::Event> events = parser.feed(noise.data(), noise.size());
  CHECK_EQ(events.size(), static_cast<size_t>(3));
  for (const star::Event& event : events) {
    CHECK_EQ(event.kind, star::EventKind::UnknownByte);
  }
}

// --- Line Mode encoding -----------------------------------------------------------------

PD_TEST(star_encoder_emits_line_mode_and_not_escpos) {
  star::Encoder encoder;
  encoder.initialize()
      .align(escpos::Alignment::Center)
      .bold(true)
      .line("HI")
      .bold(false)
      .feedLines(2)
      .cut(star::Cut::Partial);
  CHECK_BYTES(encoder.bytes(),
              0x1B, 0x40,              // ESC @
              0x1B, 0x1D, 0x61, 0x01,  // ESC GS a 1 — Star alignment, not ESC a
              0x1B, 0x45,              // ESC E — emphasis on
              'H', 'I', 0x0A,
              0x1B, 0x46,              // ESC F — emphasis off
              0x1B, 0x61, 0x02,        // ESC a 2 — Star feed, not ESC d
              0x1B, 0x64, 0x03);       // ESC d 3 — partial cut with feed
}

PD_TEST(star_encoder_text_is_ascii_and_lossy_rather_than_wrong) {
  star::Encoder encoder;
  encoder.text("caf\xC3\xA9");  // UTF-8 "café"
  CHECK_BYTES(encoder.bytes(), 'c', 'a', 'f', '?', '?');
}

PD_TEST(star_encoder_brackets_raster_rows_with_the_documented_mode_commands) {
  star::Encoder encoder;
  const std::vector<uint8_t> rows{0xFF, 0x00, 0x0F, 0xF0};  // 2 rows of 2 bytes
  encoder.rasterPacked(rows.data(), rows.size(), 16, 2);
  CHECK_BYTES(encoder.bytes(),
              0x1B, 0x2A, 0x72, 0x41,        // ESC * r A
              0x62, 0x02, 0x00, 0xFF, 0x00,  // b 2 0 <row>
              0x62, 0x02, 0x00, 0x0F, 0xF0,
              0x1B, 0x2A, 0x72, 0x42);       // ESC * r B
}

PD_TEST(star_transcoder_rewrites_the_escpos_subset_and_names_what_it_drops) {
  escpos::Encoder source;
  source.initialize()
      .selectCodePage(escpos::CodePage::PC437)
      .align(escpos::Alignment::Center)
      .bold(true)
      .line("A")
      .bold(false)
      .feedLines(3)
      .qr("token")
      .kickCashDrawer()
      .cut(escpos::CutMode::Partial);

  const star::TranscodeResult converted = star::transcodeFromEscPos(source.bytes());

  // ESC a 1 became ESC GS a 1, and ESC d 3 became ESC a 3: the two commands whose bytes
  // mean the opposite thing in the other language.
  const std::vector<uint8_t>& out = converted.bytes;
  const auto contains = [&out](const std::vector<uint8_t>& needle) {
    for (size_t i = 0; i + needle.size() <= out.size(); ++i) {
      if (std::equal(needle.begin(), needle.end(), out.begin() + static_cast<long>(i))) {
        return true;
      }
    }
    return false;
  };
  CHECK(contains({0x1B, 0x1D, 0x61, 0x01}));  // alignment, translated
  CHECK(contains({0x1B, 0x61, 0x03}));        // feed 3 lines, translated
  CHECK(contains({0x1B, 0x64, 0x03}));        // partial cut with feed
  CHECK(contains({0x1B, 0x45}));              // emphasis on
  CHECK(contains({'A', 0x0A}));

  // Nothing that had no equivalent went out silently.
  CHECK(!contains({0x1D, 0x28, 0x6B}));  // no GS ( k survived
  CHECK(!contains({0x1B, 0x70}));        // no ESC p survived
  bool named_qr = false;
  bool named_drawer = false;
  for (const std::string& entry : converted.dropped) {
    named_qr = named_qr || entry.find("QR") != std::string::npos;
    named_drawer = named_drawer || entry.find("drawer") != std::string::npos;
  }
  CHECK(named_qr);
  CHECK(named_drawer);
}

// --- Profiles -----------------------------------------------------------------------------

PD_TEST(star_desktop_profiles_are_now_drivable_and_portables_are_not) {
  // The change this milestone makes visible: the desktop families print, over a
  // documented, session-scoped fence.
  for (const char* name : {"star_tsp100", "star_tsp100iii", "star_tsp100iv", "star_tsp650",
                           "star_mcprint", "star_mcprint2", "star_mcprint3"}) {
    const CapabilityProfile profile = devices::byName(name);
    CHECK(profile.drivableByStarEngine());
    CHECK(profile.drivable());
    CHECK(!profile.drivableByEscposEngine());
    CHECK_EQ(profile.completion, CompletionMechanism::StarEscGsEtx);
    CHECK_EQ(profile.star.esc_gs_etx_provenance, Provenance::Documented);
    // Not ETB by default, whatever the paperwork says: its counter is broadcast.
    CHECK(!profile.star.exclusive_single_session);
  }

  // The SDK-first portables keep the checked block and keep being refused: their
  // documented path is Star's SDK over Bluetooth, and this core does not speak it.
  for (const char* name : {"star_sm_s230", "star_sm_l200", "star_sm_t400"}) {
    const CapabilityProfile profile = devices::byName(name);
    CHECK_EQ(profile.completion, CompletionMechanism::StarCheckedBlock);
    CHECK(!profile.drivable());
  }
}

PD_TEST(star_etb_is_refused_unless_the_session_is_exclusive) {
  CapabilityProfile profile = devices::star_tsp650();
  profile.completion = CompletionMechanism::StarEtb;
  // ASB is broadcast on TCP 9100, so ETB without an exclusive session cannot say whose
  // data finished. Refused before any paper moves rather than confirmed from a shared
  // counter.
  CHECK(!profile.drivableByStarEngine());
  profile.star.exclusive_single_session = true;
  CHECK(profile.drivableByStarEngine());
}

PD_TEST(star_evidence_names_the_fence_that_produced_it) {
  const JobEvidence etx = evidenceFor(CompletionMechanism::StarEscGsEtx);
  CHECK_EQ(etx.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(etx.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(std::string(etx.method), std::string("ESC GS ETX"));

  const JobEvidence etb = evidenceFor(CompletionMechanism::StarEtb);
  CHECK_EQ(etb.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(etb.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(std::string(etb.method), std::string("ETB"));

  // Neither fence carries a cutter-fault bit, so neither can reach CutFaultFree.
  CapabilityProfile profile = devices::star_tsp100iv();
  CHECK_EQ(profile.maxConfidence(), ConfidenceLevel::CutProcessed);
}

// --- The engine ------------------------------------------------------------------------

PD_TEST(star_etx_happy_path_prints_and_earns_grade_a) {
  Rig rig(fastStarProfile(CompletionMechanism::StarEscGsEtx));
  auto job = rig.printer->print(textPayload("TABLE 4"));
  const JobResult result = job->result();

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutProcessed);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.method, std::string("ESC GS ETX"));

  // Two fences: one behind the payload, one behind the cut.
  CHECK_EQ(rig.link.device->etxFences(), static_cast<size_t>(2));
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
  CHECK(rig.link.device->printText().find("TABLE 4") != std::string::npos);
  // Nothing ESC/POS reached the wire: no GS V, no GS ( H, no DLE EOT.
  CHECK(!rig.link.device->receivedContains({0x1D, 0x56}));
  CHECK(!rig.link.device->receivedContains({0x1D, 0x28, 0x48}));
  CHECK(!rig.link.device->receivedContains({0x10, 0x04}));
}

PD_TEST(star_etx_correlation_bytes_come_back_and_differ_per_fence) {
  Rig rig(fastStarProfile(CompletionMechanism::StarEscGsEtx));
  rig.printer->print(textPayload("ONE"))->result();
  rig.printer->print(textPayload("TWO"))->result();
  // Four fences over two jobs, each with its own correlation pair; if the runtime reused
  // one pair, the second job would have accepted the first job's echo.
  CHECK_EQ(rig.link.device->etxFences(), static_cast<size_t>(4));
}

PD_TEST(star_etb_happy_path_enables_asb_clears_the_counter_and_confirms) {
  Rig rig(fastStarProfile(CompletionMechanism::StarEtb));
  auto job = rig.printer->print(textPayload("KITCHEN"));
  const JobResult result = job->result();

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.method, std::string("ETB"));
  CHECK_EQ(rig.link.device->asbEnables(), static_cast<size_t>(1));
  CHECK_EQ(rig.link.device->counterClears(), static_cast<size_t>(1));
  CHECK_EQ(rig.link.device->etbFences(), static_cast<size_t>(2));
}

PD_TEST(star_etb_survives_the_counter_wrapping_from_31_to_0) {
  Rig rig(fastStarProfile(CompletionMechanism::StarEtb));
  // Two fences per job, so twenty jobs drive the counter past 31 -> 0 twice. A runtime
  // that expected a monotonically increasing number would fail on the sixteenth job.
  for (int i = 0; i < 20; ++i) {
    auto job = rig.printer->print(textPayload("TICKET " + std::to_string(i)));
    const JobResult result = job->result();
    CHECK_EQ(result.outcome, JobOutcome::Done);
    CHECK_EQ(result.method, std::string("ETB"));
  }
  CHECK_EQ(rig.link.device->etbFences(), static_cast<size_t>(40));
}

PD_TEST(star_asb_broadcast_from_another_host_never_confirms_our_job) {
  // THE GUARD. The printer emits an ASB frame carrying a counter that moved for somebody
  // else's data, before this driver has any fence outstanding, and then never answers our
  // own fence at all.
  //
  // A driver that reads "the counter changed" as "my data printed" reports Done here, on
  // a receipt that is still sitting in the buffer. The honest answer is Unknown — plus a
  // loud ForeignWriterDetected, because a printer with two writers is a topology problem
  // and not a transient.
  CapabilityProfile profile = fastStarProfile(CompletionMechanism::StarEtb);
  Rig rig(profile);
  pdfake::StarScript script;
  script.broadcast_foreign_asb = true;
  script.foreign_counter = 9;
  script.answer_etb = false;  // our own fence is never answered
  rig.link.device->setScript(script);

  std::vector<DeviceEvent> events;
  std::mutex events_mutex;
  rig.printer->subscribe([&events, &events_mutex](DeviceEvent event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto job = rig.printer->print(textPayload("NOT MINE"));
  const JobResult result = job->result();

  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK_EQ(result.reason, FailureReason::TimeoutAwaitingCompletion);
  // Nothing was confirmed, so nothing may be graded above transport.
  CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);

  std::lock_guard<std::mutex> lock(events_mutex);
  bool reported = false;
  for (const DeviceEvent event : events) {
    reported = reported || event == DeviceEvent::ForeignWriterDetected;
  }
  CHECK(reported);
}

PD_TEST(star_etb_counter_landing_on_the_wrong_value_is_not_our_completion) {
  // A subtler version of the same failure: the counter moves *while* our fence is
  // outstanding, but not to the value our fence would have produced. Two increments means
  // two jobs finished, and only one of them can be ours.
  Rig rig(fastStarProfile(CompletionMechanism::StarEtb));
  pdfake::StarScript script;
  script.etb_counter_skew = 3;
  rig.link.device->setScript(script);

  auto job = rig.printer->print(textPayload("SKEWED"));
  const JobResult result = job->result();
  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK_EQ(result.reason, FailureReason::TimeoutAwaitingCompletion);
}

PD_TEST(star_unanswered_fence_is_unknown_and_never_failed) {
  Rig rig(fastStarProfile(CompletionMechanism::StarEscGsEtx));
  pdfake::StarScript script;
  script.answer_etx = false;
  rig.link.device->setScript(script);

  const JobResult result = rig.printer->print(textPayload("SILENT"))->result();
  // Bytes were sent. The receipt may well be on the counter, and the one thing this SDK
  // must never do is call that Failed.
  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK_EQ(result.reason, FailureReason::TimeoutAwaitingCompletion);
}

PD_TEST(star_raster_can_be_declined_by_the_profile) {
  CapabilityProfile profile = fastStarProfile(CompletionMechanism::StarEscGsEtx);
  profile.star.raster_line_mode = false;
  Rig rig(profile);

  std::vector<uint8_t> gray(32 * 8, 0);
  RasterPayload raster;
  raster.gray = gray;
  raster.width = 32;
  raster.height = 8;
  const JobResult result = rig.printer->print(Payload::raster(raster))->result();

  // The job still completes — the profile declined the image, not the receipt — and no
  // raster command reached the device.
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(rig.link.device->rasterRows(), static_cast<size_t>(0));
}

PD_TEST(star_raster_reaches_the_device_when_the_profile_allows_it) {
  Rig rig(fastStarProfile(CompletionMechanism::StarEscGsEtx));
  std::vector<uint8_t> gray(64 * 4, 0);
  RasterPayload raster;
  raster.gray = gray;
  raster.width = 64;
  raster.height = 4;
  const JobResult result = rig.printer->print(Payload::raster(raster))->result();
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK(rig.link.device->rasterRows() > 0);
}

PD_TEST(star_declared_degradations_are_stated_rather_than_implied) {
  const std::vector<std::string>& degradations = star::declaredDegradations();
  CHECK(degradations.size() >= 5);
  bool mentions_ascii = false;
  bool mentions_symbols = false;
  bool mentions_preflight = false;
  for (const std::string& entry : degradations) {
    mentions_ascii = mentions_ascii || entry.find("ASCII") != std::string::npos;
    mentions_symbols = mentions_symbols || entry.find("barcode") != std::string::npos;
    mentions_preflight = mentions_preflight || entry.find("preflight") != std::string::npos;
  }
  CHECK(mentions_ascii);
  CHECK(mentions_symbols);
  CHECK(mentions_preflight);
}
