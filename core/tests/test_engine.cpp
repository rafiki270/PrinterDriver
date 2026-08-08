#include "printerdriver/driver.hpp"

#include <chrono>
#include <string>

#include "fake_printer.hpp"
#include "printerdriver/device_profiles.hpp"
#include "test_harness.hpp"

using namespace pd;
using namespace std::chrono_literals;

namespace {

escpos::Bytes textPayload(const std::string& text) {
  escpos::Encoder encoder;
  encoder.line(text);
  return encoder.take();
}

// First offset of `needle` in `hay` at or after `from`, or npos. Written by hand
// rather than pulling in <algorithm> for one std::search call.
size_t findSubsequence(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle,
                       size_t from = 0) {
  if (needle.empty() || hay.size() < needle.size()) {
    return std::string::npos;
  }
  for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
    size_t j = 0;
    for (; j < needle.size(); ++j) {
      if (hay[i + j] != needle[j]) {
        break;
      }
    }
    if (j == needle.size()) {
      return i;
    }
  }
  return std::string::npos;
}

struct Rig {
  pdfake::MockLink link;
  std::unique_ptr<PrinterDriver> driver;
  std::shared_ptr<Printer> printer;

  Rig(CompletionMechanism mechanism, StorageConfig storage = StorageConfig::inMemory()) {
    profile = pdfake::fastProfile(mechanism);
    driver.reset(new PrinterDriver(std::move(storage)));
  }

  // Deferred so a test can adjust `profile` / `link.behaviour` / the device script
  // between construction and the first job.
  void build() {
    PrinterConfig config;
    config.id = "printer-under-test";
    config.transport = link.factory();
    config.width_dots = escpos::kWidth80mm;
    config.profile = profile;
    printer = driver->addPrinter(config);
  }

  CapabilityProfile profile;
};

JobResult runOne(Rig& rig, const std::string& text, JobOptions options = {}) {
  auto job = rig.printer->print(Payload::raw(textPayload(text)), options);
  return job->result();
}

}  // namespace

// --- a. Strong sequence, GS ( H --------------------------------------------------

PD_TEST(engine_gsparenh_happy_path_reaches_done_cut_fault_free) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  const JobResult result = runOne(rig, "KITCHEN TICKET 7F3A");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(result.reason, FailureReason::None);
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(2));
  CHECK_EQ(rig.link.device->markers()[0].token.substr(0, 1), std::string("P"));
  CHECK_EQ(rig.link.device->markers()[1].token.substr(0, 1), std::string("C"));
  // Same three-digit suffix on both markers: one receipt, one correlation token.
  CHECK_EQ(rig.link.device->markers()[0].token.substr(1),
           rig.link.device->markers()[1].token.substr(1));
  CHECK(rig.link.device->printText().find("KITCHEN TICKET 7F3A") != std::string::npos);
  CHECK(!rig.link.device->sawConcurrentWrites());
  // Preflight 1-4 then the post-cut cutter read.
  CHECK_EQ(rig.link.device->realtimeRequests().size(), static_cast<size_t>(5));
  CHECK_EQ(rig.link.device->realtimeRequests()[4], static_cast<uint8_t>(3));
}

PD_TEST(engine_gsparenh_walks_the_full_state_machine_in_order) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  auto job = rig.printer->print(Payload::raw(textPayload("ORDERED")), {});
  job->result();

  std::vector<JobState> seen;
  job->subscribe([&seen](const JobEvent& event) { seen.push_back(event.state); });
  const std::vector<JobState> expected{
      JobState::Queued,         JobState::PreflightOk,          JobState::SendStarted,
      JobState::BytesSent,      JobState::PrintConfirmed,       JobState::CutCommandProcessed,
      JobState::DoneSoftware};
  CHECK_EQ(seen.size(), expected.size());
  for (size_t i = 0; i < expected.size() && i < seen.size(); ++i) {
    CHECK_EQ(seen[i], expected[i]);
  }
  CHECK(job->isTerminal());
  CHECK_EQ(job->state(), JobState::DoneSoftware);
}

// --- b. Fallback sequence, GS r 1 ------------------------------------------------

PD_TEST(engine_gsr1_fallback_happy_path_caps_at_cut_processed) {
  Rig rig(CompletionMechanism::GsR1);
  rig.build();
  const JobResult result = runOne(rig, "FALLBACK TICKET");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  // Not CutFaultFree: a second GS r 1 fences the cut but proves nothing about the
  // blade (docs/techspec.md §3.2).
  CHECK_EQ(result.confidence, ConfidenceLevel::CutProcessed);
  CHECK_EQ(rig.link.device->queuedRequests(), static_cast<size_t>(2));
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(0));
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
  // No post-cut cutter read on this path, so only the four preflight queries.
  CHECK_EQ(rig.link.device->realtimeRequests().size(), static_cast<size_t>(4));
}

// --- c. Write-only printer -------------------------------------------------------

PD_TEST(engine_none_profile_is_done_with_transport_accepted_only) {
  Rig rig(CompletionMechanism::None);
  rig.build();
  const JobResult result = runOne(rig, "WRITE ONLY");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::TransportAccepted);
  CHECK_EQ(rig.link.device->realtimeRequests().size(), static_cast<size_t>(0));
  CHECK_EQ(rig.link.device->queuedRequests(), static_cast<size_t>(0));
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(0));
  // The cut still has to happen; it just travels with the payload.
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
}

