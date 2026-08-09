#include "printerdriver/device_profiles.hpp"

#include <algorithm>

namespace pd::devices {
namespace {

// 80 mm media, 72 mm printable, 576 dots at 203 dpi — the geometry shared by most of
// the database. Recorded as three separate facts, never derived from each other
// (docs/compatibility-brief.md §18).
CapabilityProfile thermal80() {
  CapabilityProfile profile;
  profile.setLanguage(CommandLanguage::EscPos);
  profile.completion = CompletionMechanism::GsR1;
  profile.completion_caps.queued_gs_r = true;
  profile.cut = CutVariant::Partial;
  profile.media.nominal_roll_width_mm = 80;
  profile.media.printable_width_mm = 72;
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
  profile.media.printable_width_mm = 48;
  profile.media.printable_width_dots = escpos::kWidth58mm;
  profile.media.printable_width_dots_58mm = 0;  // this *is* the 58 mm configuration
  profile.media.paper_guide_58mm = false;
  profile.media.full_cut = false;
}

// A handheld/belt printer: no autocutter, a tear bar, a battery, and a Bluetooth link
// whose facets the caller must state explicitly (§25).
void makePortable(CapabilityProfile& profile) {
  profile.media.cutter = false;
  profile.media.full_cut = false;
  profile.media.partial_cut = false;
  profile.cut = CutVariant::None;
  profile.media.head_to_cutter_feed_dots = 0;  // nothing to clear: the operator tears
  profile.transport.raw_tcp_9100 = false;
  profile.transport.usb = true;
  // M14. A belt or handheld printer has no drawer port, so whatever the counter-top
  // family default said about one does not survive into it (docs/cash-drawer.md is
  // about static POS installations throughout). Cleared rather than left inherited:
  // a present=true here would let the engine energise an output that does not exist.
  profile.drawer = DrawerCapabilities{};
}

// --- M14: cash-drawer seeds (docs/cash-drawer.md) -----------------------------------
//
// Two facts per family, always kept apart: what the *port* is (pinout, voltage,
// current, channels, sense pin) and what the *commands* are. The doc's closing
// paragraph is explicit that the two are independent — two printers accepting the same
// "kick drawer 1" command may still wire the modular socket differently — and every
// helper below sets them separately so no entry can accidentally inherit one from the
// other.

// The de-facto Epson arrangement: 1 FG, 2 kick 1, 3 open/close input, 4 +24 V, 5 kick
// 2, 6 signal ground; solenoid between +24 V and a drive pin; >= 24 ohm, ~<= 1 A. This
// is the large interoperable ecosystem of the doc's electrical-classification section.
void makeEpsonStyleDrawerPort(CapabilityProfile& profile, Provenance electrical) {
  DrawerCapabilities& drawer = profile.drawer;
  drawer.present = true;
  drawer.electrical.standard = DrawerPortStandard::Epson24V6P6C;
  drawer.electrical.voltage = 24;
  drawer.electrical.max_current_ma = 1000;
  drawer.electrical.channel_count = 2;
  drawer.electrical.sensor_pin = 3;
  // "Two outputs, ONE switch input": both channels kick independently and the only
  // readable fact is that *some* attached drawer is open.
  drawer.status.shared_between_drawers = true;
  drawer.evidence.electrical = electrical;
}

// ESC p plus GS r 2, the reference implementation of docs/cash-drawer.md §1.
void makeEscPosDrawerCommands(CapabilityProfile& profile, DrawerKickMethod method,
                              Provenance commands) {
  DrawerCapabilities& drawer = profile.drawer;
  drawer.kick.method = method;
  // 200 ms is the doc's default for known 24 V printer-driven drawers.
  drawer.kick.default_pulse_ms = 200;
  // ESC p programs t1 in 2 ms units, so 255 units is 510 ms; 500 is that rounded to a
  // number an operator would recognise, and it is a ceiling rather than a target.
  drawer.kick.max_pulse_ms = 500;
  // Not a manufacturer figure. A driver-side floor so a retry loop cannot hold a 24 V
  // solenoid energised; the families that publish a duty limit of their own override it.
  drawer.kick.cooldown_ms = 500;
  drawer.kick.can_kick_during_print = true;
  drawer.status.available = commands == Provenance::Documented;
  drawer.status.method =
      drawer.status.available ? DrawerStatusMethod::GsR2 : DrawerStatusMethod::None;
  drawer.evidence.commands = commands;
}

// A port that exists and has not been classified. Nothing is fired on it: kickable()
// refuses on an Unknown standard, which is the giant-letters rule of the document.
void makeUnclassifiedDrawerPort(CapabilityProfile& profile, std::string note) {
  DrawerCapabilities& drawer = profile.drawer;
  drawer = DrawerCapabilities{};
  drawer.present = true;
  drawer.electrical.standard = DrawerPortStandard::Unknown;
  drawer.kick.method = DrawerKickMethod::Unsupported;
  drawer.note = std::move(note);
}
// --- end M14 -------------------------------------------------------------------------

CapabilityProfile epsonBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Epson";
  profile.completion = CompletionMechanism::GsParenH;
  profile.completion_caps.process_id_gs_h = true;
  profile.completion_caps.try_process_id_gs_h = true;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  // docs/compatibility-brief.md §2 and §28. Epson publishes a model-by-model ESC/POS
  // applicability database — not the phrase "ESC/POS compatible" — and `GS ( H` fn 48
  // appears in it by name ("Specifies the process ID response"), alongside `DLE EOT`,
  // `GS r` and ASB. This is the ONLY vendor in this database whose shipped defaults
  // carry Documented, and the reason the brief calls Epson the reference
  // implementation. A probe still matters: it verifies the *interface path* passes the
  // response back, which documentation cannot establish.
  profile.completion_caps.process_id_gs_h_provenance = Provenance::Documented;
  profile.completion_caps.queued_gs_r_provenance = Provenance::Documented;
  profile.status.dle_eot_provenance = Provenance::Documented;
  profile.status.asb_provenance = Provenance::Documented;
  profile.status.cutter_error_provenance = Provenance::Documented;
  profile.recovery.dle_enq_resume = true;
  profile.recovery.dle_enq_clear = true;
  profile.recovery.clear_buffers = true;  // DLE DC4 fn 8
  profile.transport.usb = true;
  profile.transport.serial = true;
  // --- M13b (docs/wire-protocols.md §4) --------------------------------------------
  // The MFi ExternalAccessory protocol string, recorded once for the family. It is the
  // exact value an iOS app puts in UISupportedExternalAccessoryProtocols, and there is no
  // way to derive it — an EASession opened with the wrong string simply never opens.
  profile.transport.bluetooth.mfi_protocol = "com.epson.escpos";
  // Epson publishes no raw GATT map: the TM-P20II's BLE path is the ePOS SDK's dedicated
  // profile. A generic BLE-UART scan that finds an FFE1 characteristic on one of these
  // has found a coincidence, and mapping onto it would be the silent substitution §4
  // forbids.
  profile.transport.bluetooth.ble_profile_unknown = true;
  profile.final_feed_lines = 3;
  // Mid-range of the TM family's documented 10-15 mm head-to-cutter gap at 203 dpi;
  // tighten per model once a unit is actually probed.
  profile.media.head_to_cutter_feed_dots = 100;
  // M14, docs/cash-drawer.md §1 — the reference implementation, and the only family
  // here whose drawer is documented end to end: the DK connector pinout, `ESC p m t1
  // t2` in the model-specific command tables, and `GS r 2` / `GS r 50` for the
  // kick-out connector status, with ASB able to report a change without being polled.
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  makeEscPosDrawerCommands(profile, DrawerKickMethod::EpsonEscP, Provenance::Documented);
  // "Buzzer conflict": Epson documents that with the optional external buzzer enabled,
  // the pulse that would go to the drawer connector sounds the buzzer instead.
  profile.drawer.port.shared_with_buzzer = true;
  return profile;
}

// The desktop TM geometry: 576 dots on 80 mm paper, 420 once the 58 mm guide is
// fitted. 420 is Epson's own number and is not 384 and not 576 × 58/80 — which is the
// entire reason it is stored rather than computed (§2, §18).
void makeEpsonDualWidth(CapabilityProfile& profile) {
  profile.media.paper_guide_58mm = true;
  profile.media.printable_width_dots_58mm = 420;
}

// Epson's portable Bluetooth facts, identical across the P-series units the brief
// details: Bluetooth 5.0 Dual Mode means Classic *and* BLE, and the receive buffer is
// 4 KB normally but 64 KB over Bluetooth (§6). Both numbers are documented; neither is
// derivable from the other.
void makeEpsonPortableBluetooth(CapabilityProfile& profile) {
  profile.transport.bluetooth.classic_spp = true;
  profile.transport.bluetooth.ble = true;
  profile.transport.bluetooth.receive_buffer_bytes = 4 * 1024;
  profile.transport.bluetooth.bluetooth_receive_buffer_bytes = 64 * 1024;
  // The Wi-Fi configuration of the same model is a different machine with a different
  // reachable surface, and raw 9100 is part of it. makePortable() cleared the flag
  // because a Bluetooth-only handheld has no socket; a Wi-Fi unit does.
  profile.transport.wifi = true;
  profile.transport.raw_tcp_9100 = true;
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
  // docs/compatibility-brief.md §13, THE CORRECTION. Rongta advertises ESC/POS and
  // OPOS, and both are documented. `GS ( H` fn 48 is not: no currently
  // manufacturer-hosted Rongta command reference proving it was found, and an earlier
  // revision of this database wrongly assumed it from a third-party copy of an RP80
  // manual. Everything here is therefore Unverified until a probe says otherwise —
  // including DLE EOT and ASB, which are equally unproven from Rongta's own material.
  // try_process_id_gs_h stays true: the probe should still ask.
  profile.completion_caps.try_process_id_gs_h = true;
  // The fleet quirk from docs/sdk-spec.md §9: these cut into the last printed line,
  // which the deployed stack compensates for with six feeds before the cut. CP852 is
  // the code page those installations run.
  profile.final_feed_lines = 6;
  profile.code_page = escpos::CodePage::PC852;
  // M14, docs/cash-drawer.md §7-9. The RP336S-class 80 mm units carry a 6-wire socket
  // at 24 V / 1 A, model-verified — so the *port* is established for the 80 mm family
  // and the family default says so. The *commands* are not: no trusted
  // manufacturer-hosted reference gives `ESC p` plus switch semantics across the
  // range, so they stay Unverified and `pdctl drawer test` is what establishes them.
  // rongta_rp58() below throws all of this away rather than inheriting it.
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  makeEscPosDrawerCommands(profile, DrawerKickMethod::EpsonEscP, Provenance::Unverified);
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
  // §14: "Public documentation does not prove GS ( H." Everything stays Unverified in
  // the shipped defaults even for the S-series, where our own hardware answers it —
  // that finding belongs to a probe result, not to a family default.
  profile.code_page = escpos::CodePage::PC852;
  // M14, docs/cash-drawer.md §6, the sentence in bold: several Xprinter 58 mm models
  // officially specify **12 V / 1 A** while 80 mm units are commonly 24 V. So
  // "Xprinter = 24 V" is never a family fact, and the family default classifies the
  // port as Unknown — which refuses a pulse. xprinter_s_series() below overrides it
  // with the XP-S260M's own published figure.
  makeUnclassifiedDrawerPort(
      profile,
      "Drawer voltage is a MODEL fact on this brand: several 58 mm models specify "
      "12 V / 1 A while 80 mm units are commonly 24 V. Identify the model and its "
      "published drawer output before connecting a drawer.");
  return profile;
}

CapabilityProfile starBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Star Micronics";
  // StarPRNT, not ESC/POS: beginCheckedBlock()/endCheckedBlock() is the documented way
  // to learn whether the whole data printed, and this core does not speak it. Profile
  // data only — the engine refuses these jobs rather than driving Star's ESC/POS
  // emulation and reporting a fence it never really got (§7-§8).
  profile.setLanguage(CommandLanguage::StarPrnt);
  profile.completion = CompletionMechanism::StarCheckedBlock;
  profile.completion_caps.queued_gs_r = false;
  profile.completion_caps.prefer_vendor_sdk = true;
  // The checked block is Star's own documented API, so the mechanism is Documented even
  // though this engine cannot drive it. Documentation and drivability are different
  // questions and the database answers both.
  profile.completion_caps.vendor_idle_provenance = Provenance::Documented;
  profile.status.dle_eot = false;
  profile.status.asb = false;
  profile.status.cutter_error = false;
  profile.quirks.response_parser = ResponseParserVariant::StarPrnt;
  profile.transport.usb = true;
  // --- M13b (docs/wire-protocols.md §2, §4) ----------------------------------------
  // Both raw fences are documented in Star's own Line Mode Command Specifications, which
  // is a different fact from whether this profile *selects* one: starRawBase() below does
  // that for the models where a raw socket is the deployed path, and the SDK-first
  // portables keep the checked block. Recording the capability on the shared base keeps
  // "the printer can do this" and "we drive it this way" apart, which is the distinction
  // docs/compatibility-brief.md §28 exists to protect.
  profile.star.etb_counter = true;
  profile.star.esc_gs_etx = true;
  profile.star.etb_provenance = Provenance::Documented;
  profile.star.esc_gs_etx_provenance = Provenance::Documented;
  profile.transport.bluetooth.mfi_protocol = "jp.star-m.starpro";
  // Star's SDK addresses BLE devices by `BLE:<device>` name; the GATT map is hidden.
  profile.transport.bluetooth.ble_profile_unknown = true;

