#include "printerdriver/device_profiles.hpp"

#include <algorithm>

namespace pd::devices {
namespace {

// 80 mm media, 72 mm printable, 576 dots at 203 dpi — the geometry shared by most of
// the database. Recorded as three separate facts, never derived from each other.
CapabilityProfile thermal80() {
  CapabilityProfile profile;
  profile.completion = CompletionMechanism::GsR1;
  profile.completion_caps.queued_gs_r = true;
  profile.cut = CutVariant::Partial;
  profile.media.nominal_roll_width_mm = 80;
  profile.media.printable_width_dots = escpos::kWidth80mm;
  profile.media.dpi = 203;
  profile.media.cutter = true;
  profile.media.partial_cut = true;
  profile.media.full_cut = true;
  // Conservative until a probe or a hardware soak calibrates it per model
  // (docs/testing-plan.md).
  profile.media.head_to_cutter_feed_dots = 120;
  profile.chunk_bytes = 0;
  profile.inter_chunk_delay_ms = 0;
  profile.completion_timeout_ms = 15000;
  profile.preflight_timeout_ms = 2000;
  profile.final_feed_lines = 4;
  profile.code_page = escpos::CodePage::PC437;
  return profile;
}

// 58 mm media. Usable width varies from ~48 to ~52 mm across this class, so the
// conservative 384 dots is used unless a model documents otherwise.
void makeFiftyEight(CapabilityProfile& profile) {
  profile.media.nominal_roll_width_mm = 58;
  profile.media.printable_width_dots = escpos::kWidth58mm;
  profile.media.full_cut = false;
}

CapabilityProfile epsonBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Epson";
  profile.completion = CompletionMechanism::GsParenH;
  profile.completion_caps.process_id_gs_h = true;
  profile.completion_caps.try_process_id_gs_h = true;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.recovery.dle_enq_resume = true;
  profile.recovery.dle_enq_clear = true;
  profile.recovery.clear_buffers = true;  // DLE DC4 fn 8
  profile.transport.usb = true;
  profile.transport.serial = true;
  profile.final_feed_lines = 3;
  // Mid-range of the TM family's documented 10-15 mm head-to-cutter gap at 203 dpi;
  // tighten per model once a unit is actually probed.
  profile.media.head_to_cutter_feed_dots = 100;
  return profile;
}

CapabilityProfile rongtaBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Rongta";
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.recovery.dle_enq_resume = true;  // DLE ENQ 1, operator-only
  profile.recovery.dle_enq_clear = true;   // DLE ENQ 2, operator-only
  profile.quirks.unreliable_identity = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  // The fleet quirk from docs/sdk-spec.md §9: these cut into the last printed line,
  // which the deployed stack compensates for with six feeds before the cut. CP852 is
  // the code page those installations run.
  profile.final_feed_lines = 6;
  profile.code_page = escpos::CodePage::PC852;
  return profile;
}

CapabilityProfile xprinterBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Xprinter";
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  profile.code_page = escpos::CodePage::PC852;
  return profile;
}

CapabilityProfile starBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Star Micronics";
  // StarPRNT, not ESC/POS: the checked-block API is the documented way to learn
  // whether the whole data printed, and this core does not speak it. Profile data
  // only — the engine refuses these jobs rather than driving Star's ESC/POS emulation
  // and reporting a fence it never really got.
  profile.language = CommandLanguage::StarPrnt;
  profile.completion = CompletionMechanism::StarCheckedBlock;
  profile.completion_caps.queued_gs_r = false;
  profile.completion_caps.prefer_vendor_sdk = true;
  profile.status.dle_eot = false;
  profile.status.asb = false;
  profile.status.cutter_error = false;
  profile.quirks.response_parser = ResponseParserVariant::StarPrnt;
  profile.transport.usb = true;
  return profile;
}

CapabilityProfile bixolonBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Bixolon";
  // "ESC/POS compatible" is a marketing statement, not a command table: the vendor
  // status SDK is the documented path and GS ( H is never assumed from the phrase.
  profile.completion_caps.prefer_vendor_sdk = true;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.quirks.unreliable_identity = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  return profile;
}