// --- d. Acknowledgement never arrives --------------------------------------------

PD_TEST(engine_missing_completion_ack_is_unknown_not_failed) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.answer_process_id = false;
  rig.link.device->setScript(script);
  rig.build();
  const JobResult result = runOne(rig, "NO ACK");

  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK_EQ(result.reason, FailureReason::TimeoutAwaitingCompletion);
  // Preflight succeeded, so that much is still true.
  CHECK_EQ(result.confidence, ConfidenceLevel::PrinterHealthy);
  CHECK(rig.link.device->printDataBytes() > 0);
}

PD_TEST(engine_missing_cut_ack_is_unknown_at_print_confirmed) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.process_id_answer_limit = 1;  // print marker answered, cut marker not
  rig.link.device->setScript(script);
  rig.build();
  const JobResult result = runOne(rig, "HALF FENCED");

  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK_EQ(result.reason, FailureReason::TimeoutAwaitingCompletion);
  CHECK_EQ(result.confidence, ConfidenceLevel::PrintConfirmed);
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
}

// --- e. Link drops mid-payload ---------------------------------------------------

PD_TEST(engine_disconnect_mid_payload_is_unknown) {
  Rig rig(CompletionMechanism::GsParenH);
  // 12 preflight bytes plus a slice of the payload: bytes provably left the host.
  rig.link.behaviour.fail_write_after_bytes = 20;
  rig.build();
  const JobResult result = runOne(rig, "TRUNCATED TICKET WITH PLENTY OF BYTES");

  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK(rig.link.device->received().size() > static_cast<size_t>(12));
  CHECK_EQ(rig.link.stats->bytes.load(), static_cast<size_t>(20));
}

PD_TEST(engine_disconnect_before_first_payload_byte_is_failed_known) {
  Rig rig(CompletionMechanism::GsParenH);
  // Exactly the preflight probe, so the payload write is refused with zero accepted.
  rig.link.behaviour.fail_write_after_bytes = 12;
  rig.build();
  const JobResult result = runOne(rig, "NEVER SENT");

  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::TransportUnreachable);
  CHECK_EQ(rig.link.device->printDataBytes(), static_cast<size_t>(0));
}

// --- f. Connection refused -------------------------------------------------------

PD_TEST(engine_connect_refused_is_failed_known_transport_unreachable) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.link.behaviour.connect_fails = true;
  rig.build();
  const JobResult result = runOne(rig, "UNREACHABLE");

  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::TransportUnreachable);
  CHECK_EQ(result.confidence, ConfidenceLevel::TransportAccepted);
  CHECK_EQ(rig.link.device->received().size(), static_cast<size_t>(0));
}

// --- g. Preflight refusals -------------------------------------------------------

PD_TEST(engine_preflight_paper_out_refuses_before_any_payload_byte) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.paper_sensor = pdfake::paperOutByte();
  rig.link.device->setScript(script);
  rig.build();
  const JobResult result = runOne(rig, "SHOULD NOT PRINT");

  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::PreflightPaperOut);
  CHECK_EQ(rig.link.device->printDataBytes(), static_cast<size_t>(0));
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(0));
  // Only the four DLE EOT queries reached the device.
  CHECK_EQ(rig.link.device->received().size(), static_cast<size_t>(12));
}

PD_TEST(engine_preflight_cover_open_refuses_with_its_own_reason) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.offline_cause = pdfake::coverOpenByte();
  rig.link.device->setScript(script);
  rig.build();
  const JobResult result = runOne(rig, "SHOULD NOT PRINT");

  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::PreflightCoverOpen);
  CHECK_EQ(rig.link.device->printDataBytes(), static_cast<size_t>(0));
}

PD_TEST(engine_preflight_skip_sends_no_status_queries) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.paper_sensor = pdfake::paperOutByte();  // ignored: nobody asks
  rig.link.device->setScript(script);
  rig.build();
  JobOptions options;
  options.preflight = PreflightMode::Skip;
  const JobResult result = runOne(rig, "UNCHECKED", options);

  CHECK_EQ(result.outcome, JobOutcome::Done);
  // Only the post-cut cutter read, never a preflight query.
  CHECK_EQ(rig.link.device->realtimeRequests().size(), static_cast<size_t>(1));
  CHECK_EQ(rig.link.device->realtimeRequests()[0], static_cast<uint8_t>(3));
}

PD_TEST(engine_cutter_error_after_the_fence_is_failed_known) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.error_cause = pdfake::cutterErrorByte();
  rig.link.device->setScript(script);
  rig.build();
  JobOptions options;
  options.preflight = PreflightMode::Skip;  // the fault must appear only after the cut
  const JobResult result = runOne(rig, "JAMMED CUTTER", options);

  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::CutterFault);
  // The receipt did print, and the result says so.
  CHECK_EQ(result.confidence, ConfidenceLevel::CutProcessed);
}

PD_TEST(engine_unreadable_cutter_bit_stops_at_cut_processed) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.answer_realtime = false;  // no DLE EOT answers at all
  rig.link.device->setScript(script);
  rig.build();
  const JobResult result = runOne(rig, "SILENT STATUS");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  // Silence is never upgraded into CutFaultFree.
  CHECK_EQ(result.confidence, ConfidenceLevel::CutProcessed);
}

