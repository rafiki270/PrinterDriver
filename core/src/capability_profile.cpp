#include "printerdriver/capability_profile.hpp"

namespace pd {

void CommandLanguages::add(CommandLanguage language) noexcept {
  switch (language) {
    case CommandLanguage::EscPos: esc_pos = true; return;
    case CommandLanguage::StarPrnt: star_prnt = true; return;
    case CommandLanguage::StarLine: star_line = true; return;
    case CommandLanguage::EposXml: epos_xml = true; return;
    case CommandLanguage::Zpl: zpl = true; return;
    case CommandLanguage::Cpcl: cpcl = true; return;
    case CommandLanguage::BrotherRaster: brother_raster = true; return;
    case CommandLanguage::EscP: esc_p = true; return;
  }
}

bool CommandLanguages::has(CommandLanguage language) const noexcept {
  switch (language) {
    case CommandLanguage::EscPos: return esc_pos;
    case CommandLanguage::StarPrnt: return star_prnt;
    case CommandLanguage::StarLine: return star_line;
    case CommandLanguage::EposXml: return epos_xml;
    case CommandLanguage::Zpl: return zpl;
    case CommandLanguage::Cpcl: return cpcl;
    case CommandLanguage::BrotherRaster: return brother_raster;
    case CommandLanguage::EscP: return esc_p;
  }
  return false;
}

size_t CommandLanguages::count() const noexcept {
  return static_cast<size_t>(esc_pos) + star_prnt + star_line + epos_xml + zpl + cpcl +
         brother_raster + esc_p;
}

void CapabilityProfile::setLanguage(CommandLanguage primary) noexcept {
  language = primary;
  // Resets rather than accumulates. Profiles are built by copying a base and then
  // narrowing it, so a Zebra derived from the shared 80 mm thermal base must not keep
  // the base's ESC/POS in its documented set — that would say a ZPL printer accepts
  // ESC/POS, which is the precise mistake §16 and §17 exist to stop. Extra documented
  // languages are added afterwards with languages.add().
  languages = CommandLanguages{};
  languages.add(primary);
}

bool isDrivableByEscposEngine(CompletionMechanism mechanism) noexcept {
  switch (mechanism) {
    case CompletionMechanism::GsParenH:
    case CompletionMechanism::GsR1:
    case CompletionMechanism::None:
      return true;
    case CompletionMechanism::VendorIdle:
    case CompletionMechanism::EposJobId:
    case CompletionMechanism::StarCheckedBlock:
    // M13b: Star's raw fences are drivable, but by the Star engine, not this one.
    case CompletionMechanism::StarEtb:
    case CompletionMechanism::StarEscGsEtx:
      return false;
  }
  return false;
}

// --- M13b (docs/wire-protocols.md §2) ------------------------------------------------
bool isDrivableByStarEngine(CompletionMechanism mechanism) noexcept {
  switch (mechanism) {
    case CompletionMechanism::StarEtb:
    case CompletionMechanism::StarEscGsEtx:
    // A Star printer with no fence at all still prints; it simply cannot say more than
    // "the bytes left", which is exactly what CompletionMechanism::None means everywhere
    // else and is graded the same E.
    case CompletionMechanism::None:
      return true;
    case CompletionMechanism::GsParenH:
    case CompletionMechanism::GsR1:
    case CompletionMechanism::VendorIdle:
    case CompletionMechanism::EposJobId:
    // StarCheckedBlock stays undrivable on purpose. It is an SDK call
    // (beginCheckedBlock/endCheckedBlock), not a wire primitive, and docs/wire-protocols.md
    // §2 is explicit that the SDK's checked block uses ETB counting on Line Mode printers
    // but that the same primitive must not be assumed per emulation and interface. A
    // profile that claims the SDK mechanism is describing the SDK, not this socket.
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
    // M13b: both Star fences wait for all preceding printing and motor activity, so a
    // fence placed after the cut command proves the cut was *processed*. Neither carries
    // a cutter-fault bit, so CutFaultFree is out of reach here exactly as it is for GS r 1.
    case CompletionMechanism::StarEtb:
    case CompletionMechanism::StarEscGsEtx:
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
      //
      // M13b: this now grades A+, and the promotion is earned rather than aspirational.
      // Until this milestone the core had no ePOS transport, so nothing could submit a
      // job, receive a JobID or retrieve its result, and grading the *capability* A+
      // would have been the exact overclaim the ladder exists to prevent. The client in
      // core/src/epos.cpp does all three: it posts the documented SOAP envelope with a
      // printjobid, polls with an empty epos-print body until the job reaches a terminal
      // state, and returns the retrieved result — which is the durable, queryable job
      // docs/compatibility-brief.md §24 puts at the top of the hierarchy, and the only
      // mechanism here that survives the application losing its connection between
      // submission and answer.
      return JobEvidence{ConfidenceGrade::APlus_DurableQueryableJob,
                         CompletionAuthority::VendorSpooler, "ePOS JobID"};
    case CompletionMechanism::StarCheckedBlock:
      return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                         CompletionAuthority::PhysicalPrinter, "Star checked block"};
    // --- M13b (docs/wire-protocols.md §2) --------------------------------------------
    // Both are explicit device completion — the printer itself waits for all preceding
    // printing before answering — so both are grade A with the physical printer as the
    // authority. The method strings name the actual wire primitive, because "Star" alone
    // would not tell a support engineer which of the two fences produced the claim, and
    // the two have very different misattribution properties on a shared 9100 port.
    case CompletionMechanism::StarEtb:
      return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                         CompletionAuthority::PhysicalPrinter, "ETB"};
    case CompletionMechanism::StarEscGsEtx:
      return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                         CompletionAuthority::PhysicalPrinter, "ESC GS ETX"};
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

