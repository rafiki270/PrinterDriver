#include "printerdriver/print_queue.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_printer.hpp"
#include "test_harness.hpp"

using namespace pd;
using namespace std::chrono_literals;

namespace {

escpos::Bytes textPayload(const std::string& text) {
  escpos::Encoder encoder;
  encoder.line(text);
  return encoder.take();
}

struct Rig {
  // Declaration order is the teardown contract: the queue stops before the driver it
  // subscribed to, and the driver stops before the device it was talking to.
  pdfake::MockLink link;
  CapabilityProfile profile;
  std::unique_ptr<PrinterDriver> driver;
  std::shared_ptr<Printer> printer;
  std::unique_ptr<PrintQueue> queue;

  explicit Rig(CompletionMechanism mechanism = CompletionMechanism::GsParenH) {
    profile = pdfake::fastProfile(mechanism);
    driver.reset(new PrinterDriver(StorageConfig::inMemory()));
  }

  ~Rig() {
    queue.reset();
    driver.reset();
  }

  // Deferred so a test can adjust the profile or the device script first.
  void build(QueuePolicy policy) {
    PrinterConfig config;
    config.id = "queue-printer";
    config.transport = link.factory();
    config.width_dots = escpos::kWidth80mm;
    config.profile = profile;
    printer = driver->addPrinter(config);
    queue.reset(new PrintQueue(*driver, policy));
  }

  const std::string& id() const { return printer->id(); }

  // A status round trip is the only way a device state reaches the core, and therefore
  // the queue: the fake answers DLE EOT 1 with the offline bit set, the parser lowers
  // that to DeviceEvent::Offline, and the queue's lane closes.
  void goOffline() {
    pdfake::Script script;
    script.printer_status = pdfake::withBit(0x16, 3);
    link.device->setScript(script);
    printer->refreshStatus(300ms);
  }

  void goOnline(pdfake::Script script = {}) {
    link.device->setScript(script);
    printer->refreshStatus(300ms);
  }

  std::shared_ptr<PrintJob> enqueue(const std::string& text,
                                    const QueueOptions& options = {}) {
    return queue->enqueue(printer, Payload::raw(textPayload(text)), options);
  }
};

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds budget = 3000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

std::vector<JobState> states(const std::shared_ptr<PrintJob>& job) {
  std::vector<JobState> out;
  for (const JobEvent& event : job->history()) {
    out.push_back(event.state);
  }
  return out;
}

bool contains(const std::vector<JobState>& states, JobState wanted) {
  for (const JobState state : states) {
    if (state == wanted) {
      return true;
    }
  }
  return false;
}

size_t indexOf(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle);
}

}  // namespace

// --- a. Store and forward ----------------------------------------------------------

PD_TEST(queue_holds_an_offline_printer_then_drains_on_the_online_event) {
  Rig rig;
  QueuePolicy policy;
  policy.hold_while_offline = true;
  rig.build(policy);

  rig.goOffline();
  auto job = rig.enqueue("KITCHEN 7F3A", QueueOptions{"order-7F3A#kitchen"});

  CHECK_EQ(job->state(), JobState::HeldOffline);
  CHECK_EQ(rig.queue->depth(rig.id()), size_t{1});
  CHECK_EQ(rig.link.device->printDataBytes(), size_t{0});

  rig.goOnline();

  const JobResult result = job->result();
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(rig.queue->depth(rig.id()), size_t{0});
  CHECK_EQ(rig.queue->drainedCount(), size_t{1});
  CHECK(rig.link.device->receivedContains("KITCHEN 7F3A"));
  CHECK_EQ(rig.link.device->cuts(), size_t{1});
}

PD_TEST(queue_sends_straight_through_when_the_printer_is_usable) {
  Rig rig;
  rig.build(QueuePolicy{});

  auto job = rig.enqueue("STRAIGHT THROUGH");
  const JobResult result = job->result();

  CHECK_EQ(result.outcome, JobOutcome::Done);
  // Nothing was ever parked, so nothing announced itself as parked either.
  CHECK(!contains(states(job), JobState::HeldOffline));
  CHECK_EQ(rig.queue->depth(), size_t{0});
}

PD_TEST(queue_held_offline_appears_in_the_event_stream_before_the_send_states) {
  Rig rig;
  rig.build(QueuePolicy{});
  rig.goOffline();

  auto job = rig.enqueue("EVENT ORDER");
  std::vector<JobState> seen;
  std::mutex seen_mutex;
  job->subscribe([&](const JobEvent& event) {
    std::lock_guard<std::mutex> lock(seen_mutex);
    seen.push_back(event.state);
  });

  rig.goOnline();
  CHECK_EQ(job->result().outcome, JobOutcome::Done);
  rig.printer->drain();

  std::lock_guard<std::mutex> lock(seen_mutex);
  CHECK(seen.size() >= 3);
  CHECK_EQ(seen.front(), JobState::Queued);
  CHECK_EQ(seen[1], JobState::HeldOffline);
  CHECK_EQ(seen.back(), JobState::DoneSoftware);
  // HeldOffline is a pre-send state, so it appears exactly once and never again after
  // the job has started moving bytes.
  size_t held = 0;
  for (size_t i = 0; i < seen.size(); ++i) {
    if (seen[i] == JobState::HeldOffline) {
      ++held;
      CHECK_EQ(i, size_t{1});
    }
  }
  CHECK_EQ(held, size_t{1});
}