// --- h. Idempotency ---------------------------------------------------------------

PD_TEST(engine_same_key_returns_the_same_job_and_prints_once) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-7F3A-92C1#kitchen-1";

  auto first = rig.printer->print(Payload::raw(textPayload("ONE COPY ONLY")), options);
  first->result();
  auto second = rig.printer->print(Payload::raw(textPayload("ONE COPY ONLY")), options);

  CHECK(first.get() == second.get());
  CHECK_EQ(second->attempt(), 1u);
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(2));
  CHECK(rig.driver->findJob(options.key).get() == first.get());
}

PD_TEST(engine_resubmitting_a_failed_key_starts_a_fresh_attempt) {
  // Regression from the hardware soak: a preflight refusal is FailedKnown, nothing
  // printed, and docs/api.md §4 calls that "safe to resubmit same key". Returning the
  // dead job instead swallowed the retry and the ticket never appeared.
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.paper_sensor = pdfake::paperOutByte();
  rig.link.device->setScript(script);
  rig.build();
  JobOptions options;
  options.key = "order-refused";

  auto refused = rig.printer->print(Payload::raw(textPayload("KITCHEN TICKET")), options);
  CHECK_EQ(refused->result().outcome, JobOutcome::Failed);
  CHECK_EQ(refused->result().reason, FailureReason::PreflightPaperOut);
  CHECK_EQ(rig.link.device->printDataBytes(), static_cast<size_t>(0));

  rig.link.device->setScript(pdfake::Script{});  // operator reloads the paper
  auto retried = rig.printer->print(Payload::raw(textPayload("KITCHEN TICKET")), options);
  CHECK(retried.get() != refused.get());
  CHECK_EQ(retried->attempt(), 2u);
  CHECK_EQ(retried->key(), options.key);
  CHECK_EQ(retried->result().outcome, JobOutcome::Done);
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(1));
  CHECK(rig.link.device->receivedContains("KITCHEN TICKET"));
  // Nothing was printed the first time, so there is no duplicate to warn about: the
  // banner belongs to forceReprint, not to the attempt counter.
  CHECK(!rig.link.device->receivedContains(kReprintBannerLine));
  CHECK(!rig.link.device->receivedContains(std::string(kReprintAttemptPrefix) + "2"));
  CHECK(rig.driver->findJob(options.key).get() == retried.get());
}

PD_TEST(engine_resubmitting_an_unknown_key_still_returns_the_same_job) {
  // The other half of the rule: Unknown means bytes went out and may be on paper, so
  // a resubmission must not print again. Only a deliberate forceReprint may.
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.answer_process_id = false;
  rig.link.device->setScript(script);
  rig.build();
  JobOptions options;
  options.key = "order-ambiguous";

  auto first = rig.printer->print(Payload::raw(textPayload("MAYBE PRINTED")), options);
  CHECK_EQ(first->result().outcome, JobOutcome::Unknown);
  auto second = rig.printer->print(Payload::raw(textPayload("MAYBE PRINTED")), options);
  CHECK(first.get() == second.get());
  CHECK_EQ(second->attempt(), 1u);
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(1));
}

PD_TEST(engine_resubmitting_a_key_mid_flight_still_returns_the_same_job) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.answer_process_id = false;  // job stays in flight until it times out
  rig.link.device->setScript(script);
  rig.build();
  JobOptions options;
  options.key = "order-inflight";

  auto first = rig.printer->print(Payload::raw(textPayload("IN FLIGHT")), options);
  auto second = rig.printer->print(Payload::raw(textPayload("IN FLIGHT")), options);
  CHECK(first.get() == second.get());
  CHECK_EQ(first->result().outcome, JobOutcome::Unknown);
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(1));
}

// --- i. forceReprint --------------------------------------------------------------

PD_TEST(engine_force_reprint_prints_again_with_the_duplicate_banner) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-7F3A-92C1#kitchen-1";

  auto first = rig.printer->print(Payload::raw(textPayload("ORIGINAL")), options);
  CHECK_EQ(first->result().outcome, JobOutcome::Done);
  CHECK(!rig.link.device->receivedContains(kReprintBannerLine));

  auto second = rig.driver->forceReprint(options.key);
  CHECK(second != nullptr);
  CHECK(second.get() != first.get());
  CHECK_EQ(second->result().outcome, JobOutcome::Done);
  CHECK_EQ(second->attempt(), 2u);
  CHECK_EQ(second->key(), options.key);

  CHECK(rig.link.device->receivedContains(kReprintBannerLine));
  CHECK(rig.link.device->receivedContains(std::string(kReprintAttemptPrefix) + "2"));
  CHECK(rig.link.device->receivedContains("ORDER: " + options.key));
  // Two physical prints: the original and the marked duplicate.
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(2));
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(4));
}

PD_TEST(engine_force_reprint_of_an_unknown_key_returns_nothing) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  CHECK(rig.driver->forceReprint("never-submitted") == nullptr);
  CHECK(rig.driver->findJob("never-submitted") == nullptr);
}

// --- j. FIFO, one active job per printer ------------------------------------------