CapabilityProfile citizenBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Citizen";
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  return profile;
}

struct Entry {
  const char* name;
  CapabilityProfile (*build)();
};

const std::vector<Entry>& table() {
  static const std::vector<Entry> entries{
      {"epson_tm_t20", &epson_tm_t20},
      {"epson_tm_t70", &epson_tm_t70},
      {"epson_tm_t88", &epson_tm_t88},
      {"epson_tm_m", &epson_tm_m},
      {"epson_tm_u220", &epson_tm_u220},
      {"epson_tm_i", &epson_tm_i},
      {"rongta_rp58", &rongta_rp58},
      {"rongta_rp80", &rongta_rp80},
      {"rongta_rp3xx", &rongta_rp3xx},
      {"xprinter_pos58", &xprinter_pos58},
      {"xprinter_pos80", &xprinter_pos80},
      {"xprinter_s_series", &xprinter_s_series},
      {"sewoo_slk_ts", &sewoo_slk_ts},
      {"partner_rp110", &partner_rp110},
      {"star_tsp100", &star_tsp100},
      {"star_tsp650", &star_tsp650},
      {"star_mcprint", &star_mcprint},
      {"bixolon_srp330", &bixolon_srp330},
      {"bixolon_srp350", &bixolon_srp350},
      {"bixolon_srp380", &bixolon_srp380},
      {"bixolon_q_series", &bixolon_q_series},
      {"citizen_cts_58_80", &citizen_cts_58_80},
      {"citizen_cts_fast", &citizen_cts_fast},
      {"citizen_cts_wide", &citizen_cts_wide},
      {"generic_80", &generic_80},
      {"generic_58", &generic_58},
  };
  return entries;
}

}  // namespace

// --- Epson --------------------------------------------------------------------------

CapabilityProfile epson_tm_t20() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t20";
  profile.identity.model = "TM-T20 series";
  // 58 mm operation is a paper-guide configuration of the same mechanism, printing
  // 420 dots — a separate fact from the 576 the 80 mm setup prints.
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile epson_tm_t70() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t70";
  profile.identity.model = "TM-T70 series";
  return profile;
}

CapabilityProfile epson_tm_t88() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t88";
  profile.identity.model = "TM-T88 series";
  profile.status.extended_asb = true;  // FS ( e
  profile.media.near_end_sensor = true;
  profile.media.paper_guide_58mm = true;
  profile.transport.epos = true;
  return profile;
}

CapabilityProfile epson_tm_m() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_m";
  profile.identity.model = "TM-m series";
  profile.status.extended_asb = true;
  profile.media.paper_guide_58mm = true;
  profile.transport.epos = true;
  profile.transport.bluetooth_spp = true;
  return profile;
}

CapabilityProfile epson_tm_u220() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_u220";
  profile.identity.model = "TM-U220 series";
  // Dot impact, not thermal: 76 mm paper, 63.5 mm print width, 400 dots at 160 dpi.
  // Nothing about this geometry follows from the thermal 8-dots-per-mm rule.
  profile.media.nominal_roll_width_mm = 76;
  profile.media.printable_width_dots = 400;
  profile.media.dpi = 160;
  profile.media.full_cut = false;  // the mechanism's autocutter is partial-cut only
  profile.completion = CompletionMechanism::GsR1;
  profile.completion_caps.process_id_gs_h = false;
  profile.completion_caps.try_process_id_gs_h = false;
  // Impact printing is slow enough that a completion wait sized for thermal speeds
  // times out on a long ticket.
  profile.completion_timeout_ms = 30000;
  profile.quirks.delayed_status = true;
  return profile;
}

CapabilityProfile epson_tm_i() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_i";
  profile.identity.model = "TM-i / DT series";
  // Both paths are real on this hardware. The ESC/POS fence stays the drivable
  // default; the ePOS JobID is the stronger mechanism once that transport exists,
  // and its result comes from the spooler rather than from the mechanism
  // (docs/capability-profiles.md §4).
  profile.completion_caps.epos_job_id = true;
  profile.transport.epos = true;
  profile.status.extended_asb = true;
  return profile;
}