  // M14, docs/cash-drawer.md §4 — SAME-LOOKING CONNECTOR, DIFFERENT ELECTRICS. The
  // TSP100 hardware manual gives 1 FG, 2 DRD1, **3 = +24 V**, 4 = +24 V, 5 DRD2,
  // **6 = DRSNS sense**: Epson puts sense on 3 and ground on 6, Star puts +24 V on 3
  // and sense on 6. A cable that fits is not electrically correct, which is why APG
  // sells printer-specific cables for identical-looking plugs.
  //
  // The software side is Star's own appendPeripheral(...) and drawerOpenCloseSignal,
  // not `ESC p` and not `GS r 2` — documented, and not something this ESC/POS engine
  // may drive. So the port is classified (a cable can be chosen correctly) while the
  // kick method refuses: a Star drawer is driven through StarPRNT or not at all. This
  // holds for the raw-socket models of starRawBase() too: driving Line Mode ourselves
  // (M13b) buys a print fence, not a peripheral port, so they inherit this unchanged.
  DrawerCapabilities& drawer = profile.drawer;
  drawer.present = true;
  drawer.electrical.standard = DrawerPortStandard::Star24V6P6C;
  drawer.electrical.voltage = 24;
  drawer.electrical.max_current_ma = 1000;
  drawer.electrical.channel_count = 2;
  drawer.electrical.sensor_pin = 6;
  drawer.kick.method = DrawerKickMethod::StarPrnt;
  drawer.kick.default_pulse_ms = 200;
  drawer.kick.max_pulse_ms = 500;
  drawer.kick.cooldown_ms = 500;
  drawer.status.available = true;
  drawer.status.method = DrawerStatusMethod::StarSignal;
  drawer.status.shared_between_drawers = false;  // drawer1/drawer2 signals are separate
  drawer.port.shared_with_buzzer = true;         // the external-device port is shared
  drawer.evidence.electrical = Provenance::Documented;
  drawer.evidence.commands = Provenance::Documented;
  drawer.note =
      "Star pinout: +24 V on pin 3, sense on pin 6 — the reverse of the Epson "
      "arrangement. Use a Star-specific drawer cable. The kick is StarPRNT "
      "appendPeripheral and the sense line is drawerOpenCloseSignal, neither of which "
      "this ESC/POS engine speaks; the true/false meaning of the signal still depends "
      "on the attached drawer, so calibrate before trusting it.";
  return profile;
}

// M13b. The Star models this core drives itself over a raw socket
// (docs/wire-protocols.md §2). Until this milestone every Star entry was refused with
// FailureReason::Unsupported before a byte was written, which was the honest answer while
// the only documented mechanism was an SDK call. It is no longer the whole truth: ESC GS
// ETX is a documented wire primitive, it waits for prior printing and motor activity, it
// carries a print-end counter, and it **replies only to the issuing session** — so a
// receipt on one of these printers now earns grade A with the physical printer as the
// authority, exactly like a GS ( H echo on an Epson.
//
// ESC GS ETX and not ETB, on every one of them. ETB is equally documented and equally
// real, but its counter arrives inside an ASB frame that TCP 9100 broadcasts to every
// connected host, so on a shared port it cannot say *whose* data finished. Selecting it
// is therefore a deliberate act by a deployment that owns the session
// (StarCapabilities::exclusive_single_session), never a shipped default.
CapabilityProfile starRawBase() {
  CapabilityProfile profile = starBase();
  // Driven in Line Mode, which is what star::Encoder emits and what the ETB/ESC GS ETX
  // specifications describe. StarPRNT stays in the documented set because the hardware
  // documents it too; the drive language is the narrower, verified claim.
  profile.setLanguage(CommandLanguage::StarLine);
  profile.languages.add(CommandLanguage::StarPrnt);
  profile.completion = CompletionMechanism::StarEscGsEtx;
  profile.star.exclusive_single_session = false;
  profile.star.raster_line_mode = true;
  profile.star.asb_block_bytes = 8;
  return profile;
}

