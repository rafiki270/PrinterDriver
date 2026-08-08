#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/types.hpp"

// Per model/firmware capability data (docs/sdk-spec.md §8, docs/capability-profiles.md).
// A profile is plain data, determined by the probe in docs/testing-plan.md and shipped
// with the SDK; the engine reads it and never guesses. Its most important job is
// deciding which ConfidenceLevel a job on that printer can ever reach, so the SDK
// reports what the hardware can prove instead of what the caller would like to hear.
//
// Capabilities are compositional, not a flat list of six device names
// (docs/capability-profiles.md "Compositional profile hierarchy"): identity, transport,
// completion, status, recovery, quirks and media are independent facets. A TM-T20III is
// a set of capabilities rather than a case in an inheritance tree, and the probe
// overrides any facet it can establish first-hand.

namespace pd {

// Which ordered completion fence this printer answers (docs/techspec.md §3).
enum class CompletionMechanism {
  GsParenH,          // GS ( H fn 48 process-ID echo — per-receipt correlation token
  GsR1,              // GS r 1 queued paper status — completion, but anonymous
  VendorIdle,        // vendor "working state"/idle query; profile data only
  EposJobId,         // ePOS JobID + queryable print result; profile data only
  StarCheckedBlock,  // StarPRNT begin/endCheckedBlock; profile data only
  None,              // write-only device: no backchannel, no ordered fence
};

// Whether the core's ESC/POS engine can actually drive this mechanism over a raw byte
// transport. The other mechanisms are carried so a profile can describe the hardware
// honestly; pointing this engine at one fails Unsupported rather than pretending.
bool isDrivableByEscposEngine(CompletionMechanism) noexcept;

// docs/device-database.md "Interface ≠ transport ≠ language". Only ESC/POS is
// implemented; the rest exist so a fleet containing them can be described.
enum class CommandLanguage { EscPos, StarPrnt, StarLine, EposXml };

// The cut this printer's mechanism actually performs.
enum class CutVariant { Partial, Full, None };

// Which printer→host stream dialect the parser should assume. Only Epson-like is
// implemented; the others exist so a probe finding can record what it saw.
enum class ResponseParserVariant {
  EpsonLike,   // Epson fixed-bit ASB / DLE EOT patterns, GS ( H frame
  StarPrnt,
  VendorRaw,
};

// docs/capability-profiles.md: vendor · model · firmware · fingerprintConfidence.
// `trusted` is false until something other than GS I agrees with it — Rongta's own
// manual documents GS I returning "EPOSN" / "TM-T88V".
struct DeviceIdentity {
  std::string vendor;
  std::string model;
  std::string firmware;
  std::string serial;
  uint8_t fingerprint_confidence = 0;  // 0-100
  bool trusted = false;
};

// docs/device-database.md "Interface ≠ transport ≠ language".
struct TransportCapabilities {
  bool raw_tcp_9100 = true;
  bool serial = false;
  bool usb = false;
  bool bluetooth_spp = false;
  bool epos = false;  // vendor HTTP/ePOS-Print endpoint
};

struct CompletionCapabilities {
  bool process_id_gs_h = false;  // GS ( H fn 48
  bool queued_gs_r = true;       // GS r 1
  bool vendor_idle = false;
  bool epos_job_id = false;
  // The family's own command manual documents GS ( H even though the shipped default
  // stays on the safer fence until a probe confirms it per model — the Rongta RP80
  // case in docs/capability-profiles.md §5. The probe tries GS ( H on every device
  // regardless; this records where the paperwork says it should work.
  bool try_process_id_gs_h = false;
  // Raw TCP 9100 semantics: exclusive connection, one job in flight, continuous RX
  // parser (docs/device-database.md transport note 1).
  bool one_job_in_flight = true;
  // Bixolon and Star document their own status APIs as the primary path; raw ESC/POS
  // is the fallback there, not the other way round.
  bool prefer_vendor_sdk = false;
};

struct StatusCapabilities {
  bool dle_eot = true;        // real-time DLE EOT 1-4 answers come back at all
  bool asb = true;            // GS a automatic status back
  bool extended_asb = false;  // FS ( e, optional/extended devices
  bool cutter_error = true;   // DLE EOT 3 bit 3 is meaningful on this model
};

// Data only. Nothing in the core ever sends these: a resume replays the line the error
// happened on and a clear discards a partly printed ticket, so both are deliberate
// operator actions behind `pdctl recover` (docs/capability-profiles.md §5).
struct RecoveryCapabilities {
  bool dle_enq_resume = false;  // DLE ENQ 1
  bool dle_enq_clear = false;   // DLE ENQ 2
  bool clear_buffers = false;   // DLE DC4 fn 8
};

struct Quirks {
  // GS V 65/66 n instead of GS V m: feeds to the cut position first. The workaround
  // for clones that cut into the last printed line (docs/sdk-spec.md §9, Rongta).
  // 0 disables it.
  uint8_t extra_feed_before_cut = 0;
  // GS I answers are known to be borrowed from another vendor on this family.
  bool unreliable_identity = false;
  // Status answers arrive late enough that a tight preflight budget times out on a
  // healthy device.
  bool delayed_status = false;
  ResponseParserVariant response_parser = ResponseParserVariant::EpsonLike;
};

// docs/device-database.md "Media is a capability, not a model assumption". Roll width
// and raster width are separate facts: a CT-S4500 takes 112 mm media and prints 104 mm,
// and deriving one from the other is how receipts end up clipped.
struct MediaProfile {
  uint16_t nominal_roll_width_mm = 80;
  uint32_t printable_width_dots = escpos::kWidth80mm;
  uint16_t dpi = 203;
  bool paper_guide_58mm = false;
  bool black_mark_sensor = false;
  bool gap_sensor = false;
  bool near_end_sensor = false;
  bool paper_end_sensor = true;
  bool cover_sensor = true;
  bool cutter = true;
  bool full_cut = true;
  bool partial_cut = true;
};

struct CapabilityProfile {
  std::string name = "generic-escpos";

