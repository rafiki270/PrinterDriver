#include "printerdriver/cash_drawer.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "fake_printer.hpp"
#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "test_harness.hpp"

// M14 — docs/cash-drawer.md.
//
// The suite is organised the way the document is: first what the fleet's drawer facets
// say, then the electrical refusals, then the opening sequence itself against a scripted
// solenoid-and-microswitch, then the polarity calibration that makes the switch readable
// at all. Every case in the doc that produces a *different answer* has a test here,
// because the entire point of the milestone is that "we sent the command" and "we saw
// the switch move" must never collapse into one boolean.

using namespace pd;
using namespace std::chrono_literals;

namespace {

escpos::Bytes textPayload(const std::string& text) {
  escpos::Encoder encoder;
  encoder.line(text);
  return encoder.take();
}

struct Rig {
  pdfake::MockLink link;
  std::unique_ptr<PrinterDriver> driver;
  std::shared_ptr<Printer> printer;
  CapabilityProfile profile;
  std::string id = "drawer-printer";

  explicit Rig(StorageConfig storage = StorageConfig::inMemory()) {
    profile = pdfake::drawerProfile(CompletionMechanism::GsParenH);
    driver.reset(new PrinterDriver(std::move(storage)));
  }

  void build() {
    PrinterConfig config;
    config.id = id;
    config.transport = link.factory();
    config.width_dots = escpos::kWidth80mm;
    config.profile = profile;
    printer = driver->addPrinter(config);
  }
};

}  // namespace

// --- a. The enum itself ------------------------------------------------------------

PD_TEST(drawer_states_are_a_closed_set_with_stable_values_and_spellings) {
  // The mirrors in pd.h and in four wrappers are written against these numbers, so a
  // reordering here has to break something loudly rather than silently renumber them.
  CHECK_EQ(kAllDrawerStates.size(), size_t{8});
  CHECK_EQ(static_cast<int>(DrawerState::Closed), 0);
  CHECK_EQ(static_cast<int>(DrawerState::Open), 1);
  CHECK_EQ(static_cast<int>(DrawerState::Opening), 2);
  CHECK_EQ(static_cast<int>(DrawerState::KickSentUnverified), 3);
  CHECK_EQ(static_cast<int>(DrawerState::OpenVerified), 4);
  CHECK_EQ(static_cast<int>(DrawerState::FailedToOpen), 5);
  CHECK_EQ(static_cast<int>(DrawerState::NoSensor), 6);
  CHECK_EQ(static_cast<int>(DrawerState::Unknown), 7);
  CHECK_EQ(std::string(to_string(DrawerState::KickSentUnverified)),
           std::string("KickSentUnverified"));
  CHECK_EQ(std::string(to_string(DrawerState::OpenVerified)), std::string("OpenVerified"));

  CHECK_EQ(kAllDrawerPortStandards.size(), size_t{4});
  CHECK_EQ(static_cast<int>(DrawerPortStandard::Unknown), 3);
  CHECK_EQ(std::string(to_string(DrawerPortStandard::Star24V6P6C)),
           std::string("Star24V6P6C"));

  CHECK_EQ(kAllDrawerKickMethods.size(), size_t{8});
  CHECK_EQ(static_cast<int>(DrawerKickMethod::EpsonEscP), 0);
  CHECK_EQ(static_cast<int>(DrawerKickMethod::Unsupported), 7);

  CHECK_EQ(kAllDrawerStatusMethods.size(), size_t{5});
  CHECK_EQ(std::string(to_string(DrawerStatusMethod::GsR2)), std::string("GsR2"));
}

// --- b. The wire ---------------------------------------------------------------------