CapabilityProfile bixolonBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Bixolon";
  // §9-§10: "ESC/POS compatible" is a marketing statement, not a command table. The
  // vendor status SDK is the documented path; DLE EOT and GS r are probe-or-document
  // per model; GS ( H is never assumed from the phrase.
  profile.completion_caps.prefer_vendor_sdk = true;
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.quirks.unreliable_identity = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  // M13b (docs/wire-protocols.md §4): the published MFi protocol string. The SPP-R310 is
  // dual-mode BLE plus MFi iAP2 with no public GATT map, so the BLE side stays unknown
  // rather than being mapped onto a generic UART profile.
  profile.transport.bluetooth.mfi_protocol = "com.bixolon.protocol";
  profile.transport.bluetooth.ble_profile_unknown = true;

  // M14, docs/cash-drawer.md §2. The SRP-350V/352V pinout matches Epson's layout
  // (1 FG, 2 drive 1, 3 open/close input, 4 +24 V, 5 drive 2, 6 signal ground) at
  // >= 24 ohm / max 1 A with documented pulse and recovery limits — so the port is
  // documented. The command surface is the WebPrint SDK's makeDKout({connector,
  // duration}), whose own default is pin 2 at 200 ms; that is a vendor path this
  // engine does not drive, so no pulse is emitted from here. Per-model families that
  // do not share the 350's manual override the port back to Unknown below.
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  profile.drawer.kick.method = DrawerKickMethod::BixolonSdk;
  profile.drawer.kick.default_pulse_ms = 200;  // the SDK's own default
  profile.drawer.kick.max_pulse_ms = 500;      // makeDKout's documented ladder tops out here
  profile.drawer.kick.cooldown_ms = 500;
  profile.drawer.status.available = true;
  profile.drawer.status.method = DrawerStatusMethod::VendorSdk;
  profile.drawer.evidence.commands = Provenance::Documented;
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
  // M13b (docs/wire-protocols.md §4): Citizen's MFi protocol string is **vendor-gated** —
  // issued only through MFi registration and approval — so there is nothing legitimate to
  // record. The flag says exactly that, which is a different statement from "nobody
  // looked it up": this is a blocked integration with a known next step, and guessing a
  // string here would produce an EASession that never opens and a support case nobody can
  // reproduce.
  profile.transport.bluetooth.mfi_protocol_vendor_gated = true;

  // M14, docs/cash-drawer.md §3. Citizen's common command reference explicitly lists
  // `ESC p` and `GS r 2` / `GS r 50`, and the CT-S4500 pinout is the Epson-like one
  // (1 FG, 2 DRAWER1, 3 DRSW switch input, 4 VDR, 5 DRAWER2, 6 GND) at 24 V, >= 24
  // ohm, ~<= 1 A. Documented on both halves.
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  makeEscPosDrawerCommands(profile, DrawerKickMethod::CitizenEscP,
                           Provenance::Documented);
  // The serialisation quirk, and the reason this flag exists at all: on the affected
  // models the drawer output cannot fire while the mechanism is printing, so the pulse
  // is ordered strictly behind everything already queued instead of jumping it.
  profile.drawer.kick.can_kick_during_print = false;
  profile.drawer.note =
      "Drawer 1 and drawer 2 cannot be driven simultaneously, and on the affected "
      "models the drawer output cannot fire while printing: the pulse is serialised "
      "behind receipt completion.";
  return profile;
}

// M14, docs/cash-drawer.md §5. SNBC / New Beiyang BTP-R880NP: a 6-position modular
// drawer interface at 24 V with ~<= 1 A drive, two outputs and a switch input, and a
// programming manual that documents `ESC p` alongside the realtime `DLE DC4 n m t`
// pulse. Genuinely documented on both halves. The queued `ESC p` is preferred over the
// realtime variant in normal operation, which is what this engine emits.
CapabilityProfile snbcBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "SNBC";
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  profile.completion_caps.try_process_id_gs_h = true;
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  makeEscPosDrawerCommands(profile, DrawerKickMethod::SnbcEscP, Provenance::Documented);
  return profile;
}

CapabilityProfile partnerBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Partner Tech";
  profile.status.dle_eot = true;
  profile.status.asb = true;
  profile.status.cutter_error = true;
  profile.recovery.dle_enq_resume = true;
  profile.recovery.dle_enq_clear = true;
  profile.transport.usb = true;
  profile.transport.serial = true;
  // §15: ESC/POS-compatible, with not enough public programmer documentation for the
  // Epson completion extensions. DLE EOT / GS r / GS ( H / ASB are ALL probe questions
  // here — every provenance below stays Unverified, which is the honest description of
  // a device whose manual nobody can read.
  profile.completion_caps.try_process_id_gs_h = true;
  // M14, docs/cash-drawer.md §7-9: Partner RP-110 is `cashDrawer = PROBE_REQUIRED`
  // across the board, and the doc is explicit that Sewoo's documented implementation
  // must not be auto-applied to it on OEM resemblance alone. So: a port that exists
  // and is unclassified, and no pulse until somebody establishes it on a unit.
  makeUnclassifiedDrawerPort(
      profile,
      "Probe required on every axis: connector pinout, drive voltage and command set "
      "are all unestablished on this family. OEM resemblance to another vendor's unit "
      "is not evidence about either the wiring or the firmware.");
  return profile;
}

// Zebra and Brother are not ESC/POS at any level (§16, §17). Their entries exist so a
// fleet can be inventoried and so that pointing this engine at one is a diagnosable
// refusal — FailureReason::Unsupported, zero bytes written — rather than a metre of
// garbage. Completion is None: not "no fence exists" (Zebra's Link-OS StatusConnection
// is a real, independent status channel) but "no fence this engine can reach".
CapabilityProfile zebraBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Zebra";
  profile.setLanguage(CommandLanguage::Zpl);
  profile.languages.add(CommandLanguage::Cpcl);
  profile.completion = CompletionMechanism::None;
  profile.completion_caps.queued_gs_r = false;
  profile.completion_caps.prefer_vendor_sdk = true;
  profile.status.dle_eot = false;
  profile.status.asb = false;
  profile.status.cutter_error = false;
  profile.quirks.response_parser = ResponseParserVariant::VendorRaw;
  profile.media.cutter = false;
  profile.media.full_cut = false;
  profile.media.partial_cut = false;
  profile.cut = CutVariant::None;
  profile.media.head_to_cutter_feed_dots = 0;
  profile.media.black_mark_sensor = true;
  profile.media.gap_sensor = true;  // label stock, not receipt paper
  profile.transport.raw_tcp_9100 = false;
  profile.transport.usb = true;
  // Link-OS BluetoothStatusConnection and BluetoothLeStatusConnection are separate
  // status-only channels that do not block the print channel — which is why both
  // facets are recorded rather than one "bluetooth" flag (§16, §25).
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.ble = true;
  profile.transport.bluetooth.vendor_sdk = true;
  profile.transport.wifi = true;
  // Link-OS exposes raw 9100 on the networked configurations, and the newer ones pair
  // Wi-Fi 6 with Bluetooth 5.3. Reachable is not the same as drivable: every job on
  // these profiles is still refused, because the bytes would be ESC/POS.
  profile.transport.raw_tcp_9100 = true;
  // M14. Mobile label printers, no drawer port, and not an ESC/POS device in any case:
  // present stays false and the method stays Unsupported, so a drawer call writes zero
  // bytes and claims nothing. Stated rather than inherited, because "we did not think
  // about it" and "there is nothing there" must not look the same in this database.
  profile.drawer = DrawerCapabilities{};
  return profile;
}

CapabilityProfile brotherBase() {
  CapabilityProfile profile = thermal80();
  profile.identity.vendor = "Brother";
  profile.setLanguage(CommandLanguage::BrotherRaster);
  profile.languages.add(CommandLanguage::EscP);
  profile.completion = CompletionMechanism::None;
  profile.completion_caps.queued_gs_r = false;
  profile.completion_caps.prefer_vendor_sdk = true;
  profile.status.dle_eot = false;
  profile.status.asb = false;
  profile.status.cutter_error = false;
  profile.quirks.response_parser = ResponseParserVariant::VendorRaw;
  profile.media.cutter = false;
  profile.media.full_cut = false;
  profile.media.partial_cut = false;
  profile.cut = CutVariant::None;
  profile.media.head_to_cutter_feed_dots = 0;
  profile.media.black_mark_sensor = true;
  profile.transport.raw_tcp_9100 = false;
  profile.transport.usb = true;
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.vendor_sdk = true;
  // M14. Same as Zebra: no drawer port, not ESC/POS, so a drawer call is a refusal
  // that writes nothing rather than a pulse nobody can account for.
  profile.drawer = DrawerCapabilities{};
  return profile;
}

struct Entry {
  const char* name;
  CapabilityProfile (*build)();
};

