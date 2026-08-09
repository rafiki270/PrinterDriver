#include "printerdriver/self_test.hpp"

#include <string>
#include <vector>

#include "fake_printer.hpp"
#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "test_harness.hpp"

// M15 — Printer::selfTest against the scripted device (docs/api.md §15).
//
// What this suite is actually asserting: that the ticket is not a pretty summary of what
// the driver believes, but the OUTPUT of the ordinary path. So every check either reads
// bytes the device received, or reads the tri-state result the ordinary engine produced.
// A self-test that passed while the printer received nothing would be exactly the kind of
// `{success: true}` this SDK exists to refuse.

using namespace pd;

namespace {

struct Rig {
  pdfake::MockLink link;
  std::unique_ptr<PrinterDriver> driver;
  std::shared_ptr<Printer> printer;
  CapabilityProfile profile;

  explicit Rig(CompletionMechanism mechanism = CompletionMechanism::GsParenH) {
    profile = pdfake::fastProfile(mechanism);
    driver.reset(new PrinterDriver(StorageConfig::inMemory()));
  }

  void build() {
    PrinterConfig config;
    config.id = "self-test-unit";
    config.transport = link.factory();
    config.width_dots = escpos::kWidth80mm;
    config.profile = profile;
    printer = driver->addPrinter(config);
  }
};

bool receivedContains(const Rig& rig, const std::string& needle) {
  return rig.link.device->receivedContains(needle);
}

bool anyLineContains(const std::vector<std::string>& lines, const std::string& needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool anyDegradationContains(const std::vector<std::string>& entries,
                            const std::string& needle) {
  return anyLineContains(entries, needle);
}

// The Czech/Hungarian/Polish sample as PC852 bytes. Written out rather than computed so
// the assertion is on the wire form and not on the same transliteration table the
// renderer used: příliš -> p ř(FD) í(A1) l i š(E7).
std::string czechSampleInPc852() {
  return std::string("p\xFD\xA1li\xE7");  // "příliš"
}

}  // namespace

// --- The happy path: an xp-s260m script, Done at grade A ------------------------------

PD_TEST(selftest_prints_one_ticket_and_reaches_done_at_grade_a) {
  Rig rig;  // pdfake::fastProfile(GsParenH) is the probed XP-S260M profile
  rig.build();

  const SelfTestResult result = rig.printer->selfTest();

  // The proof is the ordinary tri-state outcome of the ordinary engine.
  CHECK_EQ(result.result.outcome, JobOutcome::Done);
  CHECK_EQ(result.result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(result.result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.result.method, std::string("GS(H) fn48"));
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));

  // One job, one key, and the key says what it is.
  CHECK_EQ(result.key.rfind("selftest-", 0), static_cast<size_t>(0));
  CHECK(result.job != nullptr);
  CHECK_EQ(result.job->key(), result.key);
  CHECK(rig.driver->findJob(result.key) != nullptr);

  // docs/api.md §14: the ticket carries its own verification identifier, and the token
  // it carries is the token the fence echoed.
  CHECK_EQ(result.print_token.size(), static_cast<size_t>(4));
  CHECK_EQ(result.print_token.substr(0, 2), rig.driver->instanceNonce());
  CHECK(receivedContains(rig, "V:" + result.print_token));
  CHECK(rig.driver->jobByToken(result.print_token) != nullptr);
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(2));
  CHECK_EQ(rig.link.device->markers()[0].token, result.print_token);

  // The summary the caller gets back.
  CHECK_EQ(result.detection.completion, CompletionMechanism::GsParenH);
  CHECK_EQ(result.detection.grade_ceiling, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.detection.printable_width_dots, escpos::kWidth80mm);
  CHECK_EQ(result.detection.chars_per_line, static_cast<uint32_t>(48));
  CHECK(result.detection.degradations.empty());
}

PD_TEST(selftest_ticket_bytes_carry_the_charset_sample_and_the_identity_lines) {
  Rig rig;
  rig.build();
  const SelfTestResult result = rig.printer->selfTest();
  CHECK_EQ(result.result.outcome, JobOutcome::Done);

  // ESC t 18 — PC852. The charset line is the whole point of the ticket and it only
  // means anything if the code page that can carry it was actually selected.
  CHECK(receivedContains(rig, std::string("\x1B\x74\x12", 3)));
  CHECK(receivedContains(rig, czechSampleInPc852()));

  // The report's own labels, on the paper.
  CHECK(receivedContains(rig, "PRINTERDRIVER SELF-TEST"));
  CHECK(receivedContains(rig, "IDENTITY"));
  CHECK(receivedContains(rig, "PROFILE"));
  CHECK(receivedContains(rig, "MEDIA"));
  CHECK(receivedContains(rig, "COMPLETION"));
  CHECK(receivedContains(rig, "CHARSET"));
  CHECK(receivedContains(rig, "DRAWER"));
  CHECK(receivedContains(rig, "FENCE"));
  // The FENCE block names the mechanism this very ticket is being confirmed by.
  CHECK(receivedContains(rig, "GS(H) fn48"));
  CHECK(receivedContains(rig, "sdk "));

  // Code 128 as GS k function B, m = 73: the sample really is a symbol and not a line
  // of text that says "barcode".
  CHECK(receivedContains(rig, std::string("\x1D\x6B\x49", 3)));
  // GS ( k fn 181 — the trailer QR being printed (docs/api.md §14).
  CHECK(receivedContains(rig, std::string("\x1D\x28\x6B\x03\x00\x31\x51", 7)));

  // The same layout that produced the bytes is the one the caller can show.
  CHECK(anyLineContains(result.ticket_lines, "PRINTERDRIVER SELF-TEST"));
  CHECK(anyLineContains(result.ticket_lines, "CHARSET"));
}