PD_TEST(drawer_commands_are_the_bytes_the_command_reference_gives) {
  // docs/cash-drawer.md §1, Epson's own worked example: 1B 70 00 64 C8 is channel 1,
  // 200 ms on, 400 ms off, with both times in 2 ms units.
  CHECK_BYTES(escpos::drawerKick(1, 200), 0x1B, 0x70, 0x00, 0x64, 0xC8);
  // Channel 2 is m = 1, i.e. Epson pin 5.
  CHECK_BYTES(escpos::drawerKick(2, 100), 0x1B, 0x70, 0x01, 0x32, 0x64);
  // Saturating, never wrapping: 600 ms of ON time would be 300 units, which does not
  // fit in a byte, and a wrap would turn a long pulse into a 88 ms tap.
  CHECK_BYTES(escpos::drawerKick(1, 600), 0x1B, 0x70, 0x00, 0xFF, 0xFF);
  CHECK_THROWS(escpos::drawerKick(3, 200), escpos::EncodingError);
  CHECK_BYTES(escpos::gsDrawerStatus(), 0x1D, 0x72, 0x02);

  // Bit 0 of the GS r 2 answer is the sense pin and nothing else in the byte is read.
  CHECK(drawerPinHigh(0x01));
  CHECK(!drawerPinHigh(0x00));
  CHECK(drawerPinHigh(0x13));
}

PD_TEST(a_raw_pin_level_is_not_a_state_until_the_polarity_is_calibrated) {
  DrawerPolarity uncalibrated;
  CHECK_EQ(drawerStateFrom(true, uncalibrated), DrawerState::Unknown);
  CHECK_EQ(drawerStateFrom(false, uncalibrated), DrawerState::Unknown);

  DrawerPolarity high_open;
  high_open.calibrated = true;
  high_open.high_means_open = true;
  CHECK_EQ(drawerStateFrom(true, high_open), DrawerState::Open);
  CHECK_EQ(drawerStateFrom(false, high_open), DrawerState::Closed);

  // The same printer, the other drawer: Star's warning is that this is a property of
  // what is plugged in, so both answers have to be reachable.
  DrawerPolarity low_open;
  low_open.calibrated = true;
  low_open.high_means_open = false;
  CHECK_EQ(drawerStateFrom(true, low_open), DrawerState::Closed);
  CHECK_EQ(drawerStateFrom(false, low_open), DrawerState::Open);
}

PD_TEST(pulse_and_channel_requests_are_clamped_to_what_the_profile_documents) {
  DrawerCapabilities caps;
  caps.present = true;
  caps.electrical.standard = DrawerPortStandard::Epson24V6P6C;
  caps.electrical.channel_count = 2;
  caps.kick.method = DrawerKickMethod::EpsonEscP;
  caps.kick.default_pulse_ms = 200;
  caps.kick.max_pulse_ms = 500;

  CHECK_EQ(caps.pulseFor(0), uint16_t{200});     // 0 asks for the default
  CHECK_EQ(caps.pulseFor(120), uint16_t{120});
  CHECK_EQ(caps.pulseFor(9000), uint16_t{500});  // clamped, not truncated to a byte
  CHECK_EQ(caps.pulseFor(151), uint16_t{150});   // 2 ms units, rounded down
  CHECK_EQ(caps.channelFor(0), uint8_t{1});
  CHECK_EQ(caps.channelFor(2), uint8_t{2});
  CHECK_EQ(caps.channelFor(7), uint8_t{2});      // clamped to the documented count
}

// --- c. The fleet --------------------------------------------------------------------

PD_TEST(epson_is_the_reference_implementation_documented_end_to_end) {
  for (const std::string& name :
       {"epson_tm_t20iii", "epson_tm_t88v", "epson_tm_t88vii", "epson_tm_m30",
        "epson_tm_m50"}) {
    const DrawerCapabilities drawer = devices::byName(name).drawer;
    CHECK(drawer.present);
    CHECK_EQ(drawer.electrical.standard, DrawerPortStandard::Epson24V6P6C);
    CHECK_EQ(drawer.electrical.voltage, uint16_t{24});
    CHECK_EQ(drawer.electrical.max_current_ma, uint16_t{1000});
    CHECK_EQ(static_cast<int>(drawer.electrical.channel_count), 2);
    CHECK_EQ(static_cast<int>(drawer.electrical.sensor_pin), 3);
    CHECK_EQ(drawer.kick.method, DrawerKickMethod::EpsonEscP);
    CHECK_EQ(drawer.status.method, DrawerStatusMethod::GsR2);
    CHECK(drawer.status.available);
    CHECK(drawer.evidence.documented());
    CHECK(drawer.kickable());
    // Two drive outputs, one switch input: both channels kick, only one fact reads back.
    CHECK(drawer.status.shared_between_drawers);
    // The pulse that would go to the connector can sound the optional buzzer instead.
    CHECK(drawer.port.shared_with_buzzer);
  }
}

