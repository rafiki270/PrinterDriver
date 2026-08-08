#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

// Closed enums for the whole SDK. Every platform wrapper re-exports these verbatim;
// adding a member is a core change that must flow to all wrappers at once
// (docs/sdk-spec.md §5). The kAll* arrays below exist so wrapper generators and
// tests can enumerate members without hand-maintained lists.

namespace pd {

// docs/sdk-spec.md §5, mirroring docs/techspec.md §5.1.
enum class JobState {
  Queued,
  PreflightOk,
  SendStarted,
  BytesSent,
  PrintConfirmed,
  CutCommandProcessed,
  DoneSoftware,
  PhysicallyVerified,
  FailedKnown,
  Unknown,
  // Reserved for the print-queue addon (docs/sdk-spec.md §12). The core never
  // produces it; it is defined here so wrappers ship the complete enum from day one.
  HeldOffline,
};

// What evidence backs the current claim. The SDK never upgrades this on its own.
enum class ConfidenceLevel {
  TransportAccepted,
  PrinterHealthy,
  PrintConfirmed,
  CutProcessed,
  CutFaultFree,
  PhysicallyVerified,
};

enum class DeviceEvent {
  Online,
  Offline,
  CoverOpen,
  CoverClosed,
  PaperOut,
  PaperNearEnd,
  PaperOk,
  CutterError,
  RecoverableError,
  UnrecoverableError,
  ConnectionLost,
  ConnectionRestored,
  // A structurally valid GS ( H echo arrived carrying a token this driver instance
  // never issued: something else is writing to the same printer (docs/api.md §14,
  // docs/sdk-spec.md §14). The echo is not attributed to any job and satisfies no
  // outstanding fence — it is reported so a violated one-owner topology is loud
  // instead of silently misrouting acknowledgements.
  ForeignWriterDetected,
};

enum class FailureReason {
  None,
  TransportUnreachable,
  PreflightCoverOpen,
  PreflightPaperOut,
  PreflightHardwareError,
  TimeoutAwaitingCompletion,
  CutterFault,
  Unsupported,
  Unknown,
  // Reserved for the print-queue addon (docs/sdk-spec.md §12).
  Expired,
  QueueOverflow,
};

// docs/api.md §1.4: deliberately tri-state. There is no isSuccess boolean because
// collapsing Unknown into either bucket is the bug that produces duplicate tickets.
enum class JobOutcome {
  Done,
  Failed,
  Unknown,
};

// docs/device-database.md "Confidence grades for every route". Orthogonal to
// ConfidenceLevel: the level says how far up the evidence ladder a job climbed, the
// grade says what class of evidence the claim rests on. A job can be Done at
// CutProcessed on grade B and Done at CutProcessed on grade D, and those are not the
// same claim.
enum class ConfidenceGrade {
  A_JobLevelConfirmation,   // GS ( H, ePOS JobID result, Star checked block
  B_OrderedDeviceResponse,  // GS r, vendor idle query
  C_DeviceStatusAround,     // DLE EOT, ASB, SNMP taken around the transmission
  D_SpoolerCompleted,       // CUPS/Windows spooler/IPP gateway said completed
  E_TransportOnly,          // the write succeeded and nothing else is known
};

// Who is actually making the claim carried by a JobResult. The whole point of
// recording it is that "completed" from a print server and "completed" from the
// mechanism that moved the paper are different facts (docs/device-database.md §D).
enum class CompletionAuthority {
  PhysicalPrinter,
  VendorSpooler,
  PdAgent,
  PrintServer,
  TransportOnly,
};

constexpr std::array<JobState, 11> kAllJobStates{
    JobState::Queued,        JobState::PreflightOk,         JobState::SendStarted,
    JobState::BytesSent,     JobState::PrintConfirmed,      JobState::CutCommandProcessed,
    JobState::DoneSoftware,  JobState::PhysicallyVerified,  JobState::FailedKnown,
    JobState::Unknown,       JobState::HeldOffline,
};

constexpr std::array<ConfidenceLevel, 6> kAllConfidenceLevels{
    ConfidenceLevel::TransportAccepted, ConfidenceLevel::PrinterHealthy,
    ConfidenceLevel::PrintConfirmed,    ConfidenceLevel::CutProcessed,
    ConfidenceLevel::CutFaultFree,      ConfidenceLevel::PhysicallyVerified,
};

constexpr std::array<DeviceEvent, 13> kAllDeviceEvents{
    DeviceEvent::Online,       DeviceEvent::Offline,
    DeviceEvent::CoverOpen,    DeviceEvent::CoverClosed,
    DeviceEvent::PaperOut,     DeviceEvent::PaperNearEnd,
    DeviceEvent::PaperOk,      DeviceEvent::CutterError,
    DeviceEvent::RecoverableError, DeviceEvent::UnrecoverableError,
    DeviceEvent::ConnectionLost,   DeviceEvent::ConnectionRestored,
    DeviceEvent::ForeignWriterDetected,
};

constexpr std::array<FailureReason, 11> kAllFailureReasons{
    FailureReason::None,
    FailureReason::TransportUnreachable,
    FailureReason::PreflightCoverOpen,
    FailureReason::PreflightPaperOut,
    FailureReason::PreflightHardwareError,
    FailureReason::TimeoutAwaitingCompletion,
    FailureReason::CutterFault,
    FailureReason::Unsupported,
    FailureReason::Unknown,
    FailureReason::Expired,
    FailureReason::QueueOverflow,
};

constexpr std::array<JobOutcome, 3> kAllJobOutcomes{
    JobOutcome::Done,
    JobOutcome::Failed,
    JobOutcome::Unknown,
};

constexpr std::array<ConfidenceGrade, 5> kAllConfidenceGrades{
    ConfidenceGrade::A_JobLevelConfirmation, ConfidenceGrade::B_OrderedDeviceResponse,
    ConfidenceGrade::C_DeviceStatusAround,   ConfidenceGrade::D_SpoolerCompleted,
    ConfidenceGrade::E_TransportOnly,
};

constexpr std::array<CompletionAuthority, 5> kAllCompletionAuthorities{
    CompletionAuthority::PhysicalPrinter, CompletionAuthority::VendorSpooler,
    CompletionAuthority::PdAgent,         CompletionAuthority::PrintServer,
    CompletionAuthority::TransportOnly,
};

const char* to_string(JobState) noexcept;
const char* to_string(ConfidenceLevel) noexcept;
const char* to_string(DeviceEvent) noexcept;
const char* to_string(FailureReason) noexcept;
const char* to_string(JobOutcome) noexcept;
const char* to_string(ConfidenceGrade) noexcept;
const char* to_string(CompletionAuthority) noexcept;

// "A".."E" for reports, where the enumerator name is too long to tabulate.
const char* gradeLetter(ConfidenceGrade) noexcept;

// What kind of evidence produced a result, and who produced it
// (docs/device-database.md: never a bare {success:true}). `method` names the actual
// command, e.g. "GS(H) fn48" — the string a support engineer needs six months later.
struct JobEvidence {
  ConfidenceGrade grade = ConfidenceGrade::E_TransportOnly;
  CompletionAuthority authority = CompletionAuthority::TransportOnly;
  const char* method = "none";
};

// Terminal outcome of a job. `confidence` is carried on every outcome, not only
// Done: on Failed and Unknown it records how far up the evidence ladder the job
// actually got before it stopped, which is what an operator needs in order to
// decide about a possible reprint.
struct JobResult {
  JobOutcome outcome = JobOutcome::Unknown;
  ConfidenceLevel confidence = ConfidenceLevel::TransportAccepted;
  FailureReason reason = FailureReason::None;

