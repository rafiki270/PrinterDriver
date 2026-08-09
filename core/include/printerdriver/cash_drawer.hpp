#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/types.hpp"

// M14 — the cash drawer (docs/cash-drawer.md).
//
// Architectural rule from the first paragraph of that document: **the drawer is a
// separate printer peripheral capability**, with its own electrical profile, its own
// command method and its own feedback method. It is deliberately not a field on
// PrinterCapabilities and not "a generic ESC/POS feature", because the three facts that
// decide whether a pulse is safe — the connector pinout, the drive voltage, and which
// command the firmware actually implements — are independent of each other and of
// everything the printer does with paper.
//
// The second rule is the one the doc sets in giant letters: **RJ11/RJ12-looking drawer
// connectors are not a universal electrical standard.** Star's same-looking 6P6C socket
// swaps the purposes of pins 3 and 6 against Epson's, and 12 V units exist alongside the
// common 24 V ones. So the profile carries a small electrical classification with four
// buckets — Epson-style, Star-style, 12 V exceptions, unknown — and nothing in this SDK
// fires an output on a port classified Unknown.
//
// The third rule is the one this file's DrawerState exists for: an API that answers
// `{sent: true}` is lying by omission. Sending ESC p and watching the microswitch
// physically change are different claims, exactly as they are for printing, so the
// answer is a state out of a closed set and never a boolean.

