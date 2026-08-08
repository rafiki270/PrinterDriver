#include "printerdriver/capability_profile.hpp"

namespace pd {

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
    case CompletionMechanism::None:
      return ConfidenceLevel::TransportAccepted;
  }
  return ConfidenceLevel::TransportAccepted;
}

CapabilityProfile xp_s260m() {
  CapabilityProfile profile;
  profile.name = "xp-s260m";
  profile.completion = CompletionMechanism::GsParenH;
  profile.cut = CutVariant::Partial;
  profile.cut_with_feed = false;
  profile.cut_feed_units = 0;
  profile.supports_asb = true;
  profile.supports_realtime_status = true;
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
  profile.cut = CutVariant::Partial;
  profile.cut_with_feed = false;
  profile.cut_feed_units = 0;
  profile.supports_asb = true;
  profile.supports_realtime_status = true;
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

}  // namespace pd
