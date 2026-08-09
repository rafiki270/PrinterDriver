#include "printerdriver/driver.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
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
  // [2-char instance nonce][2-char sequence] (docs/api.md §14): both markers of one
  // receipt carry this driver's nonce, and the print fence takes the even sequence
  // with the cut fence on its odd successor, which is what keeps P and C apart inside
  // a fixed four-character layout.
  const std::string print_marker = rig.link.device->markers()[0].token;
  const std::string cut_marker = rig.link.device->markers()[1].token;
  CHECK_EQ(print_marker.size(), static_cast<size_t>(4));
  CHECK_EQ(print_marker.substr(0, 2), rig.driver->instanceNonce());
  CHECK_EQ(cut_marker.substr(0, 2), rig.driver->instanceNonce());
  CHECK(print_marker != cut_marker);
  CHECK_EQ(print_marker.substr(0, 3), cut_marker.substr(0, 3));
  CHECK_EQ(cut_marker[3], static_cast<char>(print_marker[3] + 1));
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

PD_TEST(engine_subscribe_during_live_emission_never_reorders_events) {
  // Regression: subscribe() used to register the callback under the job mutex but
  // replay the recorded history only after releasing it, so a worker emitting in
  // that window handed the new subscriber a live event ahead of older recorded ones
  // — observed from the Swift wrapper as bytesSent arriving before queued/
  // preflightOk/sendStarted about one run in three. The contract (pd.h, "Callback
  // threads") is replay-then-stream: every subscriber sees exactly the job's
  // recorded history, in order, no matter when it subscribes.
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();

  // Busy-wait rather than sleep: at microsecond scale the scheduler's wake-up jitter
  // would swamp the emission window this test is trying to land subscriptions in.
  const auto spin = [](std::chrono::microseconds duration) {
    const auto until = MonotonicClock::now() + duration;
    while (MonotonicClock::now() < until) {
    }
  };

  for (int round = 0; round < 80; ++round) {
    auto job = rig.printer->print(
        Payload::raw(textPayload("RACE-" + std::to_string(round))), {});

    struct Observer {
      std::mutex mutex;
      std::vector<JobEvent> seen;
    } observers[4];
    for (Observer& observer : observers) {
      job->subscribe([&observer, &spin](const JobEvent& event) {
        std::lock_guard<std::mutex> lock(observer.mutex);
        observer.seen.push_back(event);
        // Stretches the replay loop so a concurrent emission has a real window to
        // land in; the assertions below are what make landing there harmless.
        spin(std::chrono::microseconds(5));
      });
      // Stagger the next subscription into a different slice of the worker's
      // emission burst.
      spin(std::chrono::microseconds(25 + (round % 16) * 25));
    }
    job->result();

    // The terminal event's callbacks complete before finish() publishes the result,
    // and each subscribe() returned before result() was called, so by now every
    // observer has been handed everything it will ever be handed.
    const std::vector<JobEvent> canonical = job->history();
    CHECK(job->isTerminal());
    CHECK(canonical.size() >= static_cast<size_t>(2));
    for (Observer& observer : observers) {
      std::lock_guard<std::mutex> lock(observer.mutex);
      CHECK_EQ(observer.seen.size(), canonical.size());
      for (size_t i = 0; i < observer.seen.size() && i < canonical.size(); ++i) {
        CHECK_EQ(observer.seen[i].state, canonical[i].state);
        // Same event, not merely the same state: timestamps are unique per emit.
        CHECK(observer.seen[i].at == canonical[i].at);
        if (i > 0) {
          CHECK(!(observer.seen[i].at < observer.seen[i - 1].at));
        }
      }
    }
  }
}