PD_TEST(star_looks_the_same_and_is_wired_the_other_way_round) {
  const DrawerCapabilities drawer = devices::star_tsp100().drawer;
  CHECK(drawer.present);
  CHECK_EQ(drawer.electrical.standard, DrawerPortStandard::Star24V6P6C);
  // The single fact this whole section of the document exists for.
  CHECK_EQ(static_cast<int>(drawer.electrical.sensor_pin), 6);
  CHECK_EQ(static_cast<int>(devices::epson_tm_t88v().drawer.electrical.sensor_pin), 3);
  CHECK_EQ(drawer.kick.method, DrawerKickMethod::StarPrnt);
  CHECK_EQ(drawer.status.method, DrawerStatusMethod::StarSignal);
  // Documented, and still not something this ESC/POS engine may drive.
  CHECK(drawer.evidence.documented());
  CHECK(!drawer.kickable());
  CHECK(!drawer.sensorReadable());
  CHECK_EQ(devices::star_mcprint2().drawer.electrical.standard,
           DrawerPortStandard::Star24V6P6C);
}

PD_TEST(citizen_documents_the_drawer_and_refuses_to_fire_it_while_printing) {
  const DrawerCapabilities drawer = devices::citizen_cts_58_80().drawer;
  CHECK(drawer.present);
  CHECK_EQ(drawer.kick.method, DrawerKickMethod::CitizenEscP);
  CHECK_EQ(drawer.electrical.standard, DrawerPortStandard::Epson24V6P6C);
  CHECK(drawer.evidence.documented());
  CHECK(drawer.kickable());
  CHECK(!drawer.kick.can_kick_during_print);
  CHECK(!drawer.note.empty());
  CHECK_EQ(devices::citizen_cts_wide().drawer.kick.can_kick_during_print, false);
}

PD_TEST(bixolon_defaults_to_its_own_sdks_two_hundred_milliseconds) {
  const DrawerCapabilities drawer = devices::bixolon_srp350().drawer;
  CHECK(drawer.present);
  CHECK_EQ(drawer.kick.default_pulse_ms, uint16_t{200});
  CHECK_EQ(drawer.kick.method, DrawerKickMethod::BixolonSdk);
  CHECK_EQ(drawer.status.method, DrawerStatusMethod::VendorSdk);
  CHECK_EQ(drawer.electrical.standard, DrawerPortStandard::Epson24V6P6C);
  // The port is Epson-like and documented; the command path is the vendor SDK, which
  // this engine does not speak, so it still refuses to emit anything.
  CHECK(drawer.evidence.documented());
  CHECK(!drawer.kickable());
}

PD_TEST(snbc_is_documented_on_both_halves_and_uses_the_queued_pulse) {
  const DrawerCapabilities drawer = devices::snbc_btp_r880().drawer;
  CHECK(drawer.present);
  CHECK_EQ(drawer.kick.method, DrawerKickMethod::SnbcEscP);
  CHECK_EQ(drawer.electrical.standard, DrawerPortStandard::Epson24V6P6C);
  CHECK_EQ(drawer.status.method, DrawerStatusMethod::GsR2);
  CHECK(drawer.evidence.documented());
  CHECK(drawer.kickable());
  CHECK(devices::exists("snbc_btp_r880"));
  // The sibling family shares the port classification and not the command manual.
  const DrawerCapabilities sibling = devices::snbc_btp_u80().drawer;
  CHECK_EQ(sibling.electrical.standard, DrawerPortStandard::Epson24V6P6C);
  CHECK_EQ(sibling.evidence.commands, Provenance::Unverified);
  CHECK(!sibling.status.available);
}