// --- Rongta -------------------------------------------------------------------------

CapabilityProfile rongta_rp58() {
  CapabilityProfile profile = rongtaBase();
  profile.name = "rongta_rp58";
  profile.identity.model = "RP58 series";
  makeFiftyEight(profile);
  // The 58 mm line is the least documented of the family: stay on the queued fence
  // and let the probe promote it.
  profile.chunk_bytes = 1024;
  profile.inter_chunk_delay_ms = 20;
  return profile;
}

CapabilityProfile rongta_rp80() {
  CapabilityProfile profile = rongtaBase();
  profile.name = "rongta_rp80";
  profile.identity.model = "RP80 family";
  // The family manual documents GS ( H fn 48, but firmware varies across RP80/802/
  // 803/807/820/850, so the shipped default stays on the queued fence and the probe
  // decides per unit.
  profile.completion_caps.try_process_id_gs_h = true;
  return profile;
}

CapabilityProfile rongta_rp3xx() {
  CapabilityProfile profile = rongtaBase();
  profile.name = "rongta_rp3xx";
  profile.identity.model = "RP3xx series";
  profile.completion_caps.try_process_id_gs_h = true;
  return profile;
}

// --- Xprinter -----------------------------------------------------------------------

CapabilityProfile xprinter_pos58() {
  CapabilityProfile profile = xprinterBase();
  profile.name = "xprinter_pos58";
  profile.identity.model = "58 mm series";
  makeFiftyEight(profile);
  profile.chunk_bytes = 1024;
  profile.inter_chunk_delay_ms = 20;
  profile.final_feed_lines = 6;
  return profile;
}

CapabilityProfile xprinter_pos80() {
  CapabilityProfile profile = xprinterBase();
  profile.name = "xprinter_pos80";
  profile.identity.model = "80 mm series";
  profile.completion_caps.try_process_id_gs_h = true;
  profile.final_feed_lines = 6;
  return profile;
}

CapabilityProfile xprinter_s_series() {
  CapabilityProfile profile = xprinterBase();
  profile.name = "xprinter_s_series";
  profile.identity.model = "S series (S200/S260/S300)";
  // XP-S260M: GS ( H proven on our unit by probe and a 100-receipt soak
  // (docs/capability-profiles.md §7).
  profile.completion = CompletionMechanism::GsParenH;
  profile.completion_caps.process_id_gs_h = true;
  profile.completion_caps.try_process_id_gs_h = true;
  profile.completion_caps.vendor_idle = true;  // Xprinter one-ticket-one-control
  profile.media.black_mark_sensor = true;
  profile.final_feed_lines = 3;
  return profile;
}

// --- Sewoo / Partner Tech -----------------------------------------------------------

CapabilityProfile sewoo_slk_ts() {
  CapabilityProfile profile = thermal80();
  profile.name = "sewoo_slk_ts";
  profile.identity.vendor = "Sewoo";
  profile.identity.model = "SLK-TS series";
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.recovery.dle_enq_resume = true;
  profile.recovery.dle_enq_clear = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  // No authoritative public programmer's manual was found for this platform, so
  // GS ( H is a probe question rather than a default (docs/capability-profiles.md §6).
  profile.completion_caps.try_process_id_gs_h = true;
  return profile;
}

CapabilityProfile partner_rp110() {
  CapabilityProfile profile = sewoo_slk_ts();
  profile.name = "partner_rp110";
  profile.identity.vendor = "Partner Tech";
  profile.identity.model = "RP-110";
  // ENERGY STAR certification records group RP-110/RP-110B/RP-110W with SLK-TS200
  // under one product family, so the Sewoo profile is the honest starting point.
  profile.media.near_end_sensor = true;
  return profile;
}

// --- Star ---------------------------------------------------------------------------