PD_TEST(engine_subscriber_mid_replay_receives_recorded_before_live) {
  // The same race, reconstructed deterministically instead of hoped into: the worker
  // is pinned on the payload write with SendStarted already recorded, subscribe()
  // starts replaying, and the first replay callback un-pins the worker — so BytesSent
  // and everything after it is emitted while the replay is provably still running.
  // The subscriber must still see recorded events strictly before live ones.
  struct Gate {
    std::mutex mutex;
    std::condition_variable released_cv;
    bool armed = false;
    bool released = false;
  };
  auto gate = std::make_shared<Gate>();

  Rig rig(CompletionMechanism::GsParenH);
  // Installed before build(): the factory copies behaviour when the printer is added.
  rig.link.behaviour.before_write = [gate](const uint8_t* data, size_t size) {
    const std::string chunk(reinterpret_cast<const char*>(data), size);
    if (chunk.find("PINNED-") == std::string::npos) {
      return;  // preflight probes, fences and cuts pass straight through
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    if (!gate->armed) {
      return;
    }
    gate->armed = false;
    // The 2 s cap is a hang breaker, not a timing assumption: release always comes
    // from the subscriber's callback below.
    gate->released_cv.wait_for(lock, std::chrono::seconds(2),
                               [&] { return gate->released; });
  };
  rig.build();

  for (int round = 0; round < 15; ++round) {
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->armed = true;
      gate->released = false;
    }
    auto job = rig.printer->print(
        Payload::raw(textPayload("PINNED-" + std::to_string(round))), {});

    // The worker records SendStarted, then blocks in before_write on the payload.
    const auto deadline = MonotonicClock::now() + std::chrono::seconds(2);
    while (job->state() != JobState::SendStarted && MonotonicClock::now() < deadline) {
    }
    CHECK_EQ(job->state(), JobState::SendStarted);

    std::mutex seen_mutex;
    std::vector<JobEvent> seen;
    job->subscribe([&](const JobEvent& event) {
      {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(event);
      }
      {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->released) {
          gate->released = true;
          gate->released_cv.notify_all();
        }
      }
      // Dawdle so the rest of the replay is still running when the un-pinned worker
      // emits: the mock's writes and answers take microseconds, this takes 700 each.
      const auto until = MonotonicClock::now() + std::chrono::microseconds(700);
      while (MonotonicClock::now() < until) {
      }
    });
    job->result();

    const std::vector<JobEvent> canonical = job->history();
    std::lock_guard<std::mutex> lock(seen_mutex);
    // Queued, PreflightOk, SendStarted recorded before the pin; at least BytesSent
    // and a terminal after release.
    CHECK(canonical.size() >= static_cast<size_t>(5));
    CHECK_EQ(seen.size(), canonical.size());
    for (size_t i = 0; i < seen.size() && i < canonical.size(); ++i) {
      CHECK_EQ(seen[i].state, canonical[i].state);
      CHECK(seen[i].at == canonical[i].at);
    }
  }
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

// --- Verification identifiers (docs/api.md §14) ----------------------------------------

PD_TEST(engine_prints_the_verification_id_next_to_the_order_line_and_in_the_qr) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-7F3A-92C1";
  const JobResult result = runOne(rig, "VERIFY ME", options);
  CHECK_EQ(result.outcome, JobOutcome::Done);

  auto job = rig.driver->findJob(options.key);
  CHECK(job != nullptr);
  const std::string token = job->printToken();
  CHECK_EQ(token, rig.link.device->markers()[0].token);

  const std::string trailer = std::string(kOrderPrefix) + options.key + "  " +
                              kVerificationPrefix + token;
  // Printed once as text and once inside the QR, so the eye and the scanner read the
  // same string. The QR payload rides in a GS ( k block, which the device's scanner
  // skips wholesale rather than counting as print data.
  CHECK(rig.link.device->printText().find(trailer) != std::string::npos);
  const std::vector<uint8_t> raw(trailer.begin(), trailer.end());
  const size_t first = findSubsequence(rig.link.device->received(), raw);
  CHECK(first != std::string::npos);
  CHECK(findSubsequence(rig.link.device->received(), raw, first + 1) != std::string::npos);
}

PD_TEST(engine_resolves_a_job_from_either_of_its_printed_tokens) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-resolvable";
  CHECK_EQ(runOne(rig, "PAPER TRAIL", options).outcome, JobOutcome::Done);

  auto job = rig.driver->findJob(options.key);
  CHECK(job != nullptr);
  CHECK(rig.driver->jobByToken(job->printToken()).get() == job.get());
  CHECK(rig.driver->jobByToken(job->cutToken()).get() == job.get());
  CHECK(rig.driver->jobByToken("!!!!") == nullptr);
  CHECK(rig.driver->jobByToken("") == nullptr);
}