// --- b. TTL ------------------------------------------------------------------------

PD_TEST(queue_ttl_expiry_while_held_fails_known_without_sending_a_byte) {
  Rig rig;
  QueuePolicy policy;
  policy.default_ttl_ms = 60;
  rig.build(policy);
  rig.goOffline();

  auto job = rig.enqueue("STALE KITCHEN TICKET");
  CHECK_EQ(job->state(), JobState::HeldOffline);

  const JobResult result = job->result();
  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::Expired);
  CHECK_EQ(job->state(), JobState::FailedKnown);
  // The whole point: a ticket that expired must never reach a recovered kitchen.
  CHECK_EQ(rig.link.device->printDataBytes(), size_t{0});
  CHECK_EQ(rig.link.device->cuts(), size_t{0});
  CHECK_EQ(rig.queue->expiredCount(), size_t{1});
  CHECK_EQ(rig.queue->drainedCount(), size_t{0});
}

PD_TEST(queue_per_job_ttl_overrides_the_policy_default) {
  Rig rig;
  QueuePolicy policy;
  policy.default_ttl_ms = 0;  // policy says "keep forever"
  rig.build(policy);
  rig.goOffline();

  QueueOptions options;
  options.key = "closing-summary";
  options.ttl_ms = 50;
  auto job = rig.enqueue("PER JOB TTL", options);

  const JobResult result = job->result();
  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::Expired);
}

// --- c. Depth ----------------------------------------------------------------------

PD_TEST(queue_depth_limit_refuses_loudly_with_queue_overflow) {
  Rig rig;
  QueuePolicy policy;
  policy.max_depth = 2;
  rig.build(policy);
  rig.goOffline();

  auto first = rig.enqueue("TICKET 1", QueueOptions{"t1"});
  auto second = rig.enqueue("TICKET 2", QueueOptions{"t2"});
  auto third = rig.enqueue("TICKET 3", QueueOptions{"t3"});

  CHECK_EQ(first->state(), JobState::HeldOffline);
  CHECK_EQ(second->state(), JobState::HeldOffline);
  // Immediately terminal, not eventually: backpressure the caller can act on.
  CHECK(third->isTerminal());
  CHECK_EQ(third->state(), JobState::FailedKnown);
  CHECK_EQ(third->result().outcome, JobOutcome::Failed);
  CHECK_EQ(third->result().reason, FailureReason::QueueOverflow);
  CHECK_EQ(rig.queue->depth(rig.id()), size_t{2});
  CHECK_EQ(rig.queue->overflowCount(), size_t{1});
}

// --- d. Priority -------------------------------------------------------------------

PD_TEST(queue_priority_orders_the_waiting_set_only) {
  Rig rig;
  QueuePolicy policy;
  policy.drain_order = DrainOrder::Priority;
  policy.max_depth = 0;
  rig.build(policy);
  rig.goOffline();

  QueueOptions low;
  low.key = "low";
  low.priority = 0;
  QueueOptions high;
  high.key = "high";
  high.priority = 5;
  QueueOptions middle;
  middle.key = "middle";
  middle.priority = 1;

  auto job_low = rig.enqueue("LOWLOW", low);
  auto job_high = rig.enqueue("HIGHHIGH", high);
  auto job_middle = rig.enqueue("MIDMID", middle);
  CHECK_EQ(rig.queue->depth(rig.id()), size_t{3});

  rig.goOnline();
  CHECK_EQ(job_low->result().outcome, JobOutcome::Done);
  CHECK_EQ(job_high->result().outcome, JobOutcome::Done);
  CHECK_EQ(job_middle->result().outcome, JobOutcome::Done);

  const std::string printed = rig.link.device->printText();
  const size_t at_high = indexOf(printed, "HIGHHIGH");
  const size_t at_middle = indexOf(printed, "MIDMID");
  const size_t at_low = indexOf(printed, "LOWLOW");
  CHECK(at_high != std::string::npos);
  CHECK(at_middle != std::string::npos);
  CHECK(at_low != std::string::npos);
  CHECK(at_high < at_middle);
  CHECK(at_middle < at_low);
}

PD_TEST(queue_fifo_policy_ignores_priority) {
  Rig rig;
  QueuePolicy policy;
  policy.drain_order = DrainOrder::Fifo;
  rig.build(policy);
  rig.goOffline();

  QueueOptions first_options;
  first_options.key = "first";
  first_options.priority = 0;
  QueueOptions second_options;
  second_options.key = "second";
  second_options.priority = 9;

  auto first = rig.enqueue("FIRSTONE", first_options);
  auto second = rig.enqueue("SECONDONE", second_options);

  rig.goOnline();
  CHECK_EQ(first->result().outcome, JobOutcome::Done);
  CHECK_EQ(second->result().outcome, JobOutcome::Done);

  const std::string printed = rig.link.device->printText();
  CHECK(indexOf(printed, "FIRSTONE") < indexOf(printed, "SECONDONE"));
}