PD_TEST(electrical_documentation_and_command_documentation_are_separate_columns) {
  // The XP-S260M: DC 24 V / 1 A is in Xprinter's own specification, and nothing they
  // publish proves ESC p or the status read.
  const DrawerCapabilities xp = devices::xprinter_s_series().drawer;
  CHECK(xp.present);
  CHECK_EQ(xp.electrical.standard, DrawerPortStandard::Epson24V6P6C);
  CHECK_EQ(xp.evidence.electrical, Provenance::Documented);
  CHECK_EQ(xp.evidence.commands, Provenance::Unverified);
  CHECK(!xp.evidence.documented());
  CHECK(!xp.status.available);  // no verified open until a probe says otherwise
  CHECK(xp.kickable());         // the electrical gate is what decides a pulse

  // The same shape on the 80 mm Rongta line: the socket is model-verified, the command
  // semantics are not.
  const DrawerCapabilities rp80 = devices::rongta_rp80().drawer;
  CHECK_EQ(rp80.electrical.standard, DrawerPortStandard::Epson24V6P6C);
  CHECK_EQ(rp80.evidence.electrical, Provenance::Documented);
  CHECK_EQ(rp80.evidence.commands, Provenance::Unverified);

  // And on Sewoo, which the document explicitly forbids generalising to its OEM sibling.
  CHECK_EQ(devices::sewoo_slk_ts().drawer.electrical.standard,
           DrawerPortStandard::Epson24V6P6C);
  CHECK_EQ(devices::partner_rp110().drawer.electrical.standard,
           DrawerPortStandard::Unknown);
}

PD_TEST(unclassified_ports_are_refused_and_say_why) {
  // rongta_rp58: DO NOT INHERIT. The 80 mm family's 24 V figure is a fact about other
  // machines, and this size class includes 12 V drive circuits.
  const DrawerCapabilities rp58 = devices::rongta_rp58().drawer;
  CHECK(rp58.present);
  CHECK_EQ(rp58.electrical.standard, DrawerPortStandard::Unknown);
  CHECK_EQ(rp58.electrical.voltage, uint16_t{0});
  CHECK_EQ(rp58.kick.method, DrawerKickMethod::Unsupported);
  CHECK(!rp58.kickable());
  CHECK(rp58.note.find("DO NOT INHERIT") != std::string::npos);

  // Partner Tech is probe-required on every axis.
  CHECK(!devices::partner_rp110().drawer.kickable());
  CHECK(!devices::partner_rp710().drawer.kickable());

  // Generic 58 mm carries the 12 V warning by name.
  const DrawerCapabilities g58 = devices::generic_58().drawer;
  CHECK(!g58.kickable());
  CHECK(g58.note.find("12 V") != std::string::npos);
  CHECK(!devices::generic_80().drawer.kickable());
  CHECK(!devices::generic_unknown().drawer.kickable());
  // And the shipped default profile of the SDK claims no drawer at all.
  CHECK(!generic_escpos().drawer.present);
}

PD_TEST(label_printers_and_handhelds_have_no_drawer_at_all) {
  for (const std::string& name :
       {"zebra_zq500", "zebra_zq600_plus", "brother_rj3000", "brother_rj4000",
        "epson_tm_p80", "bixolon_spp_r310"}) {
    const DrawerCapabilities drawer = devices::byName(name).drawer;
    CHECK(!drawer.present);
    CHECK_EQ(drawer.kick.method, DrawerKickMethod::Unsupported);
    CHECK(!drawer.kickable());
  }
}

// --- d. Refusals write nothing --------------------------------------------------------

PD_TEST(a_zebra_or_brother_drawer_call_writes_zero_bytes) {
  for (const std::string& name : {"zebra_zq500", "brother_rj3000"}) {
    Rig rig;
    rig.profile = devices::byName(name);
    rig.build();
    const DrawerOpenResult result = rig.printer->openDrawer();
    CHECK_EQ(result.state, DrawerState::Unknown);
    CHECK_EQ(result.previous_state, DrawerState::Unknown);
    CHECK_EQ(result.pulse_ms, uint16_t{0});
    rig.printer->drain();
    // Not "a pulse that did nothing" — no pulse, and no connection either.
    CHECK_EQ(rig.link.device->drawerKicks(), size_t{0});
    CHECK_EQ(rig.link.stats->bytes.load(), size_t{0});
    CHECK_EQ(rig.link.stats->connects.load(), size_t{0});
  }
}