namespace pd {

// docs/cash-drawer.md "API states (never a boolean)". Closed, mirrored into pd.h and
// all four wrappers.
enum class DrawerState {
  // The sensor answered and, with a calibrated polarity, says the drawer is shut.
  Closed,
  // The sensor answered and says the drawer is already open. Reading this *before* a
  // pulse is what stops the engine energising a solenoid for a drawer that is already
  // out (step 1 of the opening sequence).
  Open,
  // A pulse is on the wire and the verification window has not closed yet.
  Opening,
  // The pulse was accepted by the link and nothing can confirm what happened next:
  // either the profile has no sensor, or the path has no working back channel. This is
  // the print-server case of docs/cash-drawer.md "Print servers and CUPS" — a cheap USB
  // print server passes the kick forward while the sensor answer never comes back — and
  // it is a genuinely different answer from OpenVerified, not a weaker spelling of it.
  KickSentUnverified,
  // The switch was seen changing from closed to open after the pulse. The only state in
  // this enum that claims physical movement.
  OpenVerified,
  // The pulse went out, the sensor kept answering, and it never changed: a locked
  // drawer, a jam, a wrong channel, or a cable that fits and is not wired for this
  // printer.
  FailedToOpen,
  // This printer's drawer port has no switch input wired or documented, so the question
  // cannot be asked at all. Distinct from Unknown: nothing is broken here.
  NoSensor,
  // Nothing is known — the port is unclassified, the method is unsupported, the link
  // went away, or the polarity has never been calibrated so a raw pin level cannot be
  // turned into open/closed without guessing.
  Unknown,
};

// docs/cash-drawer.md "Electrical classification": four buckets cover the deployed
// reality. The Epson arrangement (1 FG, 2 kick 1, 3 sensor, 4 +24 V, 5 kick 2, 6 signal
// ground) is shared by Citizen, much of Bixolon and many clones, which is why a normal
// "Epson-compatible 24 V drawer" travels across that ecosystem. Star's connector looks
// identical and puts +24 V on pin 3 and the sense line on pin 6.
enum class DrawerPortStandard {
  Epson24V6P6C,
  Star24V6P6C,
  Generic12V6P6C,
  Unknown,
};

// Which software path actually fires the drawer. The command and the cable pinout are
// independent facts (docs/cash-drawer.md, last paragraph): two printers accepting the
// same "kick drawer 1" command may still wire the modular port differently.
enum class DrawerKickMethod {
  EpsonEscP,     // ESC p m t1 t2, queued behind print data
  EpsonEpos,     // the ePOS peripheral API — never raw bytes smuggled into print XML
  StarPrnt,      // StarPRNT appendPeripheral(...)
  BixolonSdk,    // WebPrint/SDK makeDKout({connector, duration})
  CitizenEscP,   // ESC p, with the "cannot fire while printing" serialisation quirk
  SnbcEscP,      // ESC p (the realtime DLE DC4 variant exists and is not preferred)
  Vendor,        // documented, vendor-specific, not implemented here
  Unsupported,   // no drawer path on this device, or none this engine may use
};

// How the switch is read back.
enum class DrawerStatusMethod {
  GsR2,        // GS r 2 / GS r 50 — queued drawer-kick-out connector status
  Asb,         // the drawer bit inside an automatic status back frame
  StarSignal,  // drawer1OpenCloseSignal / StarXpand drawerOpenCloseSignal
  VendorSdk,   // the vendor SDK's own drawer status call
  None,
};

// Declaration order, for wrapper generators and the bridge tests that enumerate members
// without a hand-maintained list (same contract as types.hpp's kAll* arrays).
constexpr std::array<DrawerState, 8> kAllDrawerStates{
    DrawerState::Closed,       DrawerState::Open,
    DrawerState::Opening,      DrawerState::KickSentUnverified,
    DrawerState::OpenVerified, DrawerState::FailedToOpen,
    DrawerState::NoSensor,     DrawerState::Unknown,
};

constexpr std::array<DrawerPortStandard, 4> kAllDrawerPortStandards{
    DrawerPortStandard::Epson24V6P6C, DrawerPortStandard::Star24V6P6C,
    DrawerPortStandard::Generic12V6P6C, DrawerPortStandard::Unknown,
};

constexpr std::array<DrawerKickMethod, 8> kAllDrawerKickMethods{
    DrawerKickMethod::EpsonEscP,   DrawerKickMethod::EpsonEpos,
    DrawerKickMethod::StarPrnt,    DrawerKickMethod::BixolonSdk,
    DrawerKickMethod::CitizenEscP, DrawerKickMethod::SnbcEscP,
    DrawerKickMethod::Vendor,      DrawerKickMethod::Unsupported,
};

constexpr std::array<DrawerStatusMethod, 5> kAllDrawerStatusMethods{
    DrawerStatusMethod::GsR2, DrawerStatusMethod::Asb, DrawerStatusMethod::StarSignal,
    DrawerStatusMethod::VendorSdk, DrawerStatusMethod::None,
};

const char* to_string(DrawerState) noexcept;
const char* to_string(DrawerPortStandard) noexcept;
const char* to_string(DrawerKickMethod) noexcept;
const char* to_string(DrawerStatusMethod) noexcept;

// --- The capability facet ------------------------------------------------------------

// docs/cash-drawer.md "Electrical classification". `voltage` and `max_current_ma` are 0
// when the manufacturer does not document them, which is not the same as "low".
// `sensor_pin` is 3 on the Epson arrangement and 6 on Star's; 0 means nobody has
// established it, and a pulse is refused long before that matters.
struct DrawerElectrical {
  DrawerPortStandard standard = DrawerPortStandard::Unknown;
  uint16_t voltage = 0;
  uint16_t max_current_ma = 0;
  uint8_t channel_count = 0;
  uint8_t sensor_pin = 0;
};

struct DrawerKick {
  DrawerKickMethod method = DrawerKickMethod::Unsupported;
  // 200 ms is the default for known 24 V printer-driven drawers, and it is also
  // Bixolon's own SDK default. Epson programs the pulse in 2 ms units, so every value
  // here is rounded down to an even number of milliseconds on the wire.
  uint16_t default_pulse_ms = 200;
  uint16_t max_pulse_ms = 500;
  // Not a manufacturer number on most families: a driver-side floor, so a retry loop
  // cannot hold a 24 V solenoid energised. Bixolon and Citizen document recovery/duty
  // limits of their own, and those are what the per-family seeds carry.
  uint16_t cooldown_ms = 0;
  // docs/cash-drawer.md §3: on the affected Citizen models the drawer output cannot
  // fire while the mechanism is printing, so the daemon serialises receipt completion
  // and then the pulse. See PrinterRuntime::openDrawer for what this changes.
  bool can_kick_during_print = true;
};

// docs/cash-drawer.md §4. Star warns that the meaning of the sense line — whether true
// is open or closed — depends on the attached drawer and its microswitch, so this SDK
// calibrates once against the physical drawer and persists the answer instead of
// assuming a polarity. Until that has happened, a raw pin level is reported as a raw pin
// level and the state is Unknown.
struct DrawerPolarity {
  bool calibrated = false;
  bool high_means_open = false;
};

struct DrawerSensing {
  bool available = false;
  DrawerStatusMethod method = DrawerStatusMethod::None;
  // docs/cash-drawer.md "Two drawers, one switch": Epson-style hardware commonly has
  // two drive outputs and ONE switch input, so both channels can be kicked
  // independently while the only readable fact is "some attached drawer is open".
  bool shared_between_drawers = true;
  DrawerPolarity polarity;
  // The doc's "watch/poll the sensor ~1-2 s" window, and how often to ask inside it.
  // Fields rather than constants because a test has to be able to afford to wait one
  // out, and because a slow interface path deserves a longer window than a direct
  // socket does.
  uint16_t verify_window_ms = 1500;
  uint16_t poll_interval_ms = 100;
};

struct DrawerPort {
  // docs/cash-drawer.md "Buzzer conflict": Epson documents that with the optional
  // external buzzer enabled, the pulse that would go to the drawer connector sounds the
  // buzzer instead; Star's external-device port is shared the same way. Never assume
  // both coexist.
  bool shared_with_buzzer = false;
};

// docs/cash-drawer.md keeps two questions apart everywhere, and so does this: whether
// the *electrics* are documented, and whether the *commands* are. The XP-S260M is the
// case that forces the split — DC 24 V / 1 A is in Xprinter's own specification while
// no manufacturer-hosted programmer reference proves the pulse and status commands.
struct DrawerEvidence {
  Provenance electrical = Provenance::Unverified;
  Provenance commands = Provenance::Unverified;

