#pragma once

#include <array>
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

// docs/device-database.md "Interface ≠ transport ≠ language",
// docs/compatibility-brief.md §1. Only ESC/POS is implemented; the rest exist so a
// fleet containing them can be described rather than misdriven. Zebra (ZPL/CPCL) and
// Brother (raster/ESC/P) are the reason this matters: pointing an ESC/POS engine at
// either produces confetti, so a profile that names them is refused outright
// (drivableByEscposEngine) instead of being sent bytes it cannot read.
enum class CommandLanguage {
  EscPos,
  StarPrnt,
  StarLine,
  EposXml,
  Zpl,           // Zebra Programming Language (Link-OS)
  Cpcl,          // Comtec/Zebra mobile, also documented by Citizen CMP portables
  BrotherRaster, // Brother's own raster command reference
  EscP,          // Brother ESC/P (not Epson ESC/POS — a different language entirely)
};

// Several devices document more than one language on the same hardware: the Citizen
// CMP-20II/30II publish ESC/POS, CPCL and ZPL2 command references for one printer
// (docs/compatibility-brief.md §11-12), and a Zebra ZQ630 Plus documents three. Which
// languages exist is a fact about the device; which one this core drives is
// CapabilityProfile::language. Recording only the second would throw away the first.
struct CommandLanguages {
  bool esc_pos = false;
  bool star_prnt = false;
  bool star_line = false;
  bool epos_xml = false;
  bool zpl = false;
  bool cpcl = false;
  bool brother_raster = false;
  bool esc_p = false;

  void add(CommandLanguage language) noexcept;
  bool has(CommandLanguage language) const noexcept;
  size_t count() const noexcept;
};

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

// docs/compatibility-brief.md §25: **never `bluetooth = true`**. Five different things
// hide behind that boolean and they need five different stacks — a Classic SPP socket,
// a vendor protocol over Classic, a BLE GATT profile, Apple's MFi/ExternalAccessory
// channel, and "the vendor SDK is the only documented path". An Epson TM-P20II
// documents Classic *and* BLE (`TM-P20II-xxxxxx` and `TM-P20II-xxxxxx-L`); a Star
// SM-S230i is driven through Star's SDK; Citizen CMP sells an MFi variant; Zebra
// exposes separate Classic and BLE status connections.
struct BluetoothTransport {
  bool classic_spp = false;     // Bluetooth Classic, Serial Port Profile: a byte stream
  bool classic_vendor = false;  // Classic, but under the vendor's own framing
  bool ble = false;             // Bluetooth Low Energy, GATT
  bool mfi = false;             // Apple MFi / ExternalAccessory (iOS without BLE)
  bool vendor_sdk = false;      // the documented path is the SDK, not a raw socket

  // docs/compatibility-brief.md §6: the Epson portables document **4 KB of receive
  // buffer normally and 64 KB for Bluetooth** — same printer, different number by
  // path, which is exactly why pacing cannot be derived from a model name. 0 means the
  // manufacturer does not document it, which is not the same as "small".
  uint32_t receive_buffer_bytes = 0;
  uint32_t bluetooth_receive_buffer_bytes = 0;

  bool any() const noexcept {
    return classic_spp || classic_vendor || ble || mfi || vendor_sdk;
  }
};

// docs/device-database.md "Interface ≠ transport ≠ language".
struct TransportCapabilities {
  bool raw_tcp_9100 = true;
  bool serial = false;
  bool usb = false;
  bool wifi = false;
  bool epos = false;  // vendor HTTP/ePOS-Print endpoint
  // Faceted per §25 rather than a single flag. Reached through a custom transport
  // (see transport.hpp): the socket belongs to the platform, the protocol to the core.
  BluetoothTransport bluetooth;
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

  // docs/compatibility-brief.md §28: what each flag above is worth. Unverified is the
  // default everywhere, so a family added without evidence claims nothing. Epson is
  // the only family whose shipped defaults carry Documented for the process-ID echo —
  // its command tables list `GS ( H` fn 48 per model. Xprinter's XP-S260M answers it
  // on our bench but Xprinter does not document it, so the *default* is Unverified and
  // the probe writes Probed. Rongta advertises ESC/POS and OPOS with no manufacturer-
  // hosted manual proving the extension: Unverified, corrected from an earlier
  // assumption. Partner Tech is probe-only across the board (§15).
  Provenance process_id_gs_h_provenance = Provenance::Unverified;
  Provenance queued_gs_r_provenance = Provenance::Unverified;
  Provenance vendor_idle_provenance = Provenance::Unverified;
  Provenance epos_job_id_provenance = Provenance::Unverified;
};