PD_TEST(an_unclassified_electrical_standard_refuses_the_pulse) {
  Rig rig;
  rig.profile = pdfake::drawerProfile(CompletionMechanism::GsParenH);
  // Everything else about this printer is fine — the method is ESC p, the sensor
  // answers — and the port has simply never been classified. That alone is a refusal:
  // the plug that fits is not proof of the pinout behind it.
  rig.profile.drawer.electrical.standard = DrawerPortStandard::Unknown;
  rig.build();
  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.state, DrawerState::Unknown);
  CHECK_EQ(result.pulse_ms, uint16_t{0});
  rig.printer->drain();
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{0});
  CHECK_EQ(rig.link.stats->bytes.load(), size_t{0});

  // The sensor is still readable, because reading a switch cannot energise anything.
  // This is what `pdctl drawer-probe` runs on hardware it must not pulse.
  const DrawerReading reading = rig.printer->readDrawerSensor(500ms);
  CHECK(reading.available);
  CHECK(reading.answered);
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{0});
}

// --- e. The opening sequence ------------------------------------------------------------

PD_TEST(a_verified_open_is_reported_only_when_the_switch_actually_moves) {
  Rig rig;
  rig.profile.drawer.status.polarity.calibrated = true;
  rig.profile.drawer.status.polarity.high_means_open = true;
  rig.build();

  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.previous_state, DrawerState::Closed);
  CHECK_EQ(result.state, DrawerState::OpenVerified);
  CHECK_EQ(result.channel, uint8_t{1});
  CHECK_EQ(result.pulse_ms, uint16_t{200});
  CHECK(rig.link.device->drawerOpen());

  const std::vector<pdfake::DrawerKickRecord> kicks = rig.link.device->drawerKickRecords();
  CHECK_EQ(kicks.size(), size_t{1});
  if (!kicks.empty()) {
    CHECK_EQ(static_cast<int>(kicks[0].channel), 1);
    CHECK_EQ(static_cast<int>(kicks[0].on_units), 100);  // 200 ms in 2 ms units
  }
  // Step 1 happened before the pulse and the watch happened after it, so the sensor
  // was asked at least twice.
  CHECK(rig.link.device->drawerStatusRequests() >= 2);
}

PD_TEST(a_drawer_that_stays_shut_is_failed_to_open_not_a_success) {
  Rig rig;
  rig.profile.drawer.status.polarity.calibrated = true;
  rig.profile.drawer.status.polarity.high_means_open = true;
  pdfake::Script script;
  // A locked drawer, a jam, a cut cable, a wrong channel: from this side of the
  // connector they are one observation.
  script.drawer_opens_on_kick = false;
  rig.link.device->setScript(script);
  rig.build();

  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.previous_state, DrawerState::Closed);
  CHECK_EQ(result.state, DrawerState::FailedToOpen);
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{1});
  // The verdict was reached inside the profile's verification window rather than
  // instantly, because a drawer takes time to swing.
  CHECK(result.elapsed_ms >= 100);
}

PD_TEST(a_drawer_that_is_already_open_is_never_pulsed_again) {
  Rig rig;
  rig.profile.drawer.status.polarity.calibrated = true;
  rig.profile.drawer.status.polarity.high_means_open = true;
  pdfake::Script script;
  script.drawer_open = true;
  rig.link.device->setScript(script);
  rig.build();

  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.previous_state, DrawerState::Open);
  CHECK_EQ(result.state, DrawerState::Open);
  CHECK_EQ(result.pulse_ms, uint16_t{0});
  CHECK_EQ(result.elapsed_ms, uint32_t{0});
  // Step 1 of the sequence exists precisely so this number is zero: energising a
  // solenoid against an already-open latch buys nothing and heats the coil.
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{0});
  CHECK_EQ(rig.link.device->drawerStatusRequests(), size_t{1});
}