const std::vector<Entry>& table() {
  static const std::vector<Entry> entries{
      {"epson_tm_t20", &epson_tm_t20},
      {"epson_tm_t20ii", &epson_tm_t20ii},
      {"epson_tm_t20iii", &epson_tm_t20iii},
      {"epson_tm_t20iv", &epson_tm_t20iv},
      {"epson_tm_t70", &epson_tm_t70},
      {"epson_tm_t70ii", &epson_tm_t70ii},
      {"epson_tm_t82", &epson_tm_t82},
      {"epson_tm_t88", &epson_tm_t88},
      {"epson_tm_t88iv", &epson_tm_t88iv},
      {"epson_tm_t88v", &epson_tm_t88v},
      {"epson_tm_t88vi", &epson_tm_t88vi},
      {"epson_tm_t88vii", &epson_tm_t88vii},
      {"epson_tm_m", &epson_tm_m},
      {"epson_tm_m10", &epson_tm_m10},
      {"epson_tm_m30", &epson_tm_m30},
      {"epson_tm_m30ii", &epson_tm_m30ii},
      {"epson_tm_m30iii", &epson_tm_m30iii},
      {"epson_tm_m50", &epson_tm_m50},
      {"epson_tm_m50ii", &epson_tm_m50ii},
      {"epson_tm_p20", &epson_tm_p20},
      {"epson_tm_p20ii", &epson_tm_p20ii},
      {"epson_tm_p60", &epson_tm_p60},
      {"epson_tm_p60ii", &epson_tm_p60ii},
      {"epson_tm_p80", &epson_tm_p80},
      {"epson_tm_p80ii", &epson_tm_p80ii},
      {"epson_tm_p80ii_autocutter", &epson_tm_p80ii_autocutter},
      {"epson_tm_u220", &epson_tm_u220},
      {"epson_tm_u220ii", &epson_tm_u220ii},
      {"epson_tm_i", &epson_tm_i},
      {"rongta_rp58", &rongta_rp58},
      {"rongta_rp80", &rongta_rp80},
      {"rongta_rp3xx", &rongta_rp3xx},
      {"rongta_rp8xx", &rongta_rp8xx},
      {"xprinter_pos58", &xprinter_pos58},
      {"xprinter_pos80", &xprinter_pos80},
      {"xprinter_s_series", &xprinter_s_series},
      {"xprinter_v_series", &xprinter_v_series},
      {"xprinter_portable", &xprinter_portable},
      {"sewoo_slk_ts", &sewoo_slk_ts},
      {"partner_rp110", &partner_rp110},
      {"partner_rp710", &partner_rp710},
      // M14 — docs/cash-drawer.md §5.
      {"snbc_btp_r880", &snbc_btp_r880},
      {"snbc_btp_u80", &snbc_btp_u80},
      {"star_tsp100", &star_tsp100},
      {"star_tsp100iii", &star_tsp100iii},
      {"star_tsp100iv", &star_tsp100iv},
      {"star_tsp650", &star_tsp650},
      {"star_mcprint", &star_mcprint},
      {"star_mcprint2", &star_mcprint2},
      {"star_mcprint3", &star_mcprint3},
      {"star_sm_s210", &star_sm_s210},
      {"star_sm_s220", &star_sm_s220},
      {"star_sm_s230", &star_sm_s230},
      {"star_sm_l200", &star_sm_l200},
      {"star_sm_l300", &star_sm_l300},
      {"star_sm_t300", &star_sm_t300},
      {"star_sm_t400", &star_sm_t400},
      {"bixolon_srp330", &bixolon_srp330},
      {"bixolon_srp350", &bixolon_srp350},
      {"bixolon_srp380", &bixolon_srp380},
      {"bixolon_q_series", &bixolon_q_series},
      {"bixolon_srp_q200", &bixolon_srp_q200},
      {"bixolon_srp_q300", &bixolon_srp_q300},
      {"bixolon_srp_b300", &bixolon_srp_b300},
      {"bixolon_srp_f310", &bixolon_srp_f310},
      {"bixolon_srp_275", &bixolon_srp_275},
      {"bixolon_spp_r200", &bixolon_spp_r200},
      {"bixolon_spp_r210", &bixolon_spp_r210},
      {"bixolon_spp_r310", &bixolon_spp_r310},
      {"bixolon_spp_r410", &bixolon_spp_r410},
      {"citizen_cts_58_80", &citizen_cts_58_80},
      {"citizen_cts_fast", &citizen_cts_fast},
      {"citizen_cts_wide", &citizen_cts_wide},
      {"citizen_cmp_20ii", &citizen_cmp_20ii},
      {"citizen_cmp_30ii", &citizen_cmp_30ii},
      {"zebra_zq300_plus", &zebra_zq300_plus},
      {"zebra_zq500", &zebra_zq500},
      {"zebra_zq600_plus", &zebra_zq600_plus},
      {"brother_rj2000", &brother_rj2000},
      {"brother_rj3000", &brother_rj3000},
      {"brother_rj4000", &brother_rj4000},
      {"generic_80", &generic_80},
      {"generic_58", &generic_58},
      {"generic_unknown", &generic_unknown},
  };
  return entries;
}

}  // namespace

// --- Epson --------------------------------------------------------------------------

CapabilityProfile epson_tm_t20() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t20";
  profile.identity.model = "TM-T20 series";
  makeEpsonDualWidth(profile);
  return profile;
}

CapabilityProfile epson_tm_t20ii() {
  CapabilityProfile profile = epson_tm_t20();
  profile.name = "epson_tm_t20ii";
  profile.identity.model = "TM-T20II";
  return profile;
}

CapabilityProfile epson_tm_t20iii() {
  CapabilityProfile profile = epson_tm_t20();
  profile.name = "epson_tm_t20iii";
  profile.identity.model = "TM-T20III";
  // The reference unit of docs/compatibility-brief.md §2, verified against Epson's own
  // model command table: 203x203 dpi; 80 mm = 72 mm = 576 dots; 58 mm = 52.5 mm =
  // 420 dots; 250 mm/s; USB 2.0 Full Speed bidirectional bulk IN/OUT; ESC/POS plus
  // ePOS-Print XML. `GS ( H` fn 48 appears in that table by name, which is what makes
  // Documented the right provenance here and nowhere outside Epson.
  profile.transport.epos = true;
  profile.status.extended_asb = true;
  return profile;
}

CapabilityProfile epson_tm_t20iv() {
  CapabilityProfile profile = epson_tm_t20iii();
  profile.name = "epson_tm_t20iv";
  profile.identity.model = "TM-T20IV";
  return profile;
}

CapabilityProfile epson_tm_t70() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t70";
  profile.identity.model = "TM-T70 series";
  return profile;
}

CapabilityProfile epson_tm_t70ii() {
  CapabilityProfile profile = epson_tm_t70();
  profile.name = "epson_tm_t70ii";
  profile.identity.model = "TM-T70II";
  makeEpsonDualWidth(profile);
  profile.transport.epos = true;
  return profile;
}

CapabilityProfile epson_tm_t82() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t82";
  profile.identity.model = "TM-T82 series";
  makeEpsonDualWidth(profile);
  return profile;
}

CapabilityProfile epson_tm_t88() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_t88";
  profile.identity.model = "TM-T88 series";
  profile.status.extended_asb = true;  // FS ( e
  profile.media.near_end_sensor = true;
  makeEpsonDualWidth(profile);
  profile.transport.epos = true;
  return profile;
}

CapabilityProfile epson_tm_t88iv() {
  CapabilityProfile profile = epson_tm_t88();
  profile.name = "epson_tm_t88iv";
  profile.identity.model = "TM-T88IV";
  // The one Epson entry that does NOT claim GS ( H. §3 names the T88V command table,
  // the T88VI's inclusion in Epson's GS ( H applicability and the T88VII's coverage —
  // it does not extend either to this generation, which predates the extension. So the
  // fence drops to the queued GS r 1 that the same documentation does cover, and the
  // probe is left to promote a unit whose firmware turns out to answer.
  profile.completion = CompletionMechanism::GsR1;
  profile.completion_caps.process_id_gs_h = false;
  profile.completion_caps.try_process_id_gs_h = true;
  profile.completion_caps.process_id_gs_h_provenance = Provenance::Unverified;
  profile.status.extended_asb = false;
  profile.transport.epos = false;
  return profile;
}

CapabilityProfile epson_tm_t88v() {
  CapabilityProfile profile = epson_tm_t88();
  profile.name = "epson_tm_t88v";
  profile.identity.model = "TM-T88V";
  // Huge installed base, and Epson maintains an explicit command table for it (§3).
  return profile;
}

CapabilityProfile epson_tm_t88vi() {
  CapabilityProfile profile = epson_tm_t88();
  profile.name = "epson_tm_t88vi";
  profile.identity.model = "TM-T88VI";
  // §3: USB bulk bidirectional, dedicated current technical reference guide and command
  // table, explicitly included in Epson's `GS ( H` applicability, `GS ( E` serial/USB
  // configuration readback, and OmniLink documentation listing ESC/POS plus the
  // ePOS-Print Service.
  profile.transport.epos = true;
  profile.transport.wifi = true;
  // M13b (docs/wire-protocols.md §1): **plain network TM-T88VI has the spooler**, which
  // is worth stating loudly because its successor does not — see epson_tm_t88vii below.
  // Model number order is not capability order, and "OmniLink" in a product name is not
  // a capability proxy: the matrix is by exact model and firmware or it is worthless.
  profile.epos.spooler = true;
  profile.epos.job_id = true;
  profile.epos.spooler_provenance = Provenance::Documented;
  return profile;
}

CapabilityProfile epson_tm_t88vii() {
  CapabilityProfile profile = epson_tm_t88vi();
  profile.name = "epson_tm_t88vii";
  profile.identity.model = "TM-T88VII";
  // Up to 500 mm/s with the appropriate energising configuration; 450 mm/s standard
  // high speed, and 58 mm operation is limited to 450 (§3). Covered by `GS ( H`.
  // Nothing about speed changes the fence, but a faster mechanism finishes a long
  // ticket inside a shorter completion budget.
  profile.completion_timeout_ms = 12000;
  // M13b (docs/wire-protocols.md §1): **plain TM-T88VII has NO spooler**, unlike the
  // TM-T88VI it replaces. Recorded as Documented-absent rather than left at the Unverified
  // default, because "we checked and it is not there" is the fact that stops a caller
  // waiting for a JobID result that will never exist — the first success=true from this
  // model *is* the completion.
  profile.epos.spooler = false;
  profile.epos.job_id = false;
  profile.epos.spooler_provenance = Provenance::Documented;
  return profile;
}

CapabilityProfile epson_tm_m() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_m";
  profile.identity.model = "TM-m series";
  profile.status.extended_asb = true;
  makeEpsonDualWidth(profile);
  profile.transport.epos = true;
  profile.transport.wifi = true;
  profile.transport.bluetooth.classic_spp = true;
  profile.transport.bluetooth.ble = true;
  // M13b (docs/wire-protocols.md §1): the whole plain m-series — TM-m10, TM-m30, TM-m30II
  // — has **no spooler**. They speak ePOS-Print, and the submission does not return until
  // the data has printed, which is grade A rather than A+: strong, immediate, and gone the
  // moment the connection is lost.
  profile.epos.spooler = false;
  profile.epos.job_id = false;
  profile.epos.spooler_provenance = Provenance::Documented;
  return profile;
}