PD_TEST(selftest_reports_the_profile_selection_and_its_provenance) {
  Rig rig;
  rig.profile = devices::epson_tm_t88vi();
  rig.profile.completion_timeout_ms = 200;
  rig.profile.preflight_timeout_ms = 200;
  rig.build();

  const SelfTestResult result = rig.printer->selfTest();
  CHECK_EQ(result.result.outcome, JobOutcome::Done);
  // Nothing has probed this device, so the profile is the one that was configured and
  // the ticket says Documented rather than pretending anything was measured.
  CHECK_EQ(result.detection.selection, ProfileSelection::Documented);
  CHECK_EQ(result.detection.completion_provenance, Provenance::Documented);
  // Wrap-safe substrings: a 48-column line can break between any two words, and the
  // ticket is expected to lay out on 58 mm media too.
  CHECK(receivedContains(rig, "epson_tm_t88vi"));
  CHECK(receivedContains(rig, "selected by"));
  CHECK(receivedContains(rig, "Documented"));
  CHECK(receivedContains(rig, "grade ceiling A"));
}

PD_TEST(selftest_declares_a_missing_barcode_path_on_the_ticket_itself) {
  Rig rig;
  // A profile whose firmware has no GS k. The renderer refuses to half-draw a symbol,
  // and the refusal is printed rather than swallowed (docs/receipt-dsl.md degradation
  // rules, docs/api.md §15 "blocks the profile can't do appear as declared
  // degradations on the ticket itself").
  rig.profile.render.barcode_gs_k = false;
  rig.build();

  const SelfTestResult result = rig.printer->selfTest();
  CHECK_EQ(result.result.outcome, JobOutcome::Done);
  CHECK(anyDegradationContains(result.detection.degradations,
                               "BARCODE not supported on this path"));
  CHECK(receivedContains(rig, "BARCODE not supported on this path"));
  CHECK(receivedContains(rig, "DECLARED DEGRADATIONS"));
  // Declared, and therefore not drawn: no GS k reached the device.
  CHECK(!receivedContains(rig, std::string("\x1D\x6B\x49", 3)));
}

PD_TEST(selftest_on_a_queued_fence_admits_the_lower_ceiling) {
  Rig rig(CompletionMechanism::GsR1);
  rig.build();

  const SelfTestResult result = rig.printer->selfTest();
  CHECK_EQ(result.result.outcome, JobOutcome::Done);
  // GS r 1 is an ordered fence and nothing more: grade B, and no per-receipt token, so
  // there is no `V:` line to print (docs/api.md §14).
  CHECK_EQ(result.result.grade, ConfidenceGrade::B_OrderedDeviceResponse);
  CHECK_EQ(result.detection.grade_ceiling, ConfidenceGrade::B_OrderedDeviceResponse);
  CHECK_EQ(result.print_token, std::string());
  CHECK(!receivedContains(rig, "V:"));
  CHECK(receivedContains(rig, "grade ceiling B"));
}

PD_TEST(selftest_is_idempotent_on_its_own_key_like_every_other_job) {
  Rig rig;
  rig.build();

  SelfTestOptions options;
  options.key = "selftest-fixed";
  const SelfTestResult first = rig.printer->selfTest(options);
  const size_t bytes_after_first = rig.link.device->printDataBytes();
  const SelfTestResult second = rig.printer->selfTest(options);

  CHECK_EQ(first.result.outcome, JobOutcome::Done);
  CHECK_EQ(second.result.outcome, JobOutcome::Done);
  // Re-submitting a known key returns the existing job and prints nothing.
  CHECK_EQ(rig.link.device->printDataBytes(), bytes_after_first);
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
  CHECK(first.job == second.job);
}

PD_TEST(selftest_refresh_identity_runs_the_probe_and_promotes_what_it_finds) {
  Rig rig;
  pdfake::Script script;
  script.answer_identity = true;  // Rongta's documented Epson impersonation
  rig.link.device->setScript(script);
  rig.profile = devices::generic_80();
  rig.profile.completion_timeout_ms = 200;
  rig.profile.preflight_timeout_ms = 200;
  rig.build();

  SelfTestOptions options;
  options.refresh_identity = true;
  const SelfTestResult result = rig.printer->selfTest(options);

  CHECK_EQ(result.result.outcome, JobOutcome::Done);
  CHECK(result.detection.identity_fresh);
  CHECK_EQ(result.detection.identity.model, std::string("TM-T88V"));
  // GS I answered "EPOSN"/"TM-T88V" and nothing independent agrees, so the identity is
  // reported and not believed — and the ticket says so.
  CHECK(!result.detection.identity.trusted);
  CHECK(receivedContains(rig, "trusted NO"));
  // The probe established the process-ID echo first-hand, so the profile in force is
  // now a probed one.
  CHECK_EQ(result.detection.selection, ProfileSelection::Probed);
  CHECK_EQ(result.detection.completion_provenance, Provenance::Probed);
  CHECK(receivedContains(rig, "selected by Probed"));
}

PD_TEST(selftest_on_a_language_this_engine_cannot_drive_is_refused_not_faked) {
  Rig rig;
  rig.profile = devices::zebra_zq600_plus();
  rig.build();

  const SelfTestResult result = rig.printer->selfTest();
  // ZPL is not ESC/POS at any level. The ticket is still laid out — that is the report
  // an operator needs — and the job is refused before a byte reaches the link.
  CHECK_EQ(result.result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.result.reason, FailureReason::Unsupported);
  CHECK_EQ(rig.link.device->printDataBytes(), static_cast<size_t>(0));
  CHECK(anyLineContains(result.ticket_lines, "REFUSED"));
  CHECK(anyDegradationContains(result.detection.degradations,
                               "BARCODE not supported on this path"));
}