PD_TEST(no_sensor_means_kick_sent_unverified_and_says_so_in_both_fields) {
  Rig rig;
  // A printer whose drawer port has no switch input wired or documented. The kick is
  // real; the confirmation does not exist.
  rig.profile.drawer.status.available = false;
  rig.profile.drawer.status.method = DrawerStatusMethod::None;
  rig.build();

  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.previous_state, DrawerState::NoSensor);
  CHECK_EQ(result.state, DrawerState::KickSentUnverified);
  CHECK_EQ(result.pulse_ms, uint16_t{200});
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{1});
  CHECK_EQ(rig.link.device->drawerStatusRequests(), size_t{0});
}

PD_TEST(a_sensor_whose_answer_never_comes_back_is_also_unverified) {
  // The print-server case of docs/cash-drawer.md: the kick travels forward while the
  // sensor response does not come back. The profile says there is a switch, and the
  // path swallows the answer, which is a different fact from "there is no switch" and
  // lands on the same honest verdict.
  Rig rig;
  pdfake::Script script;
  script.answer_drawer_status = false;
  rig.link.device->setScript(script);
  rig.build();

  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.previous_state, DrawerState::Unknown);
  CHECK_EQ(result.state, DrawerState::KickSentUnverified);
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{1});
  // It was asked. It just did not answer.
  CHECK(rig.link.device->drawerStatusRequests() >= 1);
}

PD_TEST(an_uncalibrated_polarity_verifies_movement_and_refuses_to_name_a_direction) {
  Rig rig;  // drawerProfile leaves the polarity uncalibrated
  rig.build();

  const DrawerOpenResult result = rig.printer->openDrawer();
  // The level before the pulse was read and deliberately not interpreted.
  CHECK_EQ(result.previous_state, DrawerState::Unknown);
  // The switch moved, which is the claim OpenVerified makes and the only one it makes.
  CHECK_EQ(result.state, DrawerState::OpenVerified);

  // And when the switch does not move, an uncalibrated printer cannot tell "already
  // open" from "never opened", so it reports neither.
  Rig locked;
  pdfake::Script script;
  script.drawer_opens_on_kick = false;
  locked.link.device->setScript(script);
  locked.build();
  const DrawerOpenResult stuck = locked.printer->openDrawer();
  CHECK_EQ(stuck.state, DrawerState::KickSentUnverified);
}

PD_TEST(the_requested_channel_is_the_one_energised_and_the_other_moves_nothing) {
  Rig rig;
  rig.profile.drawer.status.polarity.calibrated = true;
  rig.profile.drawer.status.polarity.high_means_open = true;
  pdfake::Script script;
  script.drawer_wired_channel = 2;
  rig.link.device->setScript(script);
  rig.build();

  DrawerRequest wrong;
  wrong.channel = 1;
  wrong.pulse_ms = 120;
  const DrawerOpenResult missed = rig.printer->openDrawer(wrong);
  CHECK_EQ(missed.state, DrawerState::FailedToOpen);

  DrawerRequest right;
  right.channel = 2;
  right.pulse_ms = 120;
  const DrawerOpenResult hit = rig.printer->openDrawer(right);
  CHECK_EQ(hit.state, DrawerState::OpenVerified);
  CHECK_EQ(hit.pulse_ms, uint16_t{120});

  const std::vector<pdfake::DrawerKickRecord> kicks = rig.link.device->drawerKickRecords();
  CHECK_EQ(kicks.size(), size_t{2});
  if (kicks.size() == 2) {
    CHECK_EQ(static_cast<int>(kicks[0].channel), 1);
    CHECK_EQ(static_cast<int>(kicks[1].channel), 2);
    CHECK_EQ(static_cast<int>(kicks[1].on_units), 60);  // 120 ms
  }
}

PD_TEST(the_manufacturer_cooldown_is_held_between_two_pulses) {
  Rig rig;
  rig.profile.drawer.kick.cooldown_ms = 250;
  rig.profile.drawer.status.available = false;  // isolate the cooldown from the watch
  rig.profile.drawer.status.method = DrawerStatusMethod::None;
  rig.build();

  const auto started = MonotonicClock::now();
  const DrawerOpenResult first = rig.printer->openDrawer();
  const DrawerOpenResult second = rig.printer->openDrawer();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           MonotonicClock::now() - started)
                           .count();
  CHECK_EQ(first.state, DrawerState::KickSentUnverified);
  CHECK_EQ(second.state, DrawerState::KickSentUnverified);
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{2});
  // The second pulse waited out the coil's rest. Measured across both calls, so the
  // only way to be under the cooldown is to have skipped it.
  CHECK(elapsed >= 250);
}