CapabilityProfile epson_tm_m10() {
  CapabilityProfile profile = epson_tm_m();
  profile.name = "epson_tm_m10";
  profile.identity.model = "TM-m10";
  // The 58 mm member of the mPOS family: this is its native width, not a guide
  // configuration of an 80 mm mechanism, so the dual-width numbers do not apply.
  makeFiftyEight(profile);
  profile.media.full_cut = false;
  return profile;
}

CapabilityProfile epson_tm_m30() {
  CapabilityProfile profile = epson_tm_m();
  profile.name = "epson_tm_m30";
  profile.identity.model = "TM-m30";
  return profile;
}

CapabilityProfile epson_tm_m30ii() {
  CapabilityProfile profile = epson_tm_m30();
  profile.name = "epson_tm_m30ii";
  profile.identity.model = "TM-m30II";
  return profile;
}

CapabilityProfile epson_tm_m30iii() {
  CapabilityProfile profile = epson_tm_m30ii();
  profile.name = "epson_tm_m30iii";
  profile.identity.model = "TM-m30III";
  // §4, from Epson's official specification: 80 and 58 mm rolls, maximum 83 mm roll
  // diameter, ~300 mm/s, USB-A, USB-B and USB-C with USB-PD, 10/100 Ethernet, Wi-Fi on
  // applicable configurations, **Bluetooth 5.0 Dual Mode on the Wi-Fi/BT config**,
  // microSD there, and a cash-drawer port. Which is precisely why interfaces are
  // represented separately from model identity: one model number, several machines.
  profile.transport.bluetooth.classic_spp = true;
  profile.transport.bluetooth.ble = true;
  profile.media.near_end_sensor = true;
  profile.completion_timeout_ms = 12000;
  return profile;
}

CapabilityProfile epson_tm_m50() {
  CapabilityProfile profile = epson_tm_m();
  profile.name = "epson_tm_m50";
  profile.identity.model = "TM-m50";
  profile.media.near_end_sensor = true;
  profile.completion_timeout_ms = 12000;
  return profile;
}

CapabilityProfile epson_tm_m50ii() {
  CapabilityProfile profile = epson_tm_m50();
  profile.name = "epson_tm_m50ii";
  profile.identity.model = "TM-m50II";
  return profile;
}

CapabilityProfile epson_tm_p20() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_p20";
  profile.identity.model = "TM-P20";
  makeFiftyEight(profile);
  makePortable(profile);
  makeEpsonPortableBluetooth(profile);
  profile.media.paper_end_sensor = true;
  profile.media.cover_sensor = true;
  return profile;
}

CapabilityProfile epson_tm_p20ii() {
  CapabilityProfile profile = epson_tm_p20();
  profile.name = "epson_tm_p20ii";
  profile.identity.model = "TM-P20II";
  // §6, from Epson's model-specific P20II command table: 58 mm paper, **48 mm print
  // width**, 203 dpi, manual tear, optical paper-end and cover detectors, USB-C;
  // Wi-Fi model 802.11 a/b/g/n/ac on 2.4 and 5 GHz; BT model **Bluetooth 5.0 Dual Mode
  // (Classic + BLE)**, Class 2, up to 8 stored pairings, **one simultaneous
  // connection**; **4 KB receive buffer normally, 64 KB for Bluetooth**; 384 KB NV
  // graphics; USB 2.0 FS bulk IN+OUT. Bluetooth naming is `TM-P20II-xxxxxx` for Classic
  // and `TM-P20II-xxxxxx-L` for BLE — two names, one printer, which is why §25 forbids
  // a single `bluetooth` boolean.
  profile.media.printable_width_mm = 48;
  profile.media.printable_width_dots = escpos::kWidth58mm;
  profile.completion_caps.one_job_in_flight = true;  // one simultaneous connection
  return profile;
}

CapabilityProfile epson_tm_p60() {
  CapabilityProfile profile = epson_tm_p20();
  profile.name = "epson_tm_p60";
  profile.identity.model = "TM-P60";
  // The brief details the P20II and P80II specifically; the P60 line is carried with
  // the family's portable defaults rather than invented facts. Manual tear, 58 mm.
  return profile;
}

CapabilityProfile epson_tm_p60ii() {
  CapabilityProfile profile = epson_tm_p60();
  profile.name = "epson_tm_p60ii";
  profile.identity.model = "TM-P60II";
  return profile;
}

CapabilityProfile epson_tm_p80() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_p80";
  profile.identity.model = "TM-P80";
  makePortable(profile);
  makeEpsonPortableBluetooth(profile);
  profile.media.black_mark_sensor = true;
  return profile;
}

CapabilityProfile epson_tm_p80ii() {
  CapabilityProfile profile = epson_tm_p80();
  profile.name = "epson_tm_p80ii";
  profile.identity.model = "TM-P80II";
  // §6: 80 mm, direct thermal, USB-C, Wi-Fi a/b/g/n/ac on the Wi-Fi unit, Bluetooth 5
  // Dual Mode on the BT unit; paper-end, black-mark and cover detectors; the same
  // 4 KB / 64 KB buffers; 384 KB NV; barcodes plus PDF417, QR, MaxiCode, DataMatrix,
  // Aztec and GS1. **The normal unit is manual tear.**
  return profile;
}

CapabilityProfile epson_tm_p80ii_autocutter() {
  CapabilityProfile profile = epson_tm_p80ii();
  profile.name = "epson_tm_p80ii_autocutter";
  profile.identity.model = "TM-P80II Auto Cutter Model";
  // §6: the Auto Cutter Model is separately sold hardware, so it is **a distinct
  // capability profile**, not a runtime flag on the standard unit. Getting this wrong
  // in either direction is expensive: assume a cutter that is not there and every
  // receipt stays attached; assume a tear bar on a unit that has a blade and the engine
  // never clears the head-to-cutter gap, which is how a trailing QR gets sliced.
  profile.media.cutter = true;
  profile.media.partial_cut = true;
  profile.media.full_cut = false;
  profile.cut = CutVariant::Partial;
  profile.media.head_to_cutter_feed_dots = 100;
  return profile;
}

CapabilityProfile epson_tm_u220() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_u220";
  profile.identity.model = "TM-U220 series";
  // Dot impact, not thermal: 76 mm paper, 63.5 mm print width, 400 dots at 160 dpi.
  // Nothing about this geometry follows from the thermal 8-dots-per-mm rule.
  profile.media.nominal_roll_width_mm = 76;
  profile.media.printable_width_mm = 63;
  profile.media.printable_width_dots = 400;
  profile.media.dpi = 160;
  profile.media.full_cut = false;  // the mechanism's autocutter is partial-cut only
  profile.completion = CompletionMechanism::GsR1;
  profile.completion_caps.process_id_gs_h = false;
  profile.completion_caps.try_process_id_gs_h = false;
  // Epson's applicability database covers the U220, but the brief does not record it as
  // carrying the process-ID extension, so the flag is false and its provenance is
  // Unverified rather than "documented absent" — those are different claims.
  profile.completion_caps.process_id_gs_h_provenance = Provenance::Unverified;
  // Impact printing is slow enough that a completion wait sized for thermal speeds
  // times out on a long ticket.
  profile.completion_timeout_ms = 30000;
  profile.quirks.delayed_status = true;
  return profile;
}

CapabilityProfile epson_tm_u220ii() {
  CapabilityProfile profile = epson_tm_u220();
  profile.name = "epson_tm_u220ii";
  profile.identity.model = "TM-U220II";
  return profile;
}

CapabilityProfile epson_tm_i() {
  CapabilityProfile profile = epsonBase();
  profile.name = "epson_tm_i";
  profile.identity.model = "TM-i / DT series";
  // Both paths are real on this hardware. The ESC/POS fence stays the drivable
  // default; the ePOS JobID is the stronger mechanism once that transport exists
  // (§5: submission returns immediately with the spooler enabled and the result is
  // retrieved later by JobID), and its result comes from the spooler rather than from
  // the mechanism (docs/capability-profiles.md §4). That retrieval is what §24 grades
  // A+; until this core can perform it, nothing here claims that grade.
  profile.completion_caps.epos_job_id = true;
  profile.completion_caps.epos_job_id_provenance = Provenance::Documented;
  profile.languages.add(CommandLanguage::EposXml);
  profile.transport.epos = true;
  profile.transport.wifi = true;
  profile.status.extended_asb = true;
  // --- M13b (docs/wire-protocols.md §1) --------------------------------------------
  // The head of the documented spooler matrix: TM-i with ePOS-Print Service firmware 4.1
  // or newer has both the printer-side spooler and the JobIDs needed to retrieve a
  // result, which together are the only grade A+ mechanism in this SDK.
  //
  // Spooling is a DEVICE SETTING (TMNet WebConfig / EpsonNet Config / the setup utility),
  // not a request attribute, so this flag describes how the unit was configured, and a
  // deployment that left the spooler off gets submit-completes-after-print semantics from
  // the same model. It is carried per profile precisely because it cannot be asked for on
  // the wire.
  profile.epos.spooler = true;
  profile.epos.job_id = true;
  profile.epos.spooler_provenance = Provenance::Documented;
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
  // M14, docs/cash-drawer.md §7-9, verbatim: **`rongta_rp58_family: DO NOT INHERIT`**.
  // The 80 mm units' 24 V / 1 A socket is a fact about those units. Smaller Rongta
  // hardware differs, and 12 V drawer outputs exist in this size class, so inheriting
  // the family port would mean connecting a 24 V drawer on the strength of a number
  // measured on a different machine. Cleared to Unknown, which refuses the pulse.
  makeUnclassifiedDrawerPort(
      profile,
      "DO NOT INHERIT the 80 mm family's 24 V / 1 A drawer figures: this size class "
      "includes 12 V outputs, and connecting a 24 V drawer to one risks the drive "
      "circuit. Establish the port on the actual unit first.");
  return profile;
}