PD_TEST(engine_two_jobs_run_fifo_without_interleaving) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  auto first = rig.printer->print(Payload::raw(textPayload("JOB-ALPHA")), {});
  auto second = rig.printer->print(Payload::raw(textPayload("JOB-BRAVO")), {});
  CHECK_EQ(first->result().outcome, JobOutcome::Done);
  CHECK_EQ(second->result().outcome, JobOutcome::Done);

  const std::string stream = rig.link.device->printText();
  const size_t alpha = stream.find("JOB-ALPHA");
  const size_t bravo = stream.find("JOB-BRAVO");
  CHECK(alpha != std::string::npos);
  CHECK(bravo != std::string::npos);
  CHECK(alpha < bravo);

  const std::vector<pdfake::MarkerRecord> markers = rig.link.device->markers();
  CHECK_EQ(markers.size(), static_cast<size_t>(4));
  // Both of the first job's fences are reached before any of the second job's text.
  CHECK(markers[1].print_data_len <= bravo);
  CHECK(markers[2].print_data_len >= bravo);
  CHECK(!rig.link.device->sawConcurrentWrites());
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(2));
}

// --- k. Crash recovery through the driver -----------------------------------------

PD_TEST(engine_reloads_an_in_flight_journal_job_as_unknown) {
  pdfake::TempDir dir("engine-recovery");
  {
    JobStore store(StorageConfig::at(dir.path()));
    JobRecord record;
    record.id = "job-crashed";
    record.key = "order-crashed";
    record.printer_id = "printer-under-test";
    record.payload_kind = PayloadKind::Raw;
    store.createJob(record);
    store.recordState("job-crashed", JobState::SendStarted,
                      ConfidenceLevel::PrinterHealthy, FailureReason::None);
  }

  PrinterDriver driver(StorageConfig::at(dir.path()));
  auto job = driver.findJob("order-crashed");
  CHECK(job != nullptr);
  CHECK(job->isTerminal());
  CHECK_EQ(job->state(), JobState::Unknown);
  const JobResult result = job->result();
  CHECK_EQ(result.outcome, JobOutcome::Unknown);
  CHECK_EQ(result.confidence, ConfidenceLevel::PrinterHealthy);
}

PD_TEST(engine_reloaded_job_blocks_a_resubmission_of_the_same_key) {
  pdfake::TempDir dir("engine-recovery-dedupe");
  {
    PrinterDriver driver(StorageConfig::at(dir.path()));
    pdfake::MockLink link;
    PrinterConfig config;
    config.id = "printer-under-test";
    config.transport = link.factory();
    config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
    auto printer = driver.addPrinter(config);
    JobOptions options;
    options.key = "order-persisted";
    printer->print(Payload::raw(textPayload("FIRST RUN")), options)->result();
  }

  PrinterDriver driver(StorageConfig::at(dir.path()));
  pdfake::MockLink link;
  PrinterConfig config;
  config.id = "printer-under-test";
  config.transport = link.factory();
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  auto printer = driver.addPrinter(config);
  JobOptions options;
  options.key = "order-persisted";
  auto job = printer->print(Payload::raw(textPayload("SECOND RUN")), options);
  CHECK_EQ(job->result().outcome, JobOutcome::Done);
  // Nothing reached the wire on this run: the key was already known.
  CHECK_EQ(link.device->received().size(), static_cast<size_t>(0));
  CHECK_EQ(link.stats->connects.load(), static_cast<size_t>(0));
}