PD_TEST(a_pulse_never_lands_inside_a_receipt_and_can_be_ordered_behind_one) {
  // docs/cash-drawer.md step 2 and §3. The peripheral lane is the printer's own
  // worker, so a pulse cannot interleave with a fenced job's bytes whatever the
  // profile says; what canKickDuringPrint decides is whether it may go in front of
  // jobs that are merely queued.
  for (const bool during_print : {true, false}) {
    Rig rig;
    rig.profile.drawer.kick.can_kick_during_print = during_print;
    rig.profile.drawer.status.available = false;
    rig.profile.drawer.status.method = DrawerStatusMethod::None;

    // Hold the first job inside its very first write, so the queue behind it is
    // observable instead of raced for.
    std::mutex mutex;
    std::condition_variable released;
    bool release = false;
    bool holding = false;
    rig.link.behaviour.before_write = [&](const uint8_t*, size_t) {
      std::unique_lock<std::mutex> lock(mutex);
      if (holding) {
        return;
      }
      holding = true;
      released.wait(lock, [&] { return release; });
    };
    rig.build();

    auto blocker = rig.printer->print(Payload::raw(textPayload("HELD JOB")));
    // Wait until the worker is genuinely inside the held write.
    for (int i = 0; i < 200; ++i) {
      std::lock_guard<std::mutex> lock(mutex);
      if (holding) {
        break;
      }
      std::this_thread::sleep_for(5ms);
    }

    DrawerOpenResult drawer_result;
    std::thread opener([&] { drawer_result = rig.printer->openDrawer(); });
    // Give the drawer task time to reach the queue, then put a second job behind it.
    std::this_thread::sleep_for(30ms);
    auto follower = rig.printer->print(Payload::raw(textPayload("FOLLOWER JOB")));
    std::this_thread::sleep_for(30ms);
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    released.notify_all();
    opener.join();
    blocker->result();
    follower->result();
    rig.printer->drain();

    CHECK_EQ(drawer_result.state, DrawerState::KickSentUnverified);
    const std::vector<pdfake::DrawerKickRecord> kicks =
        rig.link.device->drawerKickRecords();
    CHECK_EQ(kicks.size(), size_t{1});
    const std::string printed = rig.link.device->printText();
    CHECK(printed.find("HELD JOB") != std::string::npos);
    CHECK(printed.find("FOLLOWER JOB") != std::string::npos);
    if (!kicks.empty()) {
      const size_t held_end = printed.find("HELD JOB") + 8;
      const size_t follower_start = printed.find("FOLLOWER JOB");
      // Never inside the held receipt: the pulse was reached only after all of that
      // job's print data had been consumed.
      CHECK(kicks[0].print_data_len >= held_end);
      if (during_print) {
        // Allowed to fire while the mechanism prints, so it does not wait behind a
        // queue of tickets a cashier is not holding.
        CHECK(kicks[0].print_data_len <= follower_start);
      } else {
        // The Citizen rule: finish the receipt, then pulse.
        CHECK(kicks[0].print_data_len >= follower_start);
      }
    }
  }
}

// --- f. Calibration ------------------------------------------------------------------

PD_TEST(an_uncalibrated_sensor_read_reports_the_level_and_the_missing_calibration) {
  Rig rig;
  rig.build();
  const DrawerReading reading = rig.printer->readDrawerSensor(500ms);
  CHECK(reading.available);
  CHECK(reading.answered);
  CHECK(reading.needs_calibration);
  CHECK(reading.pin_high.has_value());
  CHECK_EQ(reading.pin_high.value_or(true), false);  // the fixture starts shut, pin low
  // The raw level is reported; the interpretation is withheld, which is the whole
  // point of Star's warning.
  CHECK_EQ(reading.state, DrawerState::Unknown);
  CHECK_EQ(rig.link.device->drawerKicks(), size_t{0});
}