// --- e. Unknown blocks the lane ----------------------------------------------------

PD_TEST(queue_unknown_job_blocks_the_lane_until_unblock) {
  Rig rig;
  QueuePolicy policy;
  policy.default_ttl_ms = 0;  // nothing may expire out from under this test
  rig.build(policy);

  // The printer accepts the bytes and then says nothing, which is the definition of
  // Unknown: it may well have printed.
  pdfake::Script silent;
  silent.answer_process_id = false;
  rig.link.device->setScript(silent);

  auto first = rig.enqueue("MAYBE PRINTED", QueueOptions{"maybe"});
  auto second = rig.enqueue("MUST NOT FOLLOW", QueueOptions{"after"});

  CHECK_EQ(first->result().outcome, JobOutcome::Unknown);
  CHECK(waitFor([&] { return rig.queue->blocked(rig.id()); }));

  // The device is healthy again, but that is not evidence about the first receipt.
  rig.link.device->setScript(pdfake::Script{});
  std::this_thread::sleep_for(150ms);
  CHECK(!second->isTerminal());
  CHECK_EQ(second->state(), JobState::HeldOffline);
  CHECK_EQ(rig.queue->depth(rig.id()), size_t{1});
  CHECK(!rig.link.device->receivedContains("MUST NOT FOLLOW"));

  rig.queue->unblock(rig.id());
  CHECK_EQ(second->result().outcome, JobOutcome::Done);
  CHECK(rig.link.device->receivedContains("MUST NOT FOLLOW"));
  CHECK(!rig.queue->blocked(rig.id()));
}

PD_TEST(queue_pause_holds_a_usable_lane_and_resume_releases_it) {
  Rig rig;
  rig.build(QueuePolicy{});
  rig.queue->pause(rig.id());

  auto job = rig.enqueue("PAUSED", QueueOptions{"paused"});
  CHECK(rig.queue->paused(rig.id()));
  CHECK_EQ(job->state(), JobState::HeldOffline);
  std::this_thread::sleep_for(50ms);
  CHECK_EQ(rig.link.device->printDataBytes(), size_t{0});

  rig.queue->resume(rig.id());
  CHECK_EQ(job->result().outcome, JobOutcome::Done);
}

// --- f. Idempotency ----------------------------------------------------------------

PD_TEST(queue_re_enqueueing_an_existing_key_returns_the_existing_job) {
  Rig rig;
  rig.build(QueuePolicy{});
  rig.goOffline();

  QueueOptions options;
  options.key = "order-7F3A#kitchen";
  auto first = rig.enqueue("ONE COPY ONLY", options);
  auto second = rig.enqueue("ONE COPY ONLY", options);

  CHECK(first == second);
  CHECK_EQ(rig.queue->depth(rig.id()), size_t{1});

  rig.goOnline();
  CHECK_EQ(first->result().outcome, JobOutcome::Done);
  rig.printer->drain();
  CHECK_EQ(rig.link.device->cuts(), size_t{1});
}

PD_TEST(queue_key_dedupes_against_a_direct_print_while_still_held) {
  Rig rig;
  rig.build(QueuePolicy{});
  rig.goOffline();

  QueueOptions options;
  options.key = "shared-key";
  auto queued = rig.enqueue("HELD COPY", options);
  CHECK_EQ(queued->state(), JobState::HeldOffline);

  // The key is claimed in the driver's own index, so the core dedupes against a job it
  // has not been handed yet.
  JobOptions direct;
  direct.key = "shared-key";
  auto bypass = rig.printer->print(Payload::raw(textPayload("SECOND COPY")), direct);
  CHECK(bypass == queued);
  CHECK(rig.driver->findJob("shared-key") == queued);

  rig.goOnline();
  CHECK_EQ(queued->result().outcome, JobOutcome::Done);
  rig.printer->drain();
  CHECK(rig.link.device->receivedContains("HELD COPY"));
  CHECK(!rig.link.device->receivedContains("SECOND COPY"));
  CHECK_EQ(rig.link.device->cuts(), size_t{1});
}

// --- g. Policy off ------------------------------------------------------------------

PD_TEST(queue_with_holding_disabled_submits_to_an_offline_printer) {
  Rig rig;
  QueuePolicy policy;
  policy.hold_while_offline = false;
  rig.build(policy);
  rig.goOffline();

  auto job = rig.enqueue("NO HOLDING", QueueOptions{"nohold"});
  const JobResult result = job->result();

  // Straight into the engine, which refuses it at preflight rather than parking it.
  CHECK_EQ(result.outcome, JobOutcome::Failed);
  CHECK_EQ(result.reason, FailureReason::PreflightHardwareError);
  CHECK(!contains(states(job), JobState::HeldOffline));
}