  ConfidenceGrade grade = ConfidenceGrade::E_TransportOnly;
  CompletionAuthority authority = CompletionAuthority::TransportOnly;
  std::string method = "none";

  JobResult& with(const JobEvidence& evidence) {
    grade = evidence.grade;
    authority = evidence.authority;
    method = evidence.method;
    return *this;
  }

  static JobResult done(ConfidenceLevel confidence) {
    return JobResult{JobOutcome::Done, confidence, FailureReason::None};
  }
  static JobResult failed(FailureReason reason,
                          ConfidenceLevel reached = ConfidenceLevel::TransportAccepted) {
    return JobResult{JobOutcome::Failed, reached, reason};
  }
  static JobResult unknown(ConfidenceLevel reached = ConfidenceLevel::TransportAccepted) {
    return JobResult{JobOutcome::Unknown, reached, FailureReason::Unknown};
  }
};

// Monotonic: the wall clock may step; job timing must not.
using MonotonicClock = std::chrono::steady_clock;
using MonotonicTime = MonotonicClock::time_point;

struct JobEvent {
  JobState state = JobState::Queued;
  ConfidenceLevel confidence = ConfidenceLevel::TransportAccepted;
  std::optional<FailureReason> reason;
  MonotonicTime at{};

  static JobEvent make(JobState state, ConfidenceLevel confidence,
                       std::optional<FailureReason> reason = std::nullopt,
                       MonotonicTime at = MonotonicClock::now()) noexcept {
    return JobEvent{state, confidence, reason, at};
  }
};

}  // namespace pd