PD_TEST(engine_reloaded_dedupe_return_carries_the_original_grade) {
  // Regression for the hardware soak's other finding: a journal-reloaded job that
  // earned grade A reported E_TransportOnly/TransportOnly/"none" on a same-key
  // dedupe return, which is internally inconsistent with a CutFaultFree confidence.
  pdfake::TempDir dir("engine-recovery-grade");
  {
    PrinterDriver driver(StorageConfig::at(dir.path()));
    pdfake::MockLink link;
    PrinterConfig config;
    config.id = "printer-under-test";
    config.transport = link.factory();
    config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
    auto printer = driver.addPrinter(config);
    JobOptions options;
    options.key = "order-graded-a";
    const JobResult first =
        printer->print(Payload::raw(textPayload("GRADED ORIGINAL")), options)->result();
    CHECK_EQ(first.outcome, JobOutcome::Done);
    CHECK_EQ(first.confidence, ConfidenceLevel::CutFaultFree);
    CHECK_EQ(first.grade, ConfidenceGrade::A_JobLevelConfirmation);
  }

  // Fresh process: the job is reconstructed from the journal, not replayed in memory.
  PrinterDriver driver(StorageConfig::at(dir.path()));
  pdfake::MockLink link;
  PrinterConfig config;
  config.id = "printer-under-test";
  config.transport = link.factory();
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  auto printer = driver.addPrinter(config);
  JobOptions options;
  options.key = "order-graded-a";

  const auto reloaded = driver.findJob(options.key);
  CHECK(reloaded != nullptr);
  CHECK_EQ(reloaded->result().grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(reloaded->result().authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(reloaded->result().method, std::string("GS(H) fn48"));

  // Resubmitting the same key returns that reconstructed job untouched, and it must
  // still carry the original evidence rather than a fresh TransportOnly default.
  const auto deduped = printer->print(Payload::raw(textPayload("GRADED ORIGINAL")), options);
  CHECK(deduped.get() == reloaded.get());
  const JobResult result = deduped->result();
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.method, std::string("GS(H) fn48"));
  // Nothing reached the wire on this run: the dedupe returned before touching it.
  CHECK_EQ(link.device->received().size(), static_cast<size_t>(0));
}

// --- l. The ordering rule ----------------------------------------------------------

PD_TEST(engine_journals_send_started_before_the_first_payload_byte) {
  pdfake::TempDir dir("engine-ordering");
  const std::string journal = dir.path() + "/jobs.journal";
  const std::string marker_text = "FENCE-ME-PLEASE";

  bool checked = false;
  bool send_started_was_durable = false;
  size_t print_bytes_at_check = 1;

  Rig rig(CompletionMechanism::GsParenH, StorageConfig::at(dir.path()));
  auto device = rig.link.device;
  rig.link.behaviour.before_write = [&](const uint8_t* data, size_t size) {
    if (checked) {
      return;
    }
    const std::string chunk(reinterpret_cast<const char*>(data), size);
    if (chunk.find(marker_text) == std::string::npos) {
      return;
    }
    checked = true;
    print_bytes_at_check = device->printDataBytes();
    for (const JournalEntry& entry : readJournal(journal)) {
      if (entry.kind == JournalEntry::Kind::State &&
          entry.record.state == JobState::SendStarted) {
        send_started_was_durable = true;
      }
    }
  };
  rig.build();
  const JobResult result = runOne(rig, marker_text);

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK(checked);
  // Read straight off disk, from the writing thread, before the device saw the byte.
  CHECK(send_started_was_durable);
  CHECK_EQ(print_bytes_at_check, static_cast<size_t>(0));
}

// --- m. Pre-cut head-to-cutter feed -----------------------------------------------

PD_TEST(engine_feeds_head_to_cutter_dots_immediately_before_every_cut) {
  for (const CompletionMechanism mechanism :
       {CompletionMechanism::GsParenH, CompletionMechanism::GsR1}) {
    Rig rig(mechanism);
    rig.profile.media.head_to_cutter_feed_dots = 120;
    rig.build();
    const JobResult result = runOne(rig, "TRAILING QR AT THE VERY END");
    CHECK_EQ(result.outcome, JobOutcome::Done);

    const std::vector<uint8_t> received = rig.link.device->received();
    const std::vector<uint8_t> payload_text{'T', 'R', 'A', 'I', 'L', 'I', 'N', 'G'};
    const std::vector<uint8_t> feed_bytes{0x1B, 0x4A, 0x78};  // ESC J 120 (0x78)
    const std::vector<uint8_t> cut_bytes{0x1D, 0x56, 0x01};   // GS V 1 (partial cut)

    const size_t payload_pos = findSubsequence(received, payload_text);
    const size_t feed_pos = findSubsequence(received, feed_bytes);
    const size_t cut_pos = findSubsequence(received, cut_bytes);
    CHECK(payload_pos != std::string::npos);
    CHECK(feed_pos != std::string::npos);
    CHECK(cut_pos != std::string::npos);
    CHECK(feed_pos > payload_pos);
    // Nothing rides between the feed and the cut it guarantees: the configured dots
    // land immediately before GS V, on the wire, for both fence mechanisms.
    CHECK_EQ(feed_pos + feed_bytes.size(), cut_pos);
  }
}

PD_TEST(engine_head_to_cutter_feed_is_chunked_above_255_dots_on_the_wire) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.profile.media.head_to_cutter_feed_dots = 300;
  rig.build();
  const JobResult result = runOne(rig, "WIDE GAP PROFILE");
  CHECK_EQ(result.outcome, JobOutcome::Done);

  const std::vector<uint8_t> received = rig.link.device->received();
  const std::vector<uint8_t> feed_bytes{0x1B, 0x4A, 0xFF, 0x1B, 0x4A, 0x2D};  // 255 + 45
  const std::vector<uint8_t> cut_bytes{0x1D, 0x56, 0x01};
  const size_t feed_pos = findSubsequence(received, feed_bytes);
  CHECK(feed_pos != std::string::npos);
  CHECK_EQ(feed_pos + feed_bytes.size(), findSubsequence(received, cut_bytes));
}

// --- Profiles ----------------------------------------------------------------------

PD_TEST(profile_ceilings_match_their_completion_mechanism) {
  CHECK_EQ(xp_s260m().maxConfidence(), ConfidenceLevel::CutFaultFree);
  CHECK_EQ(generic_escpos().maxConfidence(), ConfidenceLevel::CutProcessed);
  CapabilityProfile none;
  none.completion = CompletionMechanism::None;
  CHECK_EQ(none.maxConfidence(), ConfidenceLevel::TransportAccepted);

  CHECK_EQ(xp_s260m().completion, CompletionMechanism::GsParenH);
  CHECK_EQ(xp_s260m().cut, CutVariant::Partial);
  CHECK_EQ(xp_s260m().code_page, escpos::CodePage::PC852);
  CHECK_EQ(generic_escpos().completion, CompletionMechanism::GsR1);
  CHECK(generic_escpos().chunk_bytes > 0);
  CHECK(generic_escpos().inter_chunk_delay_ms > 0);
  CHECK_EQ(std::string(to_string(CompletionMechanism::GsParenH)), std::string("GsParenH"));
  CHECK_EQ(std::string(to_string(CutVariant::Partial)), std::string("Partial"));
}