struct StatusCapabilities {
  bool dle_eot = true;        // real-time DLE EOT 1-4 answers come back at all
  bool asb = true;            // GS a automatic status back
  bool extended_asb = false;  // FS ( e, optional/extended devices
  bool cutter_error = true;   // DLE EOT 3 bit 3 is meaningful on this model

  // Same rule as above. `DLE EOT` and ASB are in Epson's documentation for the listed
  // TM models; everywhere else they are a default until something establishes them.
  Provenance dle_eot_provenance = Provenance::Unverified;
  Provenance asb_provenance = Provenance::Unverified;
  Provenance extended_asb_provenance = Provenance::Unverified;
  Provenance cutter_error_provenance = Provenance::Unverified;
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
  // The image, not the roll. docs/compatibility-brief.md §18: a CT-S4500 takes 112 mm
  // media and prints 104 mm, and a renderer that derives one from the other clips every
  // wide receipt.
  uint16_t printable_width_mm = 72;
  uint32_t printable_width_dots = escpos::kWidth80mm;
  uint16_t dpi = 203;
  bool paper_guide_58mm = false;
  // What the same mechanism prints once the 58 mm paper guide is fitted — a separate
  // documented number, not a proportion of the 80 mm figure: a TM-T20III prints 576
  // dots on 80 mm and 420 on 58 mm (§2, §18), where scaling would predict 417. 0 means
  // the model has no documented 58 mm configuration.
  uint32_t printable_width_dots_58mm = 0;
  bool black_mark_sensor = false;
  bool gap_sensor = false;
  bool near_end_sensor = false;
  bool paper_end_sensor = true;
  bool cover_sensor = true;
  bool cutter = true;
  bool full_cut = true;
  bool partial_cut = true;

  // Distance from the print head to the cutter blade, in dots at this profile's dpi:
  // the head sits ahead of the blade, so content printed right up to the cut can be
  // sliced by the mechanism that is supposed to free it (docs/testing-plan.md — an
  // XP-S260M soak found the last ~15% of a trailing QR cut off). The engine always
  // feeds this many dots immediately before every cut, on top of final_feed_lines.
  // 120 is the conservative default for anything not individually calibrated.
  uint16_t head_to_cutter_feed_dots = 120;
};

struct CapabilityProfile {
  std::string name = "generic-escpos";

  DeviceIdentity identity;
  // The language this profile is driven in. Only EscPos is implemented.
  CommandLanguage language = CommandLanguage::EscPos;
  // Every language the manufacturer documents for the same hardware. Set together with
  // `language` through setLanguage(); a device with several gets the extra ones added.
  CommandLanguages languages;
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
  // that are first-class rather than ESC/POS-emulated, whose entries exist as data,
  // and false for ZPL/CPCL/Brother-raster devices, which are not ESC/POS at any level.
  bool drivableByEscposEngine() const noexcept;

  // Sets the drive language and records it in the documented set in one step, so the
  // two can never disagree.
  void setLanguage(CommandLanguage primary) noexcept;
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

// Declaration order, for wrapper generators and the bridge tests that enumerate
// members without a hand-maintained list (same contract as types.hpp's kAll* arrays).
constexpr std::array<CommandLanguage, 8> kAllCommandLanguages{
    CommandLanguage::EscPos, CommandLanguage::StarPrnt,
    CommandLanguage::StarLine, CommandLanguage::EposXml,
    CommandLanguage::Zpl, CommandLanguage::Cpcl,
    CommandLanguage::BrotherRaster, CommandLanguage::EscP,
};

const char* to_string(CompletionMechanism) noexcept;
const char* to_string(CutVariant) noexcept;
const char* to_string(ResponseParserVariant) noexcept;
const char* to_string(CommandLanguage) noexcept;

}  // namespace pd