PD_TEST(engine_reprint_takes_a_fresh_token_and_the_newest_holder_wins) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-two-attempts";
  auto first = rig.printer->print(Payload::raw(textPayload("ORIGINAL")), options);
  CHECK_EQ(first->result().outcome, JobOutcome::Done);
  const std::string first_token = first->printToken();

  auto second = rig.driver->forceReprint(options.key);
  CHECK(second != nullptr);
  CHECK_EQ(second->result().outcome, JobOutcome::Done);
  // A second physical print is a second piece of evidence, so it gets its own
  // identifier rather than inheriting the one already on paper.
  CHECK(second->printToken() != first_token);
  CHECK(rig.driver->jobByToken(first_token).get() == first.get());
  CHECK(rig.driver->jobByToken(second->printToken()).get() == second.get());
}

PD_TEST(engine_verification_id_can_be_suppressed_without_losing_the_token) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-quiet";
  options.print_verification_id = false;
  CHECK_EQ(runOne(rig, "NO CODE ON THIS ONE", options).outcome, JobOutcome::Done);

  auto job = rig.driver->findJob(options.key);
  CHECK(job != nullptr);
  CHECK(!job->printToken().empty());
  // Nothing printed, everything still journaled and resolvable: the toggle is about
  // ink, not about evidence.
  CHECK(!rig.link.device->receivedContains(std::string(kVerificationPrefix) +
                                           job->printToken()));
  CHECK(!rig.link.device->receivedContains(std::string(kOrderPrefix) + options.key));
  CHECK(rig.driver->jobByToken(job->printToken()).get() == job.get());
}

PD_TEST(engine_without_a_gsh_fence_there_is_no_wire_token_to_print) {
  Rig rig(CompletionMechanism::GsR1);
  rig.build();
  JobOptions options;
  options.key = "order-fallback";
  CHECK_EQ(runOne(rig, "QUEUED FENCE ONLY", options).outcome, JobOutcome::Done);

  auto job = rig.driver->findJob(options.key);
  CHECK(job != nullptr);
  CHECK(job->printToken().empty());
  CHECK(!rig.link.device->receivedContains(kVerificationPrefix));
}

PD_TEST(engine_journals_both_tokens_and_resolves_them_after_a_restart) {
  pdfake::TempDir dir("engine-tokens");
  std::string print_token;
  std::string cut_token;
  std::string nonce;
  {
    Rig rig(CompletionMechanism::GsParenH, StorageConfig::at(dir.path()));
    rig.build();
    JobOptions options;
    options.key = "order-persisted-token";
    CHECK_EQ(runOne(rig, "YESTERDAY", options).outcome, JobOutcome::Done);
    auto job = rig.driver->findJob(options.key);
    CHECK(job != nullptr);
    print_token = job->printToken();
    cut_token = job->cutToken();
    nonce = rig.driver->instanceNonce();

    bool journaled = false;
    for (const JournalEntry& entry : readJournal(dir.path() + "/jobs.journal")) {
      if (entry.kind == JournalEntry::Kind::Tokens &&
          entry.record.print_token == print_token &&
          entry.record.cut_token == cut_token) {
        journaled = true;
      }
    }
    CHECK(journaled);
  }

  // A new process on the same store: the nonce is the same instance identity, the
  // compacted J line carries both tokens, and the paper still resolves.
  PrinterDriver driver(StorageConfig::at(dir.path()));
  CHECK_EQ(driver.instanceNonce(), nonce);
  auto reloaded = driver.jobByToken(print_token);
  CHECK(reloaded != nullptr);
  if (reloaded != nullptr) {
    CHECK_EQ(reloaded->key(), std::string("order-persisted-token"));
    CHECK_EQ(reloaded->printToken(), print_token);
    CHECK_EQ(reloaded->cutToken(), cut_token);
  }
  CHECK(driver.jobByToken(cut_token) != nullptr);
}

