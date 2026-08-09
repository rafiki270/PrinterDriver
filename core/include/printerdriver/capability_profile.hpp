#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// M14 — the drawer facet below is a peripheral capability with its own header, not a
// field of the printer's own capabilities (docs/cash-drawer.md).
#include "printerdriver/cash_drawer.hpp"
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

  // --- M13b: Star raw completion (docs/wire-protocols.md §2) -----------------------
  //
  // Appended after None on purpose. These enumerators are mirrored into pd.h and four
  // wrappers with explicit values, so a new member may only ever be added at the end:
  // inserting one next to its conceptual neighbours would renumber every mirror.
  //
  // Both are the *raw* fences Star documents for a socket the SDK is not holding, which
  // is what makes them drivable here where StarCheckedBlock — an SDK call, not a wire
  // primitive — is not.
  StarEtb,        // 0x17 + ASB five-bit print-end counter (Line Mode Command Specs)
  StarEscGsEtx,   // 1B 1D 03 01 n1 n2 -> echo + eight-bit counter, session-scoped
};

// Whether the core's ESC/POS engine can actually drive this mechanism over a raw byte
// transport. The other mechanisms are carried so a profile can describe the hardware
// honestly; pointing this engine at one fails Unsupported rather than pretending.
bool isDrivableByEscposEngine(CompletionMechanism) noexcept;

// M13b: the same question for the Star engine (docs/wire-protocols.md §2). True for the
// two raw fences and for None; false for StarCheckedBlock, which is an SDK call rather
// than a wire primitive and stays profile data.
bool isDrivableByStarEngine(CompletionMechanism) noexcept;

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

  // --- M13b (docs/wire-protocols.md §4) --------------------------------------------
  //
  // The MFi ExternalAccessory protocol string, i.e. the exact value an iOS app puts in
  // UISupportedExternalAccessoryProtocols. Recorded as a *profile fact* rather than
  // derived, because it cannot be derived: `mfi = true` says a Classic accessory channel
  // exists, and the string says which one, and getting it wrong means EASession never
  // opens. Empty means "not recorded", which covers two different situations that the
  // fields below keep apart.
  std::string mfi_protocol;
  // Citizen's string is vendor-gated: it is issued only through MFi registration and
  // approval, so there is nothing legitimate to put in mfi_protocol. This flag exists so
  // "we have not looked it up" and "it exists and we are not allowed to know it yet"
  // stop being the same empty string — the second is a blocked integration with a known
  // next step, not a gap in the database.
  bool mfi_protocol_vendor_gated = false;
  // docs/wire-protocols.md §4: Epson, Star and Bixolon publish no raw GATT map. Their
  // BLE path is the vendor SDK's dedicated profile, and a generic UART probe that finds
  // an FFE1 characteristic on one of them has found a coincidence, not a printer
  // interface. True means: never silently map this device onto a generic BLE-UART
  // profile, whatever a scan turns up.
  bool ble_profile_unknown = false;

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

// --- M13b: Star and ePOS facets (docs/wire-protocols.md §1-§2) ----------------------
//
// Separate facets rather than more booleans on CompletionCapabilities, for the reason
// the whole profile is compositional: what a Star device answers over a raw socket and
// what an Epson device's print *service* offers are two unrelated bodies of evidence,
// each with its own provenance, and neither is implied by the ESC/POS-shaped fields.

struct StarCapabilities {
  // docs/wire-protocols.md §2. The ETB fence (0x17) waits for all preceding printing and
  // increments a five-bit counter reported through ASB. It is real and it is documented
  // — and on TCP 9100 the ASB frame carrying it is **broadcast to every connected host**,
  // so two clients on one printer can each read the other's completion as their own.
  bool etb_counter = false;
  // ESC GS ETX (1B 1D 03 01 n1 n2). Star's preferred Ethernet fence: it also waits for
  // prior printing and motor activity, carries an eight-bit print-end counter, echoes
  // back the correlation bytes it was given, and **replies only to the issuing session**.
  // That last property is the whole reason it is the default here.
  bool esc_gs_etx = false;
  // Whether this driver is the only thing holding a 9100 session to this printer. ETB is
  // permitted only when this is true, because ASB misattribution is not a risk that can
  // be detected after the fact from inside one client — the frame looks identical either
  // way. Configuration, not a device property, which is why it lives beside the fences it
  // gates rather than in CompletionCapabilities.
  bool exclusive_single_session = false;
  // Bytes in one ASB block. The counter lives at offset 7 ("printer status 6"), which
  // only means anything against a known block length, so the length is carried as data
  // per model instead of being guessed from the stream.
  uint8_t asb_block_bytes = 8;
  // Star raster mode (ESC * r A ... b n1 n2 <row> ... ESC * r B). Line-mode text is the
  // verified receipt path; raster is the documented-provisional one, so a profile can
  // decline it and get an honest Unsupported instead of an unverified image command.
  bool raster_line_mode = true;