PD_TEST(calibration_turns_levels_into_states_and_survives_a_restart) {
  pdfake::TempDir dir("drawer-polarity");
  {
    Rig rig(StorageConfig::at(dir.path()));
    rig.build();
    CHECK(!rig.printer->drawerPolarity().calibrated);

    // The interactive procedure of `pdctl drawer-probe`, without the prompts: close
    // the drawer, read, open the drawer, read, and record which level meant open.
    rig.link.device->setDrawerOpen(false);
    const DrawerReading closed = rig.printer->readDrawerSensor(500ms);
    rig.link.device->setDrawerOpen(true);
    const DrawerReading opened = rig.printer->readDrawerSensor(500ms);
    CHECK(closed.answered);
    CHECK(opened.answered);
    CHECK(closed.pin_high.has_value() && opened.pin_high.has_value());
    CHECK(closed.pin_high != opened.pin_high);  // a switch that moved

    CHECK(rig.printer->calibrateDrawerPolarity(opened.pin_high.value_or(true)));
    CHECK(rig.printer->drawerPolarity().calibrated);
    CHECK(rig.printer->drawerPolarity().high_means_open);

    const DrawerReading now = rig.printer->readDrawerSensor(500ms);
    CHECK(!now.needs_calibration);
    CHECK_EQ(now.state, DrawerState::Open);
  }

  // A second driver over the same storage directory: the calibration describes the
  // installation, not the process that measured it.
  {
    Rig rig(StorageConfig::at(dir.path()));
    rig.build();
    CHECK(rig.printer->drawerPolarity().calibrated);
    CHECK(rig.printer->drawerPolarity().high_means_open);
    rig.link.device->setDrawerOpen(true);
    const DrawerReading reading = rig.printer->readDrawerSensor(500ms);
    CHECK(!reading.needs_calibration);
    CHECK_EQ(reading.state, DrawerState::Open);
  }
}

PD_TEST(a_reversed_switch_calibrates_to_the_other_polarity) {
  pdfake::TempDir dir("drawer-polarity-inverted");
  Rig rig(StorageConfig::at(dir.path()));
  pdfake::Script script;
  script.drawer_pin_high_when_open = false;  // the other drawer on the same printer
  rig.link.device->setScript(script);
  rig.build();

  rig.link.device->setDrawerOpen(true);
  const DrawerReading opened = rig.printer->readDrawerSensor(500ms);
  CHECK_EQ(opened.pin_high.value_or(true), false);
  CHECK(rig.printer->calibrateDrawerPolarity(opened.pin_high.value_or(false)));
  CHECK(!rig.printer->drawerPolarity().high_means_open);

  rig.link.device->setDrawerOpen(false);
  const DrawerReading shut = rig.printer->readDrawerSensor(500ms);
  CHECK_EQ(shut.state, DrawerState::Closed);

  // And the verified open works from that calibration, in the opposite direction.
  const DrawerOpenResult result = rig.printer->openDrawer();
  CHECK_EQ(result.previous_state, DrawerState::Closed);
  CHECK_EQ(result.state, DrawerState::OpenVerified);
}

PD_TEST(the_polarity_store_keeps_one_answer_per_printer) {
  pdfake::TempDir dir("drawer-store");
  {
    DrawerPolarityStore store(dir.path());
    CHECK(store.persistent());
    CHECK(!store.find("counter-1").has_value());
    store.save("counter-1", true);
    store.save("counter 2 with spaces", false);
    store.save("counter-1", false);  // recalibrated, not appended
    CHECK_EQ(store.size(), size_t{2});
  }
  {
    DrawerPolarityStore store(dir.path());
    CHECK_EQ(store.size(), size_t{2});
    CHECK_EQ(store.find("counter-1").value_or(true), false);
    CHECK_EQ(store.find("counter 2 with spaces").value_or(true), false);
    CHECK(!store.find("counter-3").has_value());
  }
  // An in-memory store answers nothing and never claims to have kept anything.
  DrawerPolarityStore memory("");
  CHECK(!memory.persistent());
  memory.save("counter-1", true);
  CHECK(memory.find("counter-1").value_or(false));
}