  DeviceIdentity identity;
  CommandLanguage language = CommandLanguage::EscPos;
  TransportCapabilities transport;
  CompletionMechanism completion = CompletionMechanism::GsR1;
  CompletionCapabilities completion_caps;
  StatusCapabilities status;
  RecoveryCapabilities recovery;
  Quirks quirks;
  MediaProfile media;

  // True once a capability probe has overridden these defaults with first-hand
  // observations (docs/capability-profiles.md §8: generic means UNKNOWN DEVICE).
  bool probed = false;

  // Native cut for this mechanism; used when JobOptions asks for CutSetting::Profile.
  CutVariant cut = CutVariant::Partial;

  // Flow control. chunk_bytes == 0 disables chunking; inter_chunk_delay_ms == 0
  // disables pacing. Both off is the correct setting for a printer with real flow
  // control — the 9-21 ms delays in the legacy stack were empirical, not universal.
  size_t chunk_bytes = 0;
  uint32_t inter_chunk_delay_ms = 0;

  // Budget for one completion wait (marker echo or queued status), not for the job.
  uint32_t completion_timeout_ms = 15000;
  uint32_t preflight_timeout_ms = 2000;

  // Lines fed after the payload so the job ends in a genuine print-and-feed
  // operation, which is what the ordered fence attaches to (docs/techspec.md §5.2).
  uint8_t final_feed_lines = 4;

  escpos::CodePage code_page = escpos::CodePage::PC437;

  // docs/sdk-spec.md §8: the ceiling this hardware allows. The engine clamps every
  // reported confidence to it.
  ConfidenceLevel maxConfidence() const noexcept;

  // The grade/authority a successful completion on this profile can claim
  // (docs/device-database.md "Confidence grades for every route").
  JobEvidence evidence() const noexcept;

  // Whether this core can print this profile at all. False for the vendor stacks
  // that are first-class rather than ESC/POS-emulated, whose entries exist as data.
  bool drivableByEscposEngine() const noexcept;
};

// docs/device-database.md: A — job-level confirmation, B — ordered device response,
// E — transport only. The authority is the physical printer for every mechanism this
// core drives itself; a spooler or print server in the path lowers it, which is why
// the field exists at all.
JobEvidence evidenceFor(CompletionMechanism) noexcept;

// Xprinter XP-S260M as probed on 2026-08-08 over LAN
// (docs/testing-plan.md — GS ( H confirmed, DLE EOT and GS r 1 also answer).
CapabilityProfile xp_s260m();

// The conservative default for anything not yet probed: queued GS r 1 fence, pacing
// on, so a cheap clone with no flow control still receives a whole receipt.
CapabilityProfile generic_escpos();

const char* to_string(CompletionMechanism) noexcept;
const char* to_string(CutVariant) noexcept;
const char* to_string(ResponseParserVariant) noexcept;
const char* to_string(CommandLanguage) noexcept;

}  // namespace pd
