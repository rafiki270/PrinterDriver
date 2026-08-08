#include "printerdriver/capability_profile.hpp"

namespace pd {

bool isDrivableByEscposEngine(CompletionMechanism mechanism) noexcept {
  switch (mechanism) {
    case CompletionMechanism::GsParenH:
    case CompletionMechanism::GsR1:
    case CompletionMechanism::None:
      return true;
    case CompletionMechanism::VendorIdle:
    case CompletionMechanism::EposJobId:
    case CompletionMechanism::StarCheckedBlock:
      return false;
  }
  return false;
}

ConfidenceLevel CapabilityProfile::maxConfidence() const noexcept {
  switch (completion) {
    case CompletionMechanism::GsParenH:
      // The process-ID echo fences the cut too, so the post-cut DLE EOT 3 read is
      // ordered and CutFaultFree is honestly reachable (docs/techspec.md §3.1).
      return ConfidenceLevel::CutFaultFree;
    case CompletionMechanism::GsR1:
      // A second GS r 1 after the cut is an ordered fence but not a documented
      // cutter guarantee (docs/techspec.md §3.2), so this stops at CutProcessed.
      return ConfidenceLevel::CutProcessed;
    case CompletionMechanism::VendorIdle:
    case CompletionMechanism::EposJobId:
    case CompletionMechanism::StarCheckedBlock:
      // These confirm that the data printed, never that the blade was fault-free.
      return ConfidenceLevel::CutProcessed;
    case CompletionMechanism::None:
      return ConfidenceLevel::TransportAccepted;
  }
  return ConfidenceLevel::TransportAccepted;
}

JobEvidence evidenceFor(CompletionMechanism mechanism) noexcept {
  switch (mechanism) {
    case CompletionMechanism::GsParenH:
      return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                         CompletionAuthority::PhysicalPrinter, "GS(H) fn48"};
    case CompletionMechanism::GsR1:
      return JobEvidence{ConfidenceGrade::B_OrderedDeviceResponse,
                         CompletionAuthority::PhysicalPrinter, "GS r 1"};
    case CompletionMechanism::VendorIdle:
      return JobEvidence{ConfidenceGrade::B_OrderedDeviceResponse,
                         CompletionAuthority::PhysicalPrinter, "vendor idle query"};
    case CompletionMechanism::EposJobId:
      // The spooler answers, not the mechanism: a JobID result on a spooling TM-i
      // means the spooler finished the job (docs/capability-profiles.md §4).
      return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                         CompletionAuthority::VendorSpooler, "ePOS JobID result"};
    case CompletionMechanism::StarCheckedBlock:
      return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                         CompletionAuthority::PhysicalPrinter, "Star checked block"};
    case CompletionMechanism::None:
      return JobEvidence{ConfidenceGrade::E_TransportOnly,
                         CompletionAuthority::TransportOnly, "transport-only"};
  }
  return JobEvidence{};
}

JobEvidence CapabilityProfile::evidence() const noexcept {
  return evidenceFor(completion);
}

bool CapabilityProfile::drivableByEscposEngine() const noexcept {
  return language == CommandLanguage::EscPos && isDrivableByEscposEngine(completion);
}

CapabilityProfile xp_s260m() {
  CapabilityProfile profile;
  profile.name = "xp-s260m";
  profile.identity.vendor = "Xprinter";
  profile.identity.model = "XP-S260M";
  profile.completion = CompletionMechanism::GsParenH;
  profile.completion_caps.process_id_gs_h = true;
  profile.completion_caps.queued_gs_r = true;
  profile.cut = CutVariant::Partial;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.media.black_mark_sensor = true;
  // Hardware-calibrated on this unit today (docs/testing-plan.md): the cutter sliced
  // the last ~15% of a trailing QR before this was measured and fed.
  profile.media.head_to_cutter_feed_dots = 120;
  // 128 KB input buffer and a LAN module that kept up with unpaced writes during the
  // probe: chunking would only add latency.
  profile.chunk_bytes = 0;
  profile.inter_chunk_delay_ms = 0;
  profile.completion_timeout_ms = 15000;
  profile.preflight_timeout_ms = 2000;
  profile.final_feed_lines = 3;
  profile.code_page = escpos::CodePage::PC852;
  return profile;
}

CapabilityProfile generic_escpos() {
  CapabilityProfile profile;
  profile.name = "generic-escpos";
  profile.completion = CompletionMechanism::GsR1;
  profile.completion_caps.process_id_gs_h = false;
  profile.completion_caps.queued_gs_r = true;
  profile.cut = CutVariant::Partial;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.quirks.unreliable_identity = true;
  profile.chunk_bytes = 1024;
  profile.inter_chunk_delay_ms = 20;
  profile.completion_timeout_ms = 20000;
  profile.preflight_timeout_ms = 2000;
  // Clones that cut into the last line are common enough that the conservative
  // default pays for the paper (docs/sdk-spec.md §9).
  profile.final_feed_lines = 6;
  profile.code_page = escpos::CodePage::PC437;
  return profile;
}

const char* to_string(CompletionMechanism mechanism) noexcept {
  switch (mechanism) {
    case CompletionMechanism::GsParenH: return "GsParenH";
    case CompletionMechanism::GsR1: return "GsR1";
    case CompletionMechanism::VendorIdle: return "VendorIdle";
    case CompletionMechanism::EposJobId: return "EposJobId";
    case CompletionMechanism::StarCheckedBlock: return "StarCheckedBlock";
    case CompletionMechanism::None: return "None";
  }
  return "None";
}

const char* to_string(CutVariant variant) noexcept {
  switch (variant) {
    case CutVariant::Partial: return "Partial";
    case CutVariant::Full: return "Full";
    case CutVariant::None: return "None";
  }
  return "None";
}

const char* to_string(CommandLanguage language) noexcept {
  switch (language) {
    case CommandLanguage::EscPos: return "EscPos";
    case CommandLanguage::StarPrnt: return "StarPrnt";
    case CommandLanguage::StarLine: return "StarLine";
    case CommandLanguage::EposXml: return "EposXml";
  }
  return "EscPos";
}

const char* to_string(ResponseParserVariant variant) noexcept {
  switch (variant) {
    case ResponseParserVariant::EpsonLike: return "EpsonLike";
    case ResponseParserVariant::StarPrnt: return "StarPrnt";
    case ResponseParserVariant::VendorRaw: return "VendorRaw";
  }
  return "EpsonLike";
}

}  // namespace pd