PD_TEST(engine_never_reports_above_the_profile_ceiling) {
  Rig rig(CompletionMechanism::GsR1);
  rig.build();
  auto job = rig.printer->print(Payload::raw(textPayload("CAPPED")), {});
  job->result();
  for (const JobEvent& event : job->history()) {
    CHECK(static_cast<int>(event.confidence) <=
          static_cast<int>(rig.profile.maxConfidence()));
  }
}

// --- Options and payload tiers ------------------------------------------------------

PD_TEST(engine_cut_none_stops_at_print_confirmed) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.cut = CutSetting::None;
  const JobResult result = runOne(rig, "NO CUT", options);

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::PrintConfirmed);
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(0));
  CHECK_EQ(rig.link.device->markers().size(), static_cast<size_t>(1));
}

PD_TEST(engine_open_drawer_option_kicks_the_drawer_inside_the_fence) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.open_drawer = true;
  const JobResult result = runOne(rig, "WITH DRAWER", options);

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(rig.link.device->drawerKicks(), static_cast<size_t>(1));
}

PD_TEST(engine_prints_all_three_payload_tiers) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();

  escpos::Encoder document;
  document.align(escpos::Alignment::Center).bold(true).line("DOCUMENT TIER");
  auto document_job = rig.printer->print(Payload::document(document), {});
  CHECK_EQ(document_job->result().outcome, JobOutcome::Done);
  CHECK(rig.link.device->receivedContains("DOCUMENT TIER"));

  std::vector<uint8_t> gray(32 * 8, 0);
  auto raster_job =
      rig.printer->print(Payload::raster(std::move(gray), 32, 8), {});
  CHECK_EQ(raster_job->result().outcome, JobOutcome::Done);
  CHECK(rig.link.device->rasterBlocks() > 0);

  auto raw_job = rig.printer->print(Payload::raw(textPayload("RAW TIER")), {});
  CHECK_EQ(raw_job->result().outcome, JobOutcome::Done);
  CHECK(rig.link.device->receivedContains("RAW TIER"));

  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(3));
  CHECK(!rig.link.device->sawConcurrentWrites());
}

PD_TEST(engine_chunk_pacing_splits_the_payload_into_several_writes) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.profile.chunk_bytes = 8;
  rig.profile.inter_chunk_delay_ms = 0;
  rig.build();
  const JobResult result = runOne(rig, "A PAYLOAD LONG ENOUGH TO NEED SEVERAL CHUNKS");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK(rig.link.stats->writes.load() > static_cast<size_t>(6));
  CHECK(rig.link.device->receivedContains("A PAYLOAD LONG ENOUGH TO NEED SEVERAL CHUNKS"));
}

PD_TEST(engine_markers_are_never_reused_while_outstanding) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(runOne(rig, "TICKET " + std::to_string(i)).outcome, JobOutcome::Done);
  }
  const std::vector<pdfake::MarkerRecord> markers = rig.link.device->markers();
  CHECK_EQ(markers.size(), static_cast<size_t>(6));
  for (size_t i = 0; i < markers.size(); ++i) {
    for (size_t j = i + 1; j < markers.size(); ++j) {
      CHECK(markers[i].token != markers[j].token);
    }
  }
}

// --- Device status and events --------------------------------------------------------

PD_TEST(engine_publishes_device_events_and_a_status_snapshot) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.paper_sensor = pdfake::paperOutByte();
  rig.link.device->setScript(script);
  rig.build();

  std::vector<DeviceEvent> printer_events;
  std::vector<std::string> driver_event_printers;
  rig.printer->subscribe(
      [&printer_events](DeviceEvent event) { printer_events.push_back(event); });
  rig.driver->subscribeDevices(
      [&driver_event_printers](const std::string& id, DeviceEvent) {
        driver_event_printers.push_back(id);
      });

  const DeviceStatus status = rig.printer->refreshStatus(500ms);
  CHECK(status.observed);
  CHECK(status.connected);
  CHECK(status.online.value_or(false));
  CHECK(status.paper_out.value_or(false));
  CHECK(!status.cover_open.value_or(true));
  CHECK(std::find(printer_events.begin(), printer_events.end(), DeviceEvent::PaperOut) !=
        printer_events.end());
  CHECK(!driver_event_printers.empty());
  CHECK_EQ(driver_event_printers.front(), std::string("printer-under-test"));
}

PD_TEST(engine_status_snapshot_starts_unobserved) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  const DeviceStatus status = rig.printer->status();
  CHECK(!status.observed);
  CHECK(!status.connected);
  CHECK(!status.online.has_value());
}

