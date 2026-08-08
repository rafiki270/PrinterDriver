#include "printerdriver/types.hpp"

namespace pd {

const char* to_string(JobState value) noexcept {
  switch (value) {
    case JobState::Queued: return "Queued";
    case JobState::PreflightOk: return "PreflightOk";
    case JobState::SendStarted: return "SendStarted";
    case JobState::BytesSent: return "BytesSent";
    case JobState::PrintConfirmed: return "PrintConfirmed";
    case JobState::CutCommandProcessed: return "CutCommandProcessed";
    case JobState::DoneSoftware: return "DoneSoftware";
    case JobState::PhysicallyVerified: return "PhysicallyVerified";
    case JobState::FailedKnown: return "FailedKnown";
    case JobState::Unknown: return "Unknown";
    case JobState::HeldOffline: return "HeldOffline";
  }
  return "Unknown";
}

const char* to_string(ConfidenceLevel value) noexcept {
  switch (value) {
    case ConfidenceLevel::TransportAccepted: return "TransportAccepted";
    case ConfidenceLevel::PrinterHealthy: return "PrinterHealthy";
    case ConfidenceLevel::PrintConfirmed: return "PrintConfirmed";
    case ConfidenceLevel::CutProcessed: return "CutProcessed";
    case ConfidenceLevel::CutFaultFree: return "CutFaultFree";
    case ConfidenceLevel::PhysicallyVerified: return "PhysicallyVerified";
  }
  return "TransportAccepted";
}

const char* to_string(DeviceEvent value) noexcept {
  switch (value) {
    case DeviceEvent::Online: return "Online";
    case DeviceEvent::Offline: return "Offline";
    case DeviceEvent::CoverOpen: return "CoverOpen";
    case DeviceEvent::CoverClosed: return "CoverClosed";
    case DeviceEvent::PaperOut: return "PaperOut";
    case DeviceEvent::PaperNearEnd: return "PaperNearEnd";
    case DeviceEvent::PaperOk: return "PaperOk";
    case DeviceEvent::CutterError: return "CutterError";
    case DeviceEvent::RecoverableError: return "RecoverableError";
    case DeviceEvent::UnrecoverableError: return "UnrecoverableError";
    case DeviceEvent::ConnectionLost: return "ConnectionLost";
    case DeviceEvent::ConnectionRestored: return "ConnectionRestored";
  }
  return "Offline";
}

const char* to_string(FailureReason value) noexcept {
  switch (value) {
    case FailureReason::None: return "None";
    case FailureReason::TransportUnreachable: return "TransportUnreachable";
    case FailureReason::PreflightCoverOpen: return "PreflightCoverOpen";
    case FailureReason::PreflightPaperOut: return "PreflightPaperOut";
    case FailureReason::PreflightHardwareError: return "PreflightHardwareError";
    case FailureReason::TimeoutAwaitingCompletion: return "TimeoutAwaitingCompletion";
    case FailureReason::CutterFault: return "CutterFault";
    case FailureReason::Unsupported: return "Unsupported";
    case FailureReason::Unknown: return "Unknown";
    case FailureReason::Expired: return "Expired";
    case FailureReason::QueueOverflow: return "QueueOverflow";
  }
  return "Unknown";
}

const char* to_string(JobOutcome value) noexcept {
  switch (value) {
    case JobOutcome::Done: return "Done";
    case JobOutcome::Failed: return "Failed";
    case JobOutcome::Unknown: return "Unknown";
  }
  return "Unknown";
}

const char* to_string(ConfidenceGrade value) noexcept {
  switch (value) {
    case ConfidenceGrade::A_JobLevelConfirmation: return "A_JobLevelConfirmation";
    case ConfidenceGrade::B_OrderedDeviceResponse: return "B_OrderedDeviceResponse";
    case ConfidenceGrade::C_DeviceStatusAround: return "C_DeviceStatusAround";
    case ConfidenceGrade::D_SpoolerCompleted: return "D_SpoolerCompleted";
    case ConfidenceGrade::E_TransportOnly: return "E_TransportOnly";
  }
  return "E_TransportOnly";
}

const char* gradeLetter(ConfidenceGrade value) noexcept {
  switch (value) {
    case ConfidenceGrade::A_JobLevelConfirmation: return "A";
    case ConfidenceGrade::B_OrderedDeviceResponse: return "B";
    case ConfidenceGrade::C_DeviceStatusAround: return "C";
    case ConfidenceGrade::D_SpoolerCompleted: return "D";
    case ConfidenceGrade::E_TransportOnly: return "E";
  }
  return "E";
}

const char* to_string(CompletionAuthority value) noexcept {
  switch (value) {
    case CompletionAuthority::PhysicalPrinter: return "PhysicalPrinter";
    case CompletionAuthority::VendorSpooler: return "VendorSpooler";
    case CompletionAuthority::PdAgent: return "PdAgent";
    case CompletionAuthority::PrintServer: return "PrintServer";
    case CompletionAuthority::TransportOnly: return "TransportOnly";
  }
  return "TransportOnly";
}

}  // namespace pd