// --- M13b (docs/wire-protocols.md §2) ------------------------------------------------
bool CapabilityProfile::drivableByStarEngine() const noexcept {
  if (language != CommandLanguage::StarPrnt && language != CommandLanguage::StarLine) {
    return false;
  }
  if (!isDrivableByStarEngine(completion)) {
    return false;
  }
  // The ETB gate. ASB frames are broadcast to every host connected to TCP 9100, so a
  // counter increment carries no evidence about *which* client's data finished. Refusing
  // here rather than at fence time is deliberate: a profile that selects ETB without
  // declaring an exclusive session is a configuration mistake, and the honest answer is
  // Unsupported before any paper moves, not a confident claim built on a shared counter.
  if (completion == CompletionMechanism::StarEtb && !star.exclusive_single_session) {
    return false;
  }
  return true;
}

bool CapabilityProfile::drivable() const noexcept {
  return drivableByEscposEngine() || drivableByStarEngine();
}

CapabilityProfile xp_s260m() {
  CapabilityProfile profile;
  profile.name = "xp-s260m";
  profile.identity.vendor = "Xprinter";
  profile.identity.model = "XP-S260M";
  profile.setLanguage(CommandLanguage::EscPos);
  profile.completion = CompletionMechanism::GsParenH;
  profile.completion_caps.process_id_gs_h = true;
  profile.completion_caps.queued_gs_r = true;
  profile.cut = CutVariant::Partial;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  // docs/compatibility-brief.md §14, §28: Xprinter's public documentation does not
  // prove GS ( H, but this unit answered it on the bench and through a 100-receipt
  // soak. Probed, not Documented — and stronger than the datasheet either way. This
  // entry describes one interrogated machine, which is why it carries Probed where the
  // xprinter_* family defaults carry Unverified. `probed` itself stays false: that flag
  // means *this* profile instance was promoted by a probe at runtime, and a shipped
  // default has not been, however it was originally learned.
  profile.completion_caps.process_id_gs_h_provenance = Provenance::Probed;
  profile.completion_caps.queued_gs_r_provenance = Provenance::Probed;
  profile.status.dle_eot_provenance = Provenance::Probed;
  profile.status.asb_provenance = Provenance::Probed;
  profile.status.cutter_error_provenance = Provenance::Probed;
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
  // Every provenance stays Unverified here, which is the whole content of the word
  // "generic": docs/capability-profiles.md §8 — an unknown device, not a device known
  // to be plain (docs/compatibility-brief.md §28).
  profile.setLanguage(CommandLanguage::EscPos);
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
    // M13b
    case CompletionMechanism::StarEtb: return "StarEtb";
    case CompletionMechanism::StarEscGsEtx: return "StarEscGsEtx";
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
    case CommandLanguage::Zpl: return "Zpl";
    case CommandLanguage::Cpcl: return "Cpcl";
    case CommandLanguage::BrotherRaster: return "BrotherRaster";
    case CommandLanguage::EscP: return "EscP";
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