CapabilityProfile star_tsp100() {
  CapabilityProfile profile = starBase();
  profile.name = "star_tsp100";
  profile.identity.model = "TSP100 series";
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile star_tsp650() {
  CapabilityProfile profile = starBase();
  profile.name = "star_tsp650";
  profile.identity.model = "TSP650II";
  profile.media.paper_guide_58mm = true;
  profile.transport.serial = true;
  profile.transport.bluetooth_spp = true;
  return profile;
}

CapabilityProfile star_mcprint() {
  CapabilityProfile profile = starBase();
  profile.name = "star_mcprint";
  profile.identity.model = "mC-Print series";
  profile.media.paper_guide_58mm = true;
  profile.transport.bluetooth_spp = true;
  return profile;
}

// --- Bixolon ------------------------------------------------------------------------

CapabilityProfile bixolon_srp330() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp330";
  profile.identity.model = "SRP-330III/332III";
  return profile;
}

CapabilityProfile bixolon_srp350() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp350";
  profile.identity.model = "SRP-350III/V, 352, 350plus";
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile bixolon_srp380() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp380";
  profile.identity.model = "SRP-380/382/380plus";
  return profile;
}

CapabilityProfile bixolon_q_series() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_q_series";
  profile.identity.model = "SRP-Q200/Q300";
  // The Q line spans both widths; 80 mm is the Q300 default and a Q200 must be
  // configured to 58 mm rather than inferred.
  profile.media.paper_guide_58mm = true;
  profile.transport.bluetooth_spp = true;
  return profile;
}

// --- Citizen ------------------------------------------------------------------------

CapabilityProfile citizen_cts_58_80() {
  CapabilityProfile profile = citizenBase();
  profile.name = "citizen_cts_58_80";
  profile.identity.model = "CT-S310II / CT-E301/351/601/651";
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile citizen_cts_fast() {
  CapabilityProfile profile = citizenBase();
  profile.name = "citizen_cts_fast";
  profile.identity.model = "CT-S801III / CT-S851III";
  profile.media.near_end_sensor = true;
  return profile;
}

CapabilityProfile citizen_cts_wide() {
  CapabilityProfile profile = citizenBase();
  profile.name = "citizen_cts_wide";
  profile.identity.model = "CT-S4500 / CT-S4000";
  // Accepts 58-112 mm media and prints at most 104 mm. The two numbers are unrelated
  // and a renderer that derives one from the other clips every wide receipt.
  profile.media.nominal_roll_width_mm = 112;
  profile.media.printable_width_dots = escpos::kWidth104mm;
  profile.media.black_mark_sensor = true;
  profile.media.near_end_sensor = true;
  return profile;
}

// --- Generic ------------------------------------------------------------------------

CapabilityProfile generic_80() {
  CapabilityProfile profile = thermal80();
  profile.name = "generic_80";
  profile.identity.vendor = "Unknown";
  profile.quirks.unreliable_identity = true;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.chunk_bytes = 1024;
  profile.inter_chunk_delay_ms = 20;
  profile.completion_timeout_ms = 20000;
  profile.final_feed_lines = 6;
  return profile;
}

CapabilityProfile generic_58() {
  CapabilityProfile profile = generic_80();
  profile.name = "generic_58";
  makeFiftyEight(profile);
  // Ultra-conservative: this class routinely ships without ASB, without a full cut,
  // and without flow control, so nothing beyond printable bytes is assumed until a
  // probe says otherwise.
  profile.status.asb = false;
  profile.status.cutter_error = false;
  profile.chunk_bytes = 512;
  profile.inter_chunk_delay_ms = 30;
  profile.final_feed_lines = 6;
  return profile;
}

// --- Lookup -------------------------------------------------------------------------

bool exists(const std::string& name) {
  const std::vector<Entry>& entries = table();
  return std::any_of(entries.begin(), entries.end(),
                     [&name](const Entry& entry) { return name == entry.name; });
}

CapabilityProfile byName(const std::string& name) {
  for (const Entry& entry : table()) {
    if (name == entry.name) {
      return entry.build();
    }
  }
  // An unknown name is an unknown device, which is exactly what generic_80 means.
  return generic_80();
}

std::vector<std::string> names() {
  std::vector<std::string> out;
  out.reserve(table().size());
  for (const Entry& entry : table()) {
    out.emplace_back(entry.name);
  }
  return out;
}

}  // namespace pd::devices