PD_TEST(engine_reports_a_foreign_echo_without_consuming_a_fence) {
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  // Another instance's nonce, so the token is structurally perfect and provably not
  // ours (docs/sdk-spec.md §14: direct multi-instance writing is unsupported, and the
  // instance nonce exists to make a violation loud).
  script.foreign_process_id = "~~~~";
  rig.link.device->setScript(script);
  rig.build();

  std::vector<DeviceEvent> events;
  rig.printer->subscribe([&events](DeviceEvent event) { events.push_back(event); });

  const JobResult result = runOne(rig, "OURS");
  // The foreign echo satisfied nothing: our own job still completed on its own fence.
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK(std::find(events.begin(), events.end(), DeviceEvent::ForeignWriterDetected) !=
        events.end());
  CHECK_EQ(std::string(to_string(DeviceEvent::ForeignWriterDetected)),
           std::string("ForeignWriterDetected"));
}

PD_TEST(engine_does_not_cry_foreign_over_our_own_late_echo) {
  // A marker answered after the job it belonged to has already timed out is late, not
  // foreign: it carries this instance's nonce, and reporting a multi-writer violation
  // there would cry wolf on the one case Unknown exists for.
  Rig rig(CompletionMechanism::GsParenH);
  pdfake::Script script;
  script.answer_process_id = false;
  rig.link.device->setScript(script);
  rig.build();

  std::vector<DeviceEvent> events;
  rig.printer->subscribe([&events](DeviceEvent event) { events.push_back(event); });

  auto first = rig.printer->print(Payload::raw(textPayload("TIMES OUT")), {});
  CHECK_EQ(first->result().outcome, JobOutcome::Unknown);

  // Replay the token the printer never acknowledged, long after the job gave up.
  pdfake::Script replay;
  replay.foreign_process_id = first->printToken();
  rig.link.device->setScript(replay);
  CHECK_EQ(runOne(rig, "NEXT TICKET").outcome, JobOutcome::Done);

  CHECK(std::find(events.begin(), events.end(), DeviceEvent::ForeignWriterDetected) ==
        events.end());
}

// --- Reprint banner and margins ---------------------------------------------------------

PD_TEST(engine_reprint_banner_can_be_turned_off_per_call) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.key = "order-customer-copy";
  CHECK_EQ(runOne(rig, "ORIGINAL", options).outcome, JobOutcome::Done);

  ReprintOptions quiet;
  quiet.banner = false;
  auto copy = rig.printer->forceReprint(options.key, quiet);
  CHECK(copy != nullptr);
  CHECK_EQ(copy->result().outcome, JobOutcome::Done);
  // Still a deliberate duplicate — the attempt counter is the record of that — but
  // without the banner an operator asked not to print.
  CHECK_EQ(copy->attempt(), 2u);
  CHECK(!rig.link.device->receivedContains(kReprintBannerLine));
  CHECK(!rig.link.device->receivedContains(std::string(kReprintAttemptPrefix) + "2"));
  CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(2));

  // The default is unchanged and the constants are untouched.
  auto loud = rig.printer->forceReprint(options.key);
  CHECK_EQ(loud->result().outcome, JobOutcome::Done);
  CHECK(rig.link.device->receivedContains(kReprintBannerLine));
  CHECK(rig.link.device->receivedContains(std::string(kReprintAttemptPrefix) + "3"));
}

PD_TEST(engine_top_margin_is_fed_before_any_content) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.top_feed_dots = 40;
  const JobResult result = runOne(rig, "MARGIN TICKET", options);
  CHECK_EQ(result.outcome, JobOutcome::Done);

  const std::vector<uint8_t> received = rig.link.device->received();
  const std::vector<uint8_t> feed_bytes{0x1B, 0x4A, 0x28};  // ESC J 40
  const std::vector<uint8_t> content{'M', 'A', 'R', 'G', 'I', 'N'};
  const size_t feed_pos = findSubsequence(received, feed_bytes);
  CHECK(feed_pos != std::string::npos);
  CHECK(feed_pos < findSubsequence(received, content));
}