  bool documented() const noexcept {
    return electrical == Provenance::Documented && commands == Provenance::Documented;
  }
  bool probed() const noexcept {
    return electrical == Provenance::Probed || commands == Provenance::Probed;
  }
};

struct DrawerCapabilities {
  // The device has a drawer port at all. False on the portables and the label printers.
  bool present = false;
  DrawerElectrical electrical;
  DrawerKick kick;
  DrawerSensing status;
  DrawerPort port;
  DrawerEvidence evidence;
  // Free text for the one thing a struct of enums cannot carry: why a given entry is
  // deliberately disabled. Read by `pdctl drawer-probe` and by the agent; never crosses
  // the C ABI, which carries the structured fields only.
  std::string note;

  // Whether this engine may fire this port. Both halves are load-bearing: a method it
  // cannot drive, and an electrical class nobody has established. The second is the
  // giant-letters rule of the doc — discovery must never blindly fire outputs on an
  // unknown drawer — enforced here rather than in the CLI, so the agent endpoint and
  // every wrapper inherit it. An integrator who has verified the port supplies a
  // profile with the standard set, which is a deliberate act with a name on it.
  bool kickable() const noexcept;
  bool electricalKnown() const noexcept {
    return electrical.standard != DrawerPortStandard::Unknown;
  }
  bool sensorReadable() const noexcept {
    return present && status.available && status.method == DrawerStatusMethod::GsR2;
  }

  // 0 asks for the profile default. The result is clamped to max_pulse_ms and rounded
  // down to Epson's 2 ms programming unit.
  uint16_t pulseFor(uint16_t requested_ms) const noexcept;
  // 0 asks for channel 1. Clamped to the documented channel count.
  uint8_t channelFor(uint8_t requested) const noexcept;
};

// --- The operation ---------------------------------------------------------------

struct DrawerRequest {
  uint8_t channel = 1;    // 1 = drive 1 (Epson pin 2), 2 = drive 2 (Epson pin 5)
  uint16_t pulse_ms = 0;  // 0 = the profile's default
};

// The answer docs/cash-drawer.md asks for by name, instead of {sent: true}.
// `elapsed_ms` is measured from the moment the pulse left for the link to the moment the
// verdict was reached — the "sensor changed 143 ms after kick" number — and is 0 when
// there was nothing to wait for.
struct DrawerOpenResult {
  DrawerState state = DrawerState::Unknown;
  DrawerState previous_state = DrawerState::Unknown;
  uint8_t channel = 1;
  uint16_t pulse_ms = 0;
  uint32_t elapsed_ms = 0;
};

// One non-destructive sensor read. Separate from DrawerOpenResult because reading is
// safe on hardware a pulse is refused on, and because it is the only place the
// needs-calibration marker can honestly live: `pin_high` is the raw fact, `state` is
// the interpretation, and without a calibration there is no interpretation.
struct DrawerReading {
  bool available = false;  // the profile documents a readable switch
  bool answered = false;   // the device actually replied within the timeout
  std::optional<bool> pin_high;
  bool needs_calibration = true;
  DrawerState state = DrawerState::Unknown;
};

// GS r 2 answers with one byte whose bit 0 is the drawer-kick-out connector's sense pin
// (docs/cash-drawer.md §1). Everything else in the byte is model-specific padding.
bool drawerPinHigh(uint8_t gs_r2_answer) noexcept;

// Raw level plus calibrated polarity in, open/closed out. Unknown when the polarity has
// never been calibrated — which is the whole point of Star's warning.
DrawerState drawerStateFrom(bool pin_high, const DrawerPolarity& polarity) noexcept;

// --- Persistence --------------------------------------------------------------------
//
// The calibrated polarity has to outlive the process, for the same reason the instance
// nonce does: it describes the physical installation, not this run. Stored as one small
// text file in the driver's storage directory, written temp-file-then-rename so a crash
// mid-write cannot leave a half-line that silently inverts a drawer's reported state.
// An empty directory means in-memory only, matching StorageConfig and FindingsStore.
class DrawerPolarityStore {
 public:
  explicit DrawerPolarityStore(std::string directory,
                               std::string file_name = "drawer.polarity");

  std::optional<bool> find(const std::string& printer_id) const;
  void save(const std::string& printer_id, bool high_means_open);

  size_t size() const;
  const std::string& path() const noexcept { return path_; }
  bool persistent() const noexcept { return !path_.empty(); }

 private:
  struct Record {
    std::string printer_id;
    bool high_means_open = false;
  };

  void load();
  void flush() const;

  mutable std::mutex mutex_;
  std::string path_;
  std::vector<Record> records_;
};

}  // namespace pd