CapabilityProfile rongta_rp80() {
  CapabilityProfile profile = rongtaBase();
  profile.name = "rongta_rp80";
  profile.identity.model = "RP80 family";
  // Firmware varies across RP80/802/803/807/820/850. With the GS ( H documentation
  // withdrawn (§13), the shipped default is the queued fence for every one of them.
  return profile;
}

CapabilityProfile rongta_rp3xx() {
  CapabilityProfile profile = rongtaBase();
  profile.name = "rongta_rp3xx";
  profile.identity.model = "RP326 / RP331";
  // §13, from Rongta's current official page for the RP326: 79.5 +/- 0.5 mm roll,
  // 72 mm and 48 mm print widths, 576 and 384 dots, 203 dpi, 250 mm/s,
  // USB/serial/Ethernet with optional Bluetooth and Wi-Fi customs, a 1.5 M-cut cutter,
  // ESC/POS and OPOS. The RP331 is the same geometry.
  profile.media.printable_width_dots_58mm = escpos::kWidth58mm;
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile rongta_rp8xx() {
  CapabilityProfile profile = rongtaBase();
  profile.name = "rongta_rp8xx";
  profile.identity.model = "RP8xx series";
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
  // §14, the case the whole provenance system exists for. Xprinter's official material
  // gives 79.5 +/- 0.5 mm media, <= 72 mm print width, 576 dots, 203 dpi, <= 260 mm/s,
  // USB + serial + Ethernet, paper-end / cover-open / black-mark sensing, a partial
  // cutter, 256 KB NV flash, ESC/POS, and an input buffer listed as 128 KB in the
  // current table but 64 KB on another page revision — which is why firmware and
  // hardware identity belong in the profile.
  //
  // It does NOT document `GS ( H`. Our XP-S260M answers it anyway, confirmed by probe
  // and a 100-receipt soak. That finding lives in xp_s260m() (Provenance::Probed) and
  // in the findings store; this *family* default stays on the queued fence with
  // Unverified provenance, because one interrogated unit is not a family, and firmware
  // varies. try_process_id_gs_h asks the question on every probe.
  profile.completion_caps.try_process_id_gs_h = true;
  profile.completion_caps.vendor_idle = true;  // Xprinter one-ticket-one-control
  profile.media.black_mark_sensor = true;
  profile.final_feed_lines = 3;
  // M14, docs/cash-drawer.md §6. The XP-S260M's official specification gives the
  // drawer output as **DC 24 V / 1 A**, which classifies the port — electrically this
  // is an ordinary Epson-compatible 24 V socket and a normal drawer cable is correct.
  // No current Xprinter-hosted programmer reference proves the pulse or the status
  // command, so the command half stays PROBE REQUIRED: the kick is emitted as `ESC p`
  // because that is the only candidate, the status path claims nothing, and the honest
  // outcome of a pulse on this entry is KickSentUnverified until a unit is tested.
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  makeEscPosDrawerCommands(profile, DrawerKickMethod::EpsonEscP, Provenance::Unverified);
  profile.drawer.note =
      "Drawer output documented as DC 24 V / 1 A. ESC p, the drawer status read and "
      "channel 2 are unproven on this brand: run `pdctl drawer test` on the unit "
      "before relying on a verified open.";
  return profile;
}

CapabilityProfile xprinter_v_series() {
  CapabilityProfile profile = xprinterBase();
  profile.name = "xprinter_v_series";
  profile.identity.model = "V series";
  profile.completion_caps.try_process_id_gs_h = true;
  profile.final_feed_lines = 6;
  return profile;
}

CapabilityProfile xprinter_portable() {
  CapabilityProfile profile = xprinterBase();
  profile.name = "xprinter_portable";
  profile.identity.model = "portable (58 mm handheld)";
  makeFiftyEight(profile);
  makePortable(profile);
  // Classic SPP only: this class does not document BLE, and claiming it would send an
  // iOS app down a code path that cannot work.
  profile.transport.bluetooth.classic_spp = true;
  profile.chunk_bytes = 512;
  profile.inter_chunk_delay_ms = 30;
  profile.final_feed_lines = 6;
  return profile;
}

// --- Sewoo / Partner Tech -----------------------------------------------------------

CapabilityProfile sewoo_slk_ts() {
  CapabilityProfile profile = partnerBase();
  profile.name = "sewoo_slk_ts";
  profile.identity.vendor = "Sewoo";
  profile.identity.model = "SLK-TS series";
  // M14, docs/cash-drawer.md §7-9. The SLK-TS200 documents cash-box control over a
  // 6-wire socket at 24 V / 1 A, so the port is established here even though the
  // Partner base it derives from leaves it Unknown. The command implementation is not
  // verified, and the doc is explicit that it must not be carried across to the
  // Partner RP-110 on OEM resemblance — which is why this override lives on the model
  // and not on partnerBase().
  makeEpsonStyleDrawerPort(profile, Provenance::Documented);
  makeEscPosDrawerCommands(profile, DrawerKickMethod::EpsonEscP, Provenance::Unverified);
  profile.drawer.note =
      "Cash-box control documented as a 6-wire 24 V / 1 A socket. The command "
      "implementation still needs verification on a unit, and must not be assumed for "
      "any OEM sibling.";
  return profile;
}

CapabilityProfile partner_rp110() {
  CapabilityProfile profile = partnerBase();
  profile.name = "partner_rp110";
  profile.identity.model = "RP-110";
  // §15, from Partner Tech's current manufacturer page: 80 mm maximum media,
  // 72 +/- 0.5 mm print width, 203 dpi, direct thermal, USB-B + RS-232 + Ethernet all
  // standard, project-specific Bluetooth Smart Ready / BT 5 + BLE, project-specific
  // 802.11 a/b/g/n, paper-end and cover-open sensing, black-mark capability,
  // full/partial guillotine rated 1.5 M cuts, ESC/POS-compatible.
  //
  // ENERGY STAR certification records group RP-110/RP-110B/RP-110W with the Sewoo
  // SLK-TS200 under one product family, which is why the Sewoo profile is the honest
  // starting point. "Project-specific" is why the Bluetooth facets are recorded but
  // the transport flags stay conservative: the unit on the counter may have none of it.
  profile.media.near_end_sensor = true;
  profile.media.black_mark_sensor = true;
  profile.transport.bluetooth.classic_spp = true;
  profile.transport.bluetooth.ble = true;
  profile.transport.wifi = true;
  return profile;
}

// --- M14: SNBC / New Beiyang (docs/cash-drawer.md §5) --------------------------------

CapabilityProfile snbc_btp_r880() {
  CapabilityProfile profile = snbcBase();
  profile.name = "snbc_btp_r880";
  profile.identity.model = "BTP-R880NP";
  profile.drawer.note =
      "Programming manual documents both the queued ESC p pulse and the realtime "
      "DLE DC4 n m t variant; the queued pulse is what this engine emits, because a "
      "realtime pulse can overtake a receipt still in the buffer.";
  return profile;
}

CapabilityProfile snbc_btp_u80() {
  CapabilityProfile profile = snbcBase();
  profile.name = "snbc_btp_u80";
  profile.identity.model = "BTP-U80 family";
  // Same vendor, same interface class, but the drawer command reference this database
  // trusts is the BTP-R880NP one. Port classification carries across the 80 mm line;
  // the command half drops back to a probe question rather than borrowing a manual.
  profile.drawer.evidence.commands = Provenance::Unverified;
  profile.drawer.status.available = false;
  profile.drawer.status.method = DrawerStatusMethod::None;
  return profile;
}

CapabilityProfile partner_rp710() {
  CapabilityProfile profile = partner_rp110();
  profile.name = "partner_rp710";
  profile.identity.model = "RP-710";
  return profile;
}

// --- Star ---------------------------------------------------------------------------

CapabilityProfile star_tsp100() {
  CapabilityProfile profile = starRawBase();
  profile.name = "star_tsp100";
  profile.identity.model = "TSP100 series";
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile star_tsp100iii() {
  CapabilityProfile profile = star_tsp100();
  profile.name = "star_tsp100iii";
  profile.identity.model = "TSP100III";
  profile.transport.wifi = true;
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.vendor_sdk = true;
  return profile;
}

CapabilityProfile star_tsp100iv() {
  CapabilityProfile profile = star_tsp100();
  profile.name = "star_tsp100iv";
  profile.identity.model = "TSP100IV";
  // §7-§8: 80 and 58 mm (guide), direct thermal, StarPRNT, 250 mm/s, USB-C host, USB-A
  // peripheral/Android, Ethernet; the wireless variant adds 2.4/5 GHz Wi-Fi and
  // Bluetooth.
  profile.transport.wifi = true;
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.ble = true;
  profile.transport.bluetooth.vendor_sdk = true;
  return profile;
}

CapabilityProfile star_tsp650() {
  CapabilityProfile profile = starRawBase();
  profile.name = "star_tsp650";
  profile.identity.model = "TSP650II";
  profile.media.paper_guide_58mm = true;
  profile.transport.serial = true;
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.vendor_sdk = true;
  return profile;
}

CapabilityProfile star_mcprint() {
  CapabilityProfile profile = starRawBase();
  profile.name = "star_mcprint";
  profile.identity.model = "mC-Print series";
  profile.media.paper_guide_58mm = true;
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.vendor_sdk = true;
  return profile;
}

CapabilityProfile star_mcprint2() {
  CapabilityProfile profile = star_mcprint();
  profile.name = "star_mcprint2";
  profile.identity.model = "mC-Print2";
  // The 2-inch member: 58 mm is its native width, not a guide setting.
  makeFiftyEight(profile);
  profile.media.cutter = true;
  profile.media.partial_cut = true;
  return profile;
}

CapabilityProfile star_mcprint3() {
  CapabilityProfile profile = star_mcprint();
  profile.name = "star_mcprint3";
  profile.identity.model = "mC-Print3";
  // §7-§8: 80 and 58 mm, front exit, print areas of 72, ~51 and 48 mm **by paper
  // setup**, USB, Ethernet, Bluetooth on variants, up to 400 mm/s on newer versions.
  // Three print widths for one mechanism is the clearest possible argument against
  // deriving raster width from roll width.
  profile.media.printable_width_dots_58mm = escpos::kWidth58mm;
  profile.transport.wifi = true;
  return profile;
}

CapabilityProfile star_sm_s210() {
  CapabilityProfile profile = starBase();
  profile.name = "star_sm_s210";
  profile.identity.model = "SM-S210i";
  makeFiftyEight(profile);
  makePortable(profile);
  // §8: the SDK-listed portables use Bluetooth transports with model-specific
  // emulations, and several support StarPRNT *and* an EscPosMobile mode. The
  // emulation is not the documented path — the SDK is — so the profile stays StarPRNT
  // and the engine keeps refusing, rather than reaching for a mode that answers no
  // checked block.
  profile.transport.bluetooth.classic_vendor = true;
  profile.transport.bluetooth.vendor_sdk = true;
  return profile;
}

CapabilityProfile star_sm_s220() {
  CapabilityProfile profile = star_sm_s210();
  profile.name = "star_sm_s220";
  profile.identity.model = "SM-S220i";
  return profile;
}

CapabilityProfile star_sm_s230() {
  CapabilityProfile profile = star_sm_s210();
  profile.name = "star_sm_s230";
  profile.identity.model = "SM-S230i";
  // §8, the current model: 58 mm paper, 48 mm print width, 80 mm/s, **Bluetooth 5.2**,
  // USB 2.0 Full Speed, ~217 g with battery, ~15 h at Star's 5-minute-interval test.
  // Bluetooth 5.2 means both radios are present, so BLE is recorded alongside Classic.
  profile.transport.bluetooth.ble = true;
  return profile;
}

CapabilityProfile star_sm_l200() {
  CapabilityProfile profile = star_sm_s210();
  profile.name = "star_sm_l200";
  profile.identity.model = "SM-L200";
  // §8: 58 mm, the Bluetooth 3.0/4.0 dual-mode generation, USB 2.0, ~220 g; the current
  // SDK identifies it as BLE-capable and StarPRNT-capable.
  profile.transport.bluetooth.ble = true;
  return profile;
}

CapabilityProfile star_sm_l300() {
  CapabilityProfile profile = star_sm_l200();
  profile.name = "star_sm_l300";
  profile.identity.model = "SM-L300";
  // The 3-inch member of the L line: 80 mm media, 72 mm image.
  profile.media.nominal_roll_width_mm = 80;
  profile.media.printable_width_mm = 72;
  profile.media.printable_width_dots = escpos::kWidth80mm;
  return profile;
}

CapabilityProfile star_sm_t300() {
  CapabilityProfile profile = star_sm_s210();
  profile.name = "star_sm_t300";
  profile.identity.model = "SM-T300i";
  profile.media.nominal_roll_width_mm = 80;
  profile.media.printable_width_mm = 72;
  profile.media.printable_width_dots = escpos::kWidth80mm;
  profile.media.black_mark_sensor = true;
  return profile;
}

CapabilityProfile star_sm_t400() {
  CapabilityProfile profile = star_sm_t300();
  profile.name = "star_sm_t400";
  profile.identity.model = "SM-T400i";
  // The 4-inch member: 112 mm media, 104 mm image — the same two-independent-numbers
  // rule as the Citizen CT-S4500.
  profile.media.nominal_roll_width_mm = 112;
  profile.media.printable_width_mm = 104;
  profile.media.printable_width_dots = escpos::kWidth104mm;
  return profile;
}

// --- Bixolon ------------------------------------------------------------------------

CapabilityProfile bixolon_srp330() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp330";
  profile.identity.model = "SRP-330III/332III";
  // Bixolon publishes an actual command manual for the SRP-330II, which is what makes
  // DLE EOT and GS r documented-per-model questions rather than guesses — but the
  // manual has not been loaded and validated here, so the provenance stays Unverified
  // (§9-§10 policy) instead of borrowing credibility from its existence.
  return profile;
}

CapabilityProfile bixolon_srp350() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp350";
  profile.identity.model = "SRP-350III/V, 352, 350plus";
  profile.media.paper_guide_58mm = true;
  // The SRP-350V documentation lists USB, Ethernet, serial, dual serial and parallel.
  profile.transport.serial = true;
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
  profile.transport.bluetooth.classic_spp = true;
  return profile;
}