PD_TEST(engine_top_margin_is_chunked_above_255_dots) {
  Rig rig(CompletionMechanism::GsParenH);
  rig.build();
  JobOptions options;
  options.top_feed_dots = 300;
  CHECK_EQ(runOne(rig, "WIDE TOP", options).outcome, JobOutcome::Done);
  // Same ESC J chunking the pre-cut clearance already uses: 255 + 45.
  const std::vector<uint8_t> feed_bytes{0x1B, 0x4A, 0xFF, 0x1B, 0x4A, 0x2D};
  CHECK(findSubsequence(rig.link.device->received(), feed_bytes) != std::string::npos);
}

PD_TEST(engine_bottom_margin_widens_the_blade_clearance_but_never_narrows_it) {
  {
    Rig rig(CompletionMechanism::GsParenH);
    rig.profile.media.head_to_cutter_feed_dots = 120;
    rig.build();
    JobOptions options;
    options.bottom_feed_dots = 200;
    CHECK_EQ(runOne(rig, "ROOMY BOTTOM", options).outcome, JobOutcome::Done);
    const std::vector<uint8_t> received = rig.link.device->received();
    const std::vector<uint8_t> feed_bytes{0x1B, 0x4A, 0xC8};  // ESC J 200
    const std::vector<uint8_t> cut_bytes{0x1D, 0x56, 0x01};
    const size_t feed_pos = findSubsequence(received, feed_bytes);
    CHECK(feed_pos != std::string::npos);
    CHECK_EQ(feed_pos + feed_bytes.size(), findSubsequence(received, cut_bytes));
  }
  {
    // Below the floor: the profile's blade clearance is unconditional, so asking for
    // less cannot reintroduce the clipped trailing QR it exists to prevent.
    Rig rig(CompletionMechanism::GsParenH);
    rig.profile.media.head_to_cutter_feed_dots = 120;
    rig.build();
    JobOptions options;
    options.bottom_feed_dots = 8;
    CHECK_EQ(runOne(rig, "TIGHT BOTTOM", options).outcome, JobOutcome::Done);
    const std::vector<uint8_t> received = rig.link.device->received();
    const std::vector<uint8_t> floor_bytes{0x1B, 0x4A, 0x78};  // ESC J 120
    const std::vector<uint8_t> cut_bytes{0x1D, 0x56, 0x01};
    const size_t feed_pos = findSubsequence(received, floor_bytes);
    CHECK(feed_pos != std::string::npos);
    CHECK_EQ(feed_pos + floor_bytes.size(), findSubsequence(received, cut_bytes));
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
  // A profile whose completion mechanism is an SDK call this core does not speak.
  // Printing it through a vendor's ESC/POS emulation would put paper out with no fence
  // behind it and a result nobody should believe.
  //
  // M13b changed which profile makes the point. The Star *desktops* are now driven for
  // real over a documented raw fence, so they are no longer an example of anything being
  // refused; the SDK-first portables are, because beginCheckedBlock is an API and not a
  // wire primitive, and no amount of raw socket makes it one.
  Rig rig(CompletionMechanism::GsParenH);
  rig.profile = devices::star_sm_s230();
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

// --- Custom transports: the embedder owns the link (brief §25) ----------------------

namespace {

// A stand-in for a Bluetooth stack. It exists to prove the boundary works, and it is
// shaped like the real thing rather than like a convenience: responses come back on a
// SEPARATE thread, because that is what a CoreBluetooth delegate queue, an Android
// BluetoothSocket reader or a BlueZ recv loop actually does, and because pd.h forbids
// feeding bytes from inside write(). A test double that answered inline would prove the
// core works under a threading model no real link has.
class ScriptedLink {
 public:
  explicit ScriptedLink(std::shared_ptr<pdfake::FakePrinter> device)
      : device_(std::move(device)) {
    CustomTransportLink::Callbacks callbacks;
    callbacks.description = "scripted-bt:00:11:22:33:44:55";
    callbacks.connect = [this] {
      ++connects_;
      return connect_succeeds_.load();
    };
    callbacks.write = [this](const uint8_t* data, size_t size) -> int64_t {
      ++writes_;
      if (short_write_after_ != 0 && bytes_written_ >= short_write_after_) {
        return -1;
      }
      bytes_written_ += size;
      std::vector<uint8_t> response = device_->receive(data, size);
      if (!response.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(response));
        cv_.notify_one();
      }
      return static_cast<int64_t>(size);
    };
    callbacks.close = [this] { ++closes_; };
    link_ = std::make_shared<CustomTransportLink>(std::move(callbacks));
    reader_ = std::thread([this] { readerLoop(); });
  }

  ~ScriptedLink() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    reader_.join();
  }

  TransportFactory factory() { return customTransport(link_); }
  const std::shared_ptr<CustomTransportLink>& link() const { return link_; }

  void refuseConnections() { connect_succeeds_ = false; }
  void failWritesAfter(size_t bytes) { short_write_after_ = bytes; }

  size_t connects() const { return connects_.load(); }
  size_t closes() const { return closes_.load(); }
  size_t writes() const { return writes_.load(); }
  size_t bytesWritten() const { return bytes_written_.load(); }

 private:
  void readerLoop() {
    for (;;) {
      std::vector<uint8_t> chunk;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
        if (stop_ && pending_.empty()) {
          return;
        }
        chunk = std::move(pending_.front());
        pending_.erase(pending_.begin());
      }
      link_->feedBytes(chunk.data(), chunk.size());
    }
  }

  std::shared_ptr<pdfake::FakePrinter> device_;
  std::shared_ptr<CustomTransportLink> link_;
  std::thread reader_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::vector<uint8_t>> pending_;
  bool stop_ = false;
  std::atomic<bool> connect_succeeds_{true};
  std::atomic<size_t> short_write_after_{0};
  std::atomic<size_t> connects_{0};
  std::atomic<size_t> closes_{0};
  std::atomic<size_t> writes_{0};
  std::atomic<size_t> bytes_written_{0};
};

}  // namespace