  Provenance etb_provenance = Provenance::Unverified;
  Provenance esc_gs_etx_provenance = Provenance::Unverified;
};

struct EposCapabilities {
  // docs/wire-protocols.md §1: **spooling is a device setting**, configured through
  // WebConfig / EpsonNet Config / the setup utility — never a request attribute, and
  // never inferable from "OmniLink" in a product name. This flag records the documented
  // model matrix (TM-i fw 4.1+ yes, TM-DT2 yes, TM-T88VI-iHUB yes, plain network
  // TM-T88VI yes; plain TM-m10/m30/m30II NO, plain TM-T88VII NO), and it decides which
  // of two completely different completion stories the client tells: with a spooler the
  // first success=true is an *enqueue acknowledgement*, and without one the submission
  // does not return until the paper has moved.
  bool spooler = false;
  // JobIDs need ePOS-Print Service 4.1 or newer. Without it the service assigns its own
  // and there is nothing durable to poll with, so the retrieval half of the mechanism
  // simply is not there.
  bool job_id = false;
  std::string device_id = "local_printer";
  // Request timeout in milliseconds: default 60 000, capped at 300 000 by the service
  // (ePOS-Print XML User's Manual rev. AC).
  uint32_t timeout_ms = 60000;

  Provenance spooler_provenance = Provenance::Unverified;
};

// --- M15 (docs/api.md §15, docs/receipt-dsl.md "Degradation rules") -----------------
//
// Which of the *drawing* commands the document renderer emits this firmware actually
// implements. A separate facet from completion and status for the usual reason: whether
// a device can draw a Code 128 symbol is unrelated to whether it can fence a print, and
// the two are established from different evidence.
//
// All three default true because every ESC/POS family in this database documents them.
// They exist so that a profile can decline one and get a **declared degradation on the
// paper** — "BARCODE not supported on this path" — instead of a GS k command the
// firmware prints as literal text across half a receipt. Star line mode is the clearest
// real case: it has no GS k and no GS ( k at all.
struct RenderCapabilities {
  bool barcode_gs_k = true;    // GS k function B (docs/receipt-dsl.md `barcode`)
  bool qr_gs_paren_k = true;   // GS ( k fn 165/167/169/180/181
  bool raster_gs_v = true;     // GS v 0
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
  // M16 (docs/api.md §16). When `completion` is CompletionMechanism::VendorIdle this names
  // the registered completion method that drives it: a namespaced id like "acme.x-idle",
  // looked up in the driver's per-instance registry. Empty on every built-in profile, and
  // ignored for every other mechanism.
  std::string completion_vendor_id;
  CompletionCapabilities completion_caps;
  StatusCapabilities status;
  RecoveryCapabilities recovery;
  Quirks quirks;
  MediaProfile media;
  // M15 — what the document renderer may draw on this path.
  RenderCapabilities render;

  // --- M13b (docs/wire-protocols.md §1-§2) -----------------------------------------
  StarCapabilities star;
  EposCapabilities epos;

  // --- M14: cash drawer (docs/cash-drawer.md) ------------------------------------
  // A peripheral facet, deliberately a sibling of media/status/completion rather than
  // a member of any of them: the drawer has its own electrical profile, its own
  // command method and its own feedback method, and none of the three is derivable
  // from anything the printer does with paper. Default-constructed means "no drawer
  // port has been established on this model", which refuses a pulse rather than
  // guessing a pinout.
  DrawerCapabilities drawer;
  // --- end M14 -------------------------------------------------------------------

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

  // --- M13b -------------------------------------------------------------------------
  // Star Line Mode / StarPRNT over a raw socket, fenced by ETB or ESC GS ETX
  // (docs/wire-protocols.md §2). A separate question from the ESC/POS one: a Star
  // printer is not an ESC/POS device with a different dialect, it is a different
  // command language whose completion primitive happens to be equally documented.
  bool drivableByStarEngine() const noexcept;
  // Either engine. What the job path actually asks before refusing Unsupported.
  bool drivable() const noexcept;

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