CapabilityProfile bixolon_srp_q200() {
  CapabilityProfile profile = bixolon_q_series();
  profile.name = "bixolon_srp_q200";
  profile.identity.model = "SRP-Q200";
  return profile;
}

CapabilityProfile bixolon_srp_q300() {
  CapabilityProfile profile = bixolon_q_series();
  profile.name = "bixolon_srp_q300";
  profile.identity.model = "SRP-Q300";
  // Bixolon publishes a command manual for this one specifically (§9).
  return profile;
}

CapabilityProfile bixolon_srp_b300() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp_b300";
  profile.identity.model = "SRP-B300";
  return profile;
}

CapabilityProfile bixolon_srp_f310() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp_f310";
  profile.identity.model = "SRP-F310";
  profile.media.paper_guide_58mm = true;
  return profile;
}

CapabilityProfile bixolon_srp_275() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_srp_275";
  profile.identity.model = "SRP-275III (impact)";
  // Dot impact, like the TM-U220 and for the same reason: 76 mm paper, a narrower
  // image, coarser dots, and a mechanism slow enough that a thermal completion budget
  // expires on a long ticket.
  profile.media.nominal_roll_width_mm = 76;
  profile.media.printable_width_mm = 63;
  profile.media.printable_width_dots = 400;
  profile.media.dpi = 160;
  profile.media.full_cut = false;
  profile.completion_timeout_ms = 30000;
  profile.quirks.delayed_status = true;
  return profile;
}

CapabilityProfile bixolon_spp_r200() {
  CapabilityProfile profile = bixolonBase();
  profile.name = "bixolon_spp_r200";
  profile.identity.model = "SPP-R200III";
  // §10: 2-inch / 58 mm; Bixolon publishes user, command **and Bluetooth** manuals for
  // it; serial and USB plus Bluetooth Class 2 v3.0+EDR. First-class, not "generic
  // Bluetooth ESC/POS".
  makeFiftyEight(profile);
  makePortable(profile);
  profile.transport.serial = true;
  profile.transport.bluetooth.classic_spp = true;
  return profile;
}

CapabilityProfile bixolon_spp_r210() {
  CapabilityProfile profile = bixolon_spp_r200();
  profile.name = "bixolon_spp_r210";
  profile.identity.model = "SPP-R210";
  return profile;
}

CapabilityProfile bixolon_spp_r310() {
  CapabilityProfile profile = bixolon_spp_r200();
  profile.name = "bixolon_spp_r310";
  profile.identity.model = "SPP-R310";
  // §10: 3-inch / 80 mm class, 203 dpi, print width <= 72 mm, <= 100 mm/s on receipt,
  // USB 2.0 Full Speed and serial standard, Bluetooth or WLAN variants, NFC-assisted
  // pairing, paper-end and cover-open sensing with optional black-mark/gap, 8 MB SDRAM
  // and 4 MB flash, Android/iOS/Windows/Linux SDKs, and a published
  // `Command Manual_SPP-R310` alongside a Bluetooth manual.
  profile.media.nominal_roll_width_mm = 80;
  profile.media.printable_width_mm = 72;
  profile.media.printable_width_dots = escpos::kWidth80mm;
  profile.media.black_mark_sensor = true;
  // The WLAN variant of the R310; the Bluetooth variant is a different unit with the
  // same model number, which is why interfaces are a facet and not part of identity.
  profile.transport.wifi = true;
  profile.transport.raw_tcp_9100 = true;
  return profile;
}