PD_TEST(custom_transport_drives_a_whole_job_and_keeps_the_same_fence) {
  // The point of the boundary: a printer reached over a link the core knows nothing
  // about still gets the full GS ( H sequence, the correlated cut fence and grade A
  // from the physical printer. If Bluetooth produced a weaker claim than TCP, the
  // wrapper would have become part of the completion story, which is exactly what
  // docs/compatibility-brief.md §25 is arranged to prevent.
  auto device = std::make_shared<pdfake::FakePrinter>();
  ScriptedLink link(device);

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "bluetooth-printer";
  config.transport = link.factory();
  config.width_dots = escpos::kWidth58mm;
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  std::shared_ptr<Printer> printer = driver.addPrinter(config);

  auto job = printer->print(Payload::raw(textPayload("BLUETOOTH TICKET")));
  const JobResult result = job->result();

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.method, std::string("GS(H) fn48"));

  // The bytes really went through the embedder's callback, and the paper really moved.
  CHECK(link.connects() >= 1);
  CHECK(link.writes() >= 1);
  CHECK(link.bytesWritten() > 0);
  CHECK_EQ(device->cuts(), static_cast<size_t>(1));
  CHECK(device->receivedContains("BLUETOOTH TICKET"));

  driver.shutdown();
  // Closing the driver closes the link: a Bluetooth channel left open holds a radio
  // connection and, on the Epson portables, the printer's single allowed pairing slot.
  CHECK(link.closes() >= 1);
}

PD_TEST(custom_transport_reports_a_refused_link_as_unreachable) {
  auto device = std::make_shared<pdfake::FakePrinter>();
  ScriptedLink link(device);
  link.refuseConnections();

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "unpaired-printer";
  config.transport = link.factory();
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  std::shared_ptr<Printer> printer = driver.addPrinter(config);

  const JobResult result = printer->print(Payload::raw(textPayload("NOWHERE")))->result();
  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::TransportUnreachable);
  CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(device->received().size(), static_cast<size_t>(0));
  driver.shutdown();
}

