#include <set>
#include <string>

#include "printerdriver/capability_profile.hpp"
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

PD_TEST(confidence_grade_and_authority_names_are_unique_and_complete) {
  checkNamesUniqueAndNonEmpty(kAllConfidenceGrades);
  checkNamesUniqueAndNonEmpty(kAllCompletionAuthorities);
  CHECK_EQ(std::string(to_string(ConfidenceGrade::A_JobLevelConfirmation)),
           std::string("A_JobLevelConfirmation"));
  CHECK_EQ(std::string(to_string(CompletionAuthority::PhysicalPrinter)),
           std::string("PhysicalPrinter"));
  std::set<std::string> letters;
  for (const ConfidenceGrade grade : kAllConfidenceGrades) {
    CHECK(letters.insert(gradeLetter(grade)).second);
  }
  CHECK_EQ(letters.size(), static_cast<size_t>(6));

  // docs/compatibility-brief.md §24: A+ sits above A and is a distinct letter, because
  // a report that collapsed the two would erase the difference between "the printer
  // confirmed this receipt" and "the printer holds a durable job record I can still
  // query after a crash".
  CHECK_EQ(std::string(gradeLetter(ConfidenceGrade::APlus_DurableQueryableJob)),
           std::string("A+"));
  CHECK_EQ(std::string(gradeLetter(ConfidenceGrade::A_JobLevelConfirmation)),
           std::string("A"));
  CHECK_EQ(std::string(to_string(ConfidenceGrade::APlus_DurableQueryableJob)),
           std::string("APlus_DurableQueryableJob"));

  // The hierarchy is ordered strongest-first, and A+ is value 0: the enum's own
  // ordering is what lets a caller compare grades with `<`.
  CHECK(ConfidenceGrade::APlus_DurableQueryableJob < ConfidenceGrade::A_JobLevelConfirmation);
  CHECK(ConfidenceGrade::A_JobLevelConfirmation < ConfidenceGrade::B_OrderedDeviceResponse);
  CHECK(ConfidenceGrade::D_SpoolerCompleted < ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(static_cast<int>(ConfidenceGrade::APlus_DurableQueryableJob), 0);
  CHECK_EQ(static_cast<int>(ConfidenceGrade::E_TransportOnly), 5);

  // Nothing this core can do produces A+ yet: the ePOS transport that would retrieve a
  // JobID result does not exist, so no mechanism maps onto it. Asserting the absence
  // keeps the claim honest until the transport lands and this line has to change.
  for (const CompletionMechanism mechanism :
       {CompletionMechanism::GsParenH, CompletionMechanism::GsR1,
        CompletionMechanism::VendorIdle, CompletionMechanism::EposJobId,
        CompletionMechanism::StarCheckedBlock, CompletionMechanism::None}) {
    CHECK(evidenceFor(mechanism).grade != ConfidenceGrade::APlus_DurableQueryableJob);
  }

  checkNamesUniqueAndNonEmpty(kAllProvenances);
  CHECK_EQ(std::string(to_string(Provenance::Documented)), std::string("Documented"));
  CHECK_EQ(std::string(to_string(Provenance::Probed)), std::string("Probed"));
  CHECK_EQ(std::string(to_string(Provenance::Unverified)), std::string("Unverified"));
}

PD_TEST(job_result_carries_grade_authority_and_method) {
  // docs/device-database.md: every result says what kind of evidence it rests on and
  // who produced it, never a bare success flag.
  const JobResult bare = JobResult::done(ConfidenceLevel::CutFaultFree);
  CHECK_EQ(bare.grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(bare.authority, CompletionAuthority::TransportOnly);
  CHECK_EQ(bare.method, std::string("none"));

  const JobResult graded = JobResult::done(ConfidenceLevel::CutFaultFree)
                               .with(JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                                                 CompletionAuthority::PhysicalPrinter,
                                                 "GS(H) fn48"});
  CHECK_EQ(graded.outcome, JobOutcome::Done);
  CHECK_EQ(graded.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(graded.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(graded.method, std::string("GS(H) fn48"));
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