CapabilityProfile bixolon_spp_r410() {
  CapabilityProfile profile = bixolon_spp_r310();
  profile.name = "bixolon_spp_r410";
  profile.identity.model = "SPP-R410";
  // The 4-inch member: 112 mm media, 104 mm image.
  profile.media.nominal_roll_width_mm = 112;
  profile.media.printable_width_mm = 104;
  profile.media.printable_width_dots = escpos::kWidth104mm;
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
  // §11-§12: 500 mm/s; top exit on the 801III and front exit on the 851III; USB
  // standard with Wi-Fi, serial, Bluetooth, Ethernet and parallel optional; an LCD
  // status UI.
  profile.media.near_end_sensor = true;
  profile.completion_timeout_ms = 12000;
  return profile;
}

CapabilityProfile citizen_cts_wide() {
  CapabilityProfile profile = citizenBase();
  profile.name = "citizen_cts_wide";
  profile.identity.model = "CT-S4500 / CT-S4000";
  // §11-§12: media 58-112 mm, **maximum print width 104 mm**, 203 dpi, <= 200 mm/s,
  // USB 2.0 FS standard, optional Bluetooth including **Apple MFi**, optional serial,
  // LAN and Wi-Fi; gap, black-mark and paper-end sensing; a guillotine doing full and
  // partial cuts. "Do not derive raster width from roll width" is written about this
  // exact printer.
  profile.media.nominal_roll_width_mm = 112;
  profile.media.printable_width_mm = 104;
  profile.media.printable_width_dots = escpos::kWidth104mm;
  profile.media.black_mark_sensor = true;
  profile.media.gap_sensor = true;
  profile.media.near_end_sensor = true;
  profile.transport.wifi = true;
  profile.transport.bluetooth.classic_spp = true;
  profile.transport.bluetooth.mfi = true;
  return profile;
}

CapabilityProfile citizen_cmp_20ii() {
  CapabilityProfile profile = citizenBase();
  profile.name = "citizen_cmp_20ii";
  profile.identity.model = "CMP-20II";
  // §11-§12: 58 mm media, 48 mm print width, 203 dpi, 80 mm/s, RS-232 and USB,
  // Bluetooth Class 2 in a 4.2 configuration, an MFi option, a Wi-Fi option,
  // **ESC/POS + CPCL + ZPL2**, paper-end sensing, 7.4 V / 1800 mAh. Citizen publishes
  // the actual CMP20II ESC/POS command reference, so ESC/POS here is a real document
  // rather than a compatibility claim — but the Epson feedback extensions are not in
  // it, so every completion provenance stays Unverified.
  makeFiftyEight(profile);
  makePortable(profile);
  profile.languages.add(CommandLanguage::Cpcl);
  profile.languages.add(CommandLanguage::Zpl);
  profile.transport.serial = true;
  profile.transport.bluetooth.classic_spp = true;
  profile.transport.bluetooth.mfi = true;
  // Wi-Fi is an option on this model rather than a configuration of it, so the socket
  // is claimed alongside it and not instead of the Bluetooth facets.
  profile.transport.wifi = true;
  profile.transport.raw_tcp_9100 = true;
  profile.completion_caps.try_process_id_gs_h = true;
  return profile;
}

CapabilityProfile citizen_cmp_30ii() {
  CapabilityProfile profile = citizen_cmp_20ii();
  profile.name = "citizen_cmp_30ii";
  profile.identity.model = "CMP-30II";
  // §11-§12: media 25-80 mm, **maximum print width 72 mm**, 203 dpi, 100 mm/s, RS-232
  // and USB, Bluetooth Class 2 / 4.2 variants, MFi, Wi-Fi, ESC/POS + CPCL + ZPL2,
  // paper-end plus a reflective black mark sensor, 7.4 V / 2600 mAh, IP42. Citizen
  // publishes CMP30II ESC/POS, CMP30II CPCL and CMP ZPL command references —
  // unusually good material for protocol-level work, and the reason a profile carries
  // a set of languages rather than one.
  profile.media.nominal_roll_width_mm = 80;
  profile.media.printable_width_mm = 72;
  profile.media.printable_width_dots = escpos::kWidth80mm;
  profile.media.black_mark_sensor = true;
  return profile;
}

// --- Zebra --------------------------------------------------------------------------

CapabilityProfile zebra_zq300_plus() {
  CapabilityProfile profile = zebraBase();
  profile.name = "zebra_zq300_plus";
  profile.identity.model = "ZQ310 Plus / ZQ320 Plus";
  // §16: the ZQ310 Plus is the 2-inch member and the ZQ320 Plus the 3-inch one; the
  // narrower geometry is recorded because assuming the wider one clips.
  makeFiftyEight(profile);
  makePortable(profile);
  profile.media.cutter = false;
  return profile;
}

CapabilityProfile zebra_zq500() {
  CapabilityProfile profile = zebraBase();
  profile.name = "zebra_zq500";
  profile.identity.model = "ZQ511 / ZQ521";
  makePortable(profile);
  return profile;
}

CapabilityProfile zebra_zq600_plus() {
  CapabilityProfile profile = zebraBase();
  profile.name = "zebra_zq600_plus";
  profile.identity.model = "ZQ610/620/630 Plus";
  makePortable(profile);
  // §16: the ZQ630 Plus documents ZPL, CPCL **and EPL**, and newer configurations pair
  // Wi-Fi 6 with Bluetooth 5.3. Link-OS supplies a separate `StatusConnection` —
  // including `BluetoothStatusConnection` and `BluetoothLeStatusConnection` — a
  // status-only channel that does not block the print channel. That is a genuinely
  // better design than anything in the ESC/POS world, and worth implementing natively;
  // until it is, this profile refuses rather than pretending.
  profile.transport.bluetooth.ble = true;
  return profile;
}

// --- Brother ------------------------------------------------------------------------

CapabilityProfile brother_rj2000() {
  CapabilityProfile profile = brotherBase();
  profile.name = "brother_rj2000";
  profile.identity.model = "RJ-2030 / RJ-2050 / RJ-2140 / RJ-2150";
  makeFiftyEight(profile);
  makePortable(profile);
  return profile;
}

CapabilityProfile brother_rj3000() {
  CapabilityProfile profile = brotherBase();
  profile.name = "brother_rj3000";
  profile.identity.model = "RJ-3230B / RJ-3230BL";
  makePortable(profile);
  return profile;
}

CapabilityProfile brother_rj4000() {
  CapabilityProfile profile = brotherBase();
  profile.name = "brother_rj4000";
  profile.identity.model = "RJ-4230B / RJ-4250WB";
  makePortable(profile);
  // §17: 4-inch class, USB, WLAN and Bluetooth documented. Brother publishes Raster
  // Command References and ESC/P references, and some models/configurations add ZPL II
  // and CPCL — none of which is ESC/POS. `Brother != generic_escpos`.
  profile.media.nominal_roll_width_mm = 112;
  profile.media.printable_width_mm = 104;
  profile.media.printable_width_dots = escpos::kWidth104mm;
  profile.languages.add(CommandLanguage::Zpl);
  profile.languages.add(CommandLanguage::Cpcl);
  profile.transport.wifi = true;
  profile.transport.raw_tcp_9100 = true;
  profile.transport.bluetooth.ble = true;
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
  // M14, docs/cash-drawer.md fleet table: "Generic 80 mm ESC/POS — disabled
  // initially, electrical unknown". The connector on an unidentified printer is a
  // 6P6C-shaped hole and nothing more.
  makeUnclassifiedDrawerPort(
      profile,
      "Drawer disabled: this is an unidentified device, so the connector pinout and "
      "drive voltage are unknown. Identify the model, then its drawer port, then "
      "connect a cable — in that order.");
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
  // M14, docs/cash-drawer.md fleet table, the row printed in bold: "Generic 58 mm
  // ESC/POS — **frequently 12 V** — Never assume 24 V." Same refusal as generic_80,
  // different warning, because the failure mode here is not "the drawer does not open"
  // but "a 24 V drawer on a 12 V output, or a 12 V drive circuit asked to run one".
  makeUnclassifiedDrawerPort(
      profile,
      "Drawer disabled, and NEVER assume 24 V on this size class: 58 mm units "
      "frequently specify a 12 V / 1 A drawer output. Voltage and pinout must both be "
      "established on the actual model before a drawer is connected.");
  return profile;
}

CapabilityProfile generic_unknown() {
  CapabilityProfile profile = generic_80();
  profile.name = "generic_unknown";
  profile.identity.model = "unidentified";
  // The escape hatch of docs/compatibility-brief.md §26, and the most honest entry in
  // the database: it prints, it cuts, and it claims nothing. No ordered fence, no
  // status queries, so every job on it terminates at grade E — transport only — which
  // is exactly what is known about a device nobody has identified. generic_80 says
  // "an ordinary 80 mm ESC/POS printer"; this says "we do not know what this is", and
  // those are different statements that were previously the same entry.
  profile.completion = CompletionMechanism::None;
  profile.completion_caps.queued_gs_r = false;
  profile.completion_caps.try_process_id_gs_h = true;  // the probe should still ask
  profile.status.dle_eot = false;
  profile.status.asb = false;
  profile.status.cutter_error = false;
  profile.media.black_mark_sensor = false;
  profile.media.near_end_sensor = false;
  profile.chunk_bytes = 512;
  profile.inter_chunk_delay_ms = 30;
  profile.completion_timeout_ms = 20000;
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