PD_TEST(custom_transport_feeding_with_nothing_bound_is_information_not_a_crash) {
  // Bytes can arrive from a Bluetooth peripheral before the core has connected or after
  // it has closed — a notification already in flight, a buffered chunk delivered late.
  // Dropping them is right; crashing on them is not, and neither is queueing them into
  // a parser that has no job to attribute them to.
  auto device = std::make_shared<pdfake::FakePrinter>();
  ScriptedLink link(device);
  const uint8_t stray[] = {0x12, 0x34};
  CHECK(!link.link()->bound());
  CHECK(!link.link()->feedBytes(stray, sizeof(stray)));
  CHECK(!link.link()->linkDropped("gone"));

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "late-bytes-printer";
  config.transport = link.factory();
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  std::shared_ptr<Printer> printer = driver.addPrinter(config);
  CHECK_EQ(printer->print(Payload::raw(textPayload("ONE")))->result().outcome,
           JobOutcome::Done);
  // Now a transport exists and is bound, so the same call is delivered.
  CHECK(link.link()->bound());
  CHECK(link.link()->feedBytes(stray, sizeof(stray)));

  driver.shutdown();
  CHECK(!link.link()->bound());
  CHECK(!link.link()->feedBytes(stray, sizeof(stray)));
}

PD_TEST(custom_transport_short_write_leaves_the_job_unknown_not_failed) {
  // docs/api.md §4: how far a write got is what separates a receipt that certainly did
  // not print from one that may have printed most of itself. A Bluetooth link that dies
  // mid-receipt is the case this exists for.
  auto device = std::make_shared<pdfake::FakePrinter>();
  ScriptedLink link(device);
  link.failWritesAfter(1);  // the preflight goes out, the payload does not

  PrinterDriver driver(StorageConfig::inMemory());
  PrinterConfig config;
  config.id = "dropping-printer";
  config.transport = link.factory();
  config.profile = pdfake::fastProfile(CompletionMechanism::GsParenH);
  std::shared_ptr<Printer> printer = driver.addPrinter(config);

  const JobResult result = printer->print(Payload::raw(textPayload("HALF")))->result();
  CHECK(result.outcome != JobOutcome::Done);
  CHECK(result.grade == ConfidenceGrade::E_TransportOnly ||
        result.grade == ConfidenceGrade::C_DeviceStatusAround);
  driver.shutdown();
}

PD_TEST(engine_refuses_zebra_and_brother_without_writing_a_byte) {
  // docs/compatibility-brief.md §16 and §17. ZPL, CPCL and Brother Raster are not
  // ESC/POS at any level; an ESC/POS engine pointed at one prints a metre of text
  // rather than a label. The refusal has to happen before the transport is even
  // opened, which is what the byte and connect counts below assert — a refusal that
  // still wasted a roll would be a worse bug than the one it replaced.
  for (const CapabilityProfile& profile :
       {devices::zebra_zq300_plus(), devices::zebra_zq500(), devices::zebra_zq600_plus(),
        devices::brother_rj2000(), devices::brother_rj3000(), devices::brother_rj4000()}) {
    Rig rig(CompletionMechanism::GsParenH);
    rig.profile = profile;
    rig.build();
    const JobResult result = runOne(rig, "LABEL");

    CHECK_EQ(result.outcome, JobOutcome::Failed);
    CHECK_EQ(result.reason, FailureReason::Unsupported);
    CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);
    CHECK_EQ(result.authority, CompletionAuthority::TransportOnly);
    CHECK_EQ(result.method, std::string("none"));
    // Zero bytes, zero connections, zero cuts, zero paper.
    CHECK_EQ(rig.link.device->received().size(), static_cast<size_t>(0));
    CHECK_EQ(rig.link.stats->bytes.load(), static_cast<size_t>(0));
    CHECK_EQ(rig.link.stats->connects.load(), static_cast<size_t>(0));
    CHECK_EQ(rig.link.device->cuts(), static_cast<size_t>(0));
  }
}

PD_TEST(engine_prints_generic_unknown_at_grade_e_and_claims_nothing_more) {
  // The other half of the refusal story. An unidentified ESC/POS device is not refused
  // — refusing to print because nobody has catalogued the printer would be useless in a
  // venue — but nothing about the result is inflated to compensate.
  Rig rig(CompletionMechanism::None);
  rig.profile = devices::generic_unknown();
  rig.build();
  const JobResult result = runOne(rig, "UNIDENTIFIED");

  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::TransportAccepted);
  CHECK_EQ(result.grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(result.authority, CompletionAuthority::TransportOnly);
  CHECK(rig.link.device->receivedContains("UNIDENTIFIED"));
}
