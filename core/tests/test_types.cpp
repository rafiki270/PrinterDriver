#include <set>
#include <string>

#include "printerdriver/types.hpp"
#include "test_harness.hpp"

using namespace pd;

namespace {

template <typename Array>
void checkNamesUniqueAndNonEmpty(const Array& values) {
  std::set<std::string> seen;
  for (const auto value : values) {
    const std::string name = to_string(value);
    CHECK(!name.empty());
    CHECK(seen.insert(name).second);
  }
  CHECK_EQ(seen.size(), values.size());
}

}  // namespace

PD_TEST(job_state_names_are_unique_and_complete) {
  checkNamesUniqueAndNonEmpty(kAllJobStates);
  CHECK_EQ(std::string(to_string(JobState::Queued)), std::string("Queued"));
  CHECK_EQ(std::string(to_string(JobState::CutCommandProcessed)),
           std::string("CutCommandProcessed"));
}

PD_TEST(confidence_level_names_are_unique_and_complete) {
  checkNamesUniqueAndNonEmpty(kAllConfidenceLevels);
}

PD_TEST(device_event_names_are_unique_and_complete) {
  checkNamesUniqueAndNonEmpty(kAllDeviceEvents);
}

PD_TEST(failure_reason_names_are_unique_and_complete) {
  checkNamesUniqueAndNonEmpty(kAllFailureReasons);
}

PD_TEST(job_outcome_names_are_unique_and_complete) {
  checkNamesUniqueAndNonEmpty(kAllJobOutcomes);
}

PD_TEST(queue_addon_reservations_are_present) {
  // docs/sdk-spec.md §12: the core never produces these, but every wrapper must
  // ship them so the addon needs no enum change later.
  CHECK_EQ(std::string(to_string(JobState::HeldOffline)), std::string("HeldOffline"));
  CHECK_EQ(std::string(to_string(FailureReason::Expired)), std::string("Expired"));
  CHECK_EQ(std::string(to_string(FailureReason::QueueOverflow)),
           std::string("QueueOverflow"));
}

PD_TEST(job_result_is_tri_state) {
  const JobResult done = JobResult::done(ConfidenceLevel::CutFaultFree);
  CHECK_EQ(done.outcome, JobOutcome::Done);
  CHECK_EQ(done.confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(done.reason, FailureReason::None);

  const JobResult failed = JobResult::failed(FailureReason::PreflightPaperOut);
  CHECK_EQ(failed.outcome, JobOutcome::Failed);
  CHECK_EQ(failed.reason, FailureReason::PreflightPaperOut);

  // A completion-wait timeout is Unknown, not Failed (docs/api.md §4).
  const JobResult unknown = JobResult::unknown(ConfidenceLevel::PrinterHealthy);
  CHECK_EQ(unknown.outcome, JobOutcome::Unknown);
  CHECK_EQ(unknown.confidence, ConfidenceLevel::PrinterHealthy);
  CHECK_EQ(unknown.reason, FailureReason::Unknown);
}

PD_TEST(job_event_carries_optional_reason_and_monotonic_time) {
  const JobEvent sending =
      JobEvent::make(JobState::SendStarted, ConfidenceLevel::TransportAccepted);
  CHECK(!sending.reason.has_value());

  const JobEvent failure =
      JobEvent::make(JobState::FailedKnown, ConfidenceLevel::TransportAccepted,
                     FailureReason::TransportUnreachable);
  CHECK(failure.reason.has_value());
  CHECK_EQ(*failure.reason, FailureReason::TransportUnreachable);

  const MonotonicTime earlier = MonotonicClock::now();
  const MonotonicTime later = MonotonicClock::now();
  CHECK(later >= earlier);
  CHECK(MonotonicClock::is_steady);
}