PD_TEST(engine_shutdown_terminates_queued_jobs_without_sending) {
  auto link = std::make_shared<pdfake::MockLink>();
  pdfake::Script script;
  script.answer_process_id = false;
  link->device->setScript(script);
  std::shared_ptr<PrintJob> queued;
  {
    PrinterDriver driver(StorageConfig::inMemory());
    PrinterConfig config;
    config.id = "printer-under-test";
    config.transport = link->factory();
    config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
    config.profile.completion_timeout_ms = 5000;
    auto printer = driver.addPrinter(config);
    printer->print(Payload::raw(textPayload("STUCK")), {});
    queued = printer->print(Payload::raw(textPayload("QUEUED BEHIND")), {});
    std::this_thread::sleep_for(50ms);
  }
  CHECK(queued->isTerminal());
  CHECK_EQ(queued->result().outcome, JobOutcome::Failed);
  CHECK(!link->device->receivedContains("QUEUED BEHIND"));
}

// --- Confidence grade and completion authority ------------------------------------------

PD_TEST(engine_grades_a_gsparenh_completion_as_job_level_confirmation) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  const JobResult result = runOne(rig, "GRADED A");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.method, std::string("GS(H) fn48"));
}

PD_TEST(engine_grades_a_gsr1_completion_as_an_ordered_device_response) {
  Rig rig(CompletionMechanism::GsR1);
  rig.build();
  const JobResult result = runOne(rig, "GRADED B");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.grade, ConfidenceGrade::B_OrderedDeviceResponse);
  CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.method, std::string("GS r 1"));
}

PD_TEST(engine_grades_a_write_only_printer_as_transport_only) {
  Rig rig(CompletionMechanism::None);
  rig.build();
  const JobResult result = runOne(rig, "GRADED E");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(result.authority, CompletionAuthority::TransportOnly);
  CHECK_EQ(result.method, std::string("transport-only"));
}

PD_TEST(engine_grades_a_status_refusal_and_an_unacknowledged_job_honestly) {
  {
    // The refusal came out of DLE EOT: device status around the transmission.
    Rig rig(CompletionMechanism::GsParenH);
    pdfake::Script script;
    script.offline_cause = pdfake::coverOpenByte();
    rig.link.device->setScript(script);
    rig.build();
    const JobResult result = runOne(rig, "REFUSED");
    CHECK_EQ(result.outcome, JobOutcome::Failed);
    CHECK_EQ(result.grade, ConfidenceGrade::C_DeviceStatusAround);
    CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
    CHECK_EQ(result.method, std::string("DLE EOT"));
  }
  {
    // No acknowledgement ever arrived, so nothing above transport was confirmed —
    // whatever grade the profile would have been able to claim.
    Rig rig(CompletionMechanism::GsParenH);
    pdfake::Script script;
    script.answer_process_id = false;
    rig.link.device->setScript(script);
    rig.build();
    const JobResult result = runOne(rig, "NEVER FENCED");
    CHECK_EQ(result.outcome, JobOutcome::Unknown);
    CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);
    CHECK_EQ(result.authority, CompletionAuthority::TransportOnly);
  }
  {
    Rig rig(CompletionMechanism::GsParenH);
    rig.link.behaviour.connect_fails = true;
    rig.build();
    const JobResult result = runOne(rig, "UNREACHABLE");
    CHECK_EQ(result.outcome, JobOutcome::Failed);
    CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);
    CHECK_EQ(result.method, std::string("none"));
  }
}

PD_TEST(engine_refuses_a_mechanism_it_cannot_drive_instead_of_printing_blind) {
  // A Star profile describes a checked-block device this core does not speak.
  // Printing it through Star's ESC/POS emulation would put paper out with no fence
  // behind it and a result nobody should believe.
  Rig rig(CompletionMechanism::GsParenH);
  rig.profile = devices::star_tsp100();
  rig.build();
  const JobResult result = runOne(rig, "STAR TICKET");

  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::Unsupported);
  CHECK_EQ(rig.link.device->received().size(), static_cast<size_t>(0));
  CHECK_EQ(rig.link.stats->connects.load(), static_cast<size_t>(0));
}

// --- Probe-then-promote through the driver ------------------------------------------------

namespace {

pdfake::Script probeableDevice() {
  pdfake::Script script;
  script.answer_identity = true;
  script.gs_i_manufacturer = "EPOSN";  // the documented impersonation
  script.gs_i_model = "TM-T88V";
  script.gs_i_firmware = "1.02";
  script.gs_i_serial = "RT99001";
  return script;
}

ProbeOptions fastProbe() {
  ProbeOptions options;
  options.status_timeout_ms = 150;
  options.identity_timeout_ms = 150;
  options.completion_timeout_ms = 400;
  return options;
}

}  // namespace

PD_TEST(engine_probes_on_add_and_promotes_the_profile_before_the_first_job) {
  pdfake::MockLink link;
  link.device->setScript(probeableDevice());

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "printer-under-test";
  config.transport = link.factory();
  // Shipped as an unknown 80 mm clone: GS r 1, capped at CutProcessed.
  config.profile = devices::generic_80();
  config.profile.completion_timeout_ms = 400;
  config.profile.preflight_timeout_ms = 200;
  config.profile.chunk_bytes = 0;
  config.profile.inter_chunk_delay_ms = 0;
  config.probe = ProbePolicy::IfUnknown;
  config.probe_options = fastProbe();
  auto printer = driver.addPrinter(config);

  auto job = printer->print(Payload::raw(textPayload("PROMOTED")), {});
  const JobResult result = job->result();

  // The device answered GS ( H, so the job ran on the strong fence rather than on
  // the conservative default the profile shipped with.
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);

  const CapabilityProfile promoted = printer->profile();
  CHECK(promoted.probed);
  CHECK_EQ(promoted.completion, CompletionMechanism::GsParenH);
  CHECK_EQ(promoted.identity.vendor, std::string("Rongta"));
  CHECK(!promoted.identity.trusted);
  CHECK(promoted.quirks.unreliable_identity);
  // Media is not something a probe can measure, so the profile's facts survive.
  CHECK_EQ(promoted.media.printable_width_dots, escpos::kWidth80mm);

  const std::optional<CapabilityFindings> findings = printer->findings();
  CHECK(findings.has_value());
  CHECK_EQ(findings->reported.model, std::string("TM-T88V"));
  CHECK_EQ(findings->key, std::string("tm_t88v-1_02-rt99001"));
}

PD_TEST(engine_reuses_persisted_findings_instead_of_reprobing_every_boot) {
  pdfake::TempDir dir("engine-findings");
  pdfake::MockLink link;
  link.device->setScript(probeableDevice());

  const auto configure = [&link](ProbePolicy policy) {
    PrinterConfig config;
    config.id = "tcp://printer-under-test:9100";
    config.transport = link.factory();
    config.profile = devices::generic_80();
    config.profile.completion_timeout_ms = 400;
    config.profile.preflight_timeout_ms = 200;
    config.profile.chunk_bytes = 0;
    config.profile.inter_chunk_delay_ms = 0;
    config.probe = policy;
    config.probe_options = fastProbe();
    return config;
  };

  {
    PrinterDriver driver(StorageConfig::at(dir.path()));
    auto printer = driver.addPrinter(configure(ProbePolicy::IfUnknown));
    printer->drain();
    CHECK_EQ(printer->profile().completion, CompletionMechanism::GsParenH);
    CHECK_EQ(driver.capabilities().size(), static_cast<size_t>(1));
  }
  const size_t identity_queries = link.device->identityRequests().size();
  CHECK_EQ(identity_queries, static_cast<size_t>(7));

  {
    // Same printer, next boot: the stored findings promote the profile and the
    // device is never interrogated again.
    PrinterDriver driver(StorageConfig::at(dir.path()));
    auto printer = driver.addPrinter(configure(ProbePolicy::IfUnknown));
    printer->drain();
    CHECK_EQ(printer->profile().completion, CompletionMechanism::GsParenH);
    CHECK(printer->profile().probed);
    CHECK(printer->findings().has_value());
    CHECK_EQ(link.device->identityRequests().size(), identity_queries);
  }

  {
    // ProbePolicy::Never ignores the cache as well: the profile is the whole truth.
    PrinterDriver driver(StorageConfig::at(dir.path()));
    auto printer = driver.addPrinter(configure(ProbePolicy::Never));
    printer->drain();
    CHECK_EQ(printer->profile().completion, CompletionMechanism::GsR1);
    CHECK(!printer->profile().probed);
    CHECK_EQ(link.device->identityRequests().size(), identity_queries);
  }
}

// --- The one real-socket path ---------------------------------------------------------

PD_TEST(transport_real_socket_drives_the_full_engine) {
  auto device = std::make_shared<pdfake::FakePrinter>();
  pdfake::FakePrinterServer server(device);
  CHECK(server.start());

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "loopback";
  config.transport = tcp("127.0.0.1", server.port(), 2000);
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  // Real sockets, real threads: the fences need a budget a loaded machine can meet.
  config.profile.completion_timeout_ms = 5000;
  config.profile.preflight_timeout_ms = 2000;
  auto printer = driver.addPrinter(config);

  auto job = printer->print(Payload::raw(textPayload("OVER A REAL SOCKET")), {});
  const JobResult result = job->result();
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK(device->receivedContains("OVER A REAL SOCKET"));
  CHECK_EQ(device->cuts(), static_cast<size_t>(1));
  CHECK_EQ(device->markers().size(), static_cast<size_t>(2));
  driver.shutdown();
  server.stop();
}

PD_TEST(transport_connect_to_a_closed_port_fails_known) {
  uint16_t port = 0;
  {
    auto device = std::make_shared<pdfake::FakePrinter>();
    pdfake::FakePrinterServer server(device);
    CHECK(server.start());
    port = server.port();
    server.stop();
  }

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "closed-port";
  config.transport = tcp("127.0.0.1", port, 1000);
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  auto printer = driver.addPrinter(config);

  const JobResult result =
      printer->print(Payload::raw(textPayload("NOBODY HOME")), {})->result();
  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::TransportUnreachable);
}

PD_TEST(transport_tcp_factory_describes_its_endpoint) {
  auto factory = tcp("192.0.2.10", 9100, 500);
  auto transport = factory();
  CHECK(transport != nullptr);
  CHECK_EQ(transport->describe(), std::string("tcp://192.0.2.10:9100"));
  CHECK(!transport->isConnected());
  CHECK_EQ(std::string(to_string(TransportError::ConnectTimeout)),
           std::string("ConnectTimeout"));
  const TransportResult result = transport->write(nullptr, 0);
  CHECK(!result.ok);
  CHECK_EQ(result.error, TransportError::NotConnected);
}
