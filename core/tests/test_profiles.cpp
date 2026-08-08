#include <set>
#include <string>

#include "fake_printer.hpp"
#include "printerdriver/capability_probe.hpp"
#include "printerdriver/device_profiles.hpp"
#include "test_harness.hpp"

using namespace pd;

namespace {

// Drives a CapabilityProbe against the in-process device, wiring the response path
// the way a transport would.
CapabilityFindings probeDevice(pdfake::FakePrinter& device, ProbeOptions options = {}) {
  options.status_timeout_ms = 150;
  options.identity_timeout_ms = 150;
  options.completion_timeout_ms = 300;
  CapabilityProbe probe(options);
  return probe.run([&device, &probe](const escpos::Bytes& bytes) {
    const std::vector<uint8_t> response = device.receive(bytes.data(), bytes.size());
    if (!response.empty()) {
      probe.onBytes(response.data(), response.size());
    }
    return true;
  });
}

}  // namespace

// --- Compositional profile ------------------------------------------------------------

PD_TEST(profile_facets_are_independent_of_each_other) {
  CapabilityProfile profile = devices::epson_tm_t88();
  CHECK_EQ(profile.completion, CompletionMechanism::GsParenH);
  CHECK(profile.status.dle_eot);
  CHECK(profile.status.extended_asb);
  CHECK(profile.recovery.dle_enq_resume);
  CHECK_EQ(profile.media.printable_width_dots, escpos::kWidth80mm);

  // Turning off one facet changes nothing about the others: capabilities compose,
  // they do not inherit (docs/capability-profiles.md).
  profile.status.asb = false;
  CHECK_EQ(profile.completion, CompletionMechanism::GsParenH);
  CHECK(profile.status.dle_eot);
  CHECK_EQ(profile.maxConfidence(), ConfidenceLevel::CutFaultFree);
}

PD_TEST(profile_identity_starts_untrusted) {
  for (const std::string& name : devices::names()) {
    const CapabilityProfile profile = devices::byName(name);
    // A shipped profile is a default, never a fingerprint: nothing in the database
    // may claim a trusted identity before a device has been interrogated.
    CHECK(!profile.identity.trusted);
    CHECK_EQ(static_cast<int>(profile.identity.fingerprint_confidence), 0);
    CHECK(!profile.probed);
  }
}

PD_TEST(profile_ceilings_and_evidence_follow_the_mechanism) {
  CHECK_EQ(evidenceFor(CompletionMechanism::GsParenH).grade,
           ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(evidenceFor(CompletionMechanism::GsParenH).authority,
           CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(std::string(evidenceFor(CompletionMechanism::GsParenH).method),
           std::string("GS(H) fn48"));

  CHECK_EQ(evidenceFor(CompletionMechanism::GsR1).grade,
           ConfidenceGrade::B_OrderedDeviceResponse);
  CHECK_EQ(evidenceFor(CompletionMechanism::GsR1).authority,
           CompletionAuthority::PhysicalPrinter);

  CHECK_EQ(evidenceFor(CompletionMechanism::None).grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(evidenceFor(CompletionMechanism::None).authority,
           CompletionAuthority::TransportOnly);

  // The ePOS answer comes from the printer's spooler, not from the mechanism that
  // moved the paper, and the authority says so.
  CHECK_EQ(evidenceFor(CompletionMechanism::EposJobId).authority,
           CompletionAuthority::VendorSpooler);
  CHECK_EQ(evidenceFor(CompletionMechanism::StarCheckedBlock).grade,
           ConfidenceGrade::A_JobLevelConfirmation);

  CHECK_EQ(std::string(gradeLetter(ConfidenceGrade::A_JobLevelConfirmation)),
           std::string("A"));
  CHECK_EQ(std::string(gradeLetter(ConfidenceGrade::E_TransportOnly)), std::string("E"));
}

PD_TEST(profile_only_escpos_mechanisms_are_drivable_here) {
  CHECK(isDrivableByEscposEngine(CompletionMechanism::GsParenH));
  CHECK(isDrivableByEscposEngine(CompletionMechanism::GsR1));
  CHECK(isDrivableByEscposEngine(CompletionMechanism::None));
  CHECK(!isDrivableByEscposEngine(CompletionMechanism::StarCheckedBlock));
  CHECK(!isDrivableByEscposEngine(CompletionMechanism::EposJobId));
  CHECK(!isDrivableByEscposEngine(CompletionMechanism::VendorIdle));

  CHECK(devices::generic_80().drivableByEscposEngine());
  CHECK(devices::epson_tm_t20().drivableByEscposEngine());
  // Language and mechanism are separate reasons to refuse, and a Star profile is
  // both: StarPRNT bytes and a checked block this core does not speak.
  CHECK_EQ(devices::star_tsp100().language, CommandLanguage::StarPrnt);
  CHECK(!devices::star_tsp100().drivableByEscposEngine());
  CapabilityProfile mislabelled = devices::star_tsp650();
  mislabelled.completion = CompletionMechanism::GsR1;
  CHECK(!mislabelled.drivableByEscposEngine());
  CHECK_EQ(std::string(to_string(CommandLanguage::StarPrnt)), std::string("StarPrnt"));
  CHECK_EQ(devices::epson_tm_t20().language, CommandLanguage::EscPos);
}

// --- Device database --------------------------------------------------------------------

PD_TEST(device_database_names_are_unique_and_resolvable) {
  const std::vector<std::string> names = devices::names();
  CHECK(names.size() >= 24);
  std::set<std::string> seen;
  for (const std::string& name : names) {
    CHECK(seen.insert(name).second);
    CHECK(devices::exists(name));
    CHECK_EQ(devices::byName(name).name, name);
  }
  // An unknown name is an unknown device, which is what generic_80 means.
  CHECK(!devices::exists("epson_tm_t99"));
  CHECK_EQ(devices::byName("epson_tm_t99").name, std::string("generic_80"));
}

PD_TEST(device_database_media_facts_are_recorded_not_derived) {
  // 80 mm media, 72 mm printable, 576 dots — three separate facts.
  const CapabilityProfile eighty = devices::epson_tm_t20();
  CHECK_EQ(static_cast<int>(eighty.media.nominal_roll_width_mm), 80);
  CHECK_EQ(eighty.media.printable_width_dots, escpos::kWidth80mm);
  CHECK_EQ(static_cast<int>(eighty.media.dpi), 203);

  const CapabilityProfile fifty_eight = devices::generic_58();
  CHECK_EQ(static_cast<int>(fifty_eight.media.nominal_roll_width_mm), 58);
  CHECK_EQ(fifty_eight.media.printable_width_dots, escpos::kWidth58mm);
  CHECK(!fifty_eight.media.full_cut);

  // 112 mm media, 104 mm printable: deriving one from the other clips every receipt.
  const CapabilityProfile wide = devices::citizen_cts_wide();
  CHECK_EQ(static_cast<int>(wide.media.nominal_roll_width_mm), 112);
  CHECK_EQ(wide.media.printable_width_dots, escpos::kWidth104mm);
  CHECK_EQ(static_cast<int>(wide.media.dpi), 203);

  // Dot impact: 76 mm paper at 160 dpi, nothing like the thermal 8-dots-per-mm rule.
  const CapabilityProfile impact = devices::epson_tm_u220();
  CHECK_EQ(static_cast<int>(impact.media.nominal_roll_width_mm), 76);
  CHECK_EQ(impact.media.printable_width_dots, 400u);
  CHECK_EQ(static_cast<int>(impact.media.dpi), 160);
  CHECK(!impact.media.full_cut);
  CHECK_EQ(impact.completion, CompletionMechanism::GsR1);
}

PD_TEST(device_database_records_the_researched_per_family_facts) {
  // Epson: GS ( H fn 48 is in the published command tables.
  CHECK_EQ(devices::epson_tm_t20().completion, CompletionMechanism::GsParenH);
  CHECK_EQ(devices::epson_tm_t70().completion, CompletionMechanism::GsParenH);
  CHECK(devices::epson_tm_i().completion_caps.epos_job_id);
  CHECK(devices::epson_tm_i().transport.epos);

  // Rongta RP80: the manual documents GS ( H, and it documents GS I lying.
  const CapabilityProfile rp80 = devices::rongta_rp80();
  CHECK(rp80.completion_caps.try_process_id_gs_h);
  CHECK(rp80.quirks.unreliable_identity);
  CHECK(rp80.recovery.dle_enq_resume);
  CHECK(rp80.recovery.dle_enq_clear);
  CHECK_EQ(rp80.code_page, escpos::CodePage::PC852);
  CHECK_EQ(static_cast<int>(rp80.final_feed_lines), 6);
  CHECK_EQ(devices::rongta_rp58().media.printable_width_dots, escpos::kWidth58mm);
  CHECK(devices::rongta_rp3xx().completion_caps.try_process_id_gs_h);

  // Xprinter S series: GS ( H hardware-confirmed on the XP-S260M.
  CHECK_EQ(devices::xprinter_s_series().completion, CompletionMechanism::GsParenH);
  CHECK(devices::xprinter_s_series().completion_caps.process_id_gs_h);
  CHECK_EQ(devices::xprinter_pos58().media.printable_width_dots, escpos::kWidth58mm);
  CHECK(devices::xprinter_pos80().completion_caps.try_process_id_gs_h);

  // Partner RP-110 is the Sewoo platform, with GS ( H still a probe question.
  CHECK_EQ(devices::partner_rp110().media.printable_width_dots, escpos::kWidth80mm);
  CHECK(devices::partner_rp110().completion_caps.try_process_id_gs_h);
  CHECK(!devices::partner_rp110().completion_caps.process_id_gs_h);
  CHECK_EQ(devices::sewoo_slk_ts().identity.vendor, std::string("Sewoo"));

  // Star: checked block, StarPRNT parser, no ESC/POS status assumptions.
  for (const CapabilityProfile& star :
       {devices::star_tsp100(), devices::star_tsp650(), devices::star_mcprint()}) {
    CHECK_EQ(star.completion, CompletionMechanism::StarCheckedBlock);
    CHECK_EQ(star.quirks.response_parser, ResponseParserVariant::StarPrnt);
    CHECK(!star.status.dle_eot);
    CHECK(star.completion_caps.prefer_vendor_sdk);
  }

  // Bixolon: vendor SDK first, GS ( H never assumed from "ESC/POS compatible".
  for (const CapabilityProfile& bixolon :
       {devices::bixolon_srp330(), devices::bixolon_srp350(), devices::bixolon_srp380(),
        devices::bixolon_q_series()}) {
    CHECK(bixolon.completion_caps.prefer_vendor_sdk);
    CHECK(!bixolon.completion_caps.process_id_gs_h);
    CHECK_EQ(bixolon.completion, CompletionMechanism::GsR1);
  }

  CHECK_EQ(devices::citizen_cts_58_80().completion, CompletionMechanism::GsR1);
  CHECK(devices::citizen_cts_fast().media.near_end_sensor);
}

// --- Probe-then-promote --------------------------------------------------------------

PD_TEST(promote_lets_findings_win_over_profile_defaults) {
  CapabilityFindings findings;
  findings.gs_h_process_id = true;
  findings.gs_r1 = true;
  findings.dle_eot = true;
  findings.asb = true;
  findings.cutter_error_status = true;

  // An unknown £35 clone is promoted by what it actually does, not by its name.
  const CapabilityProfile promoted = promote(devices::generic_80(), findings);
  CHECK_EQ(promoted.completion, CompletionMechanism::GsParenH);
  CHECK(promoted.completion_caps.process_id_gs_h);
  CHECK(promoted.probed);
  CHECK_EQ(promoted.maxConfidence(), ConfidenceLevel::CutFaultFree);
  // Facets the probe said nothing about keep their defaults.
  CHECK_EQ(promoted.media.printable_width_dots, escpos::kWidth80mm);
  CHECK_EQ(promoted.code_page, devices::generic_80().code_page);
}

PD_TEST(promote_demotes_a_profile_the_hardware_contradicts) {
  CapabilityFindings findings;
  findings.gs_h_process_id = false;
  findings.gs_r1 = true;
  findings.dle_eot = false;
  findings.asb = false;

  // An "identical" unit on other firmware: the shipped Epson defaults are wrong for
  // it, and the probe is what says so.
  const CapabilityProfile promoted = promote(devices::epson_tm_t20(), findings);
  CHECK_EQ(promoted.completion, CompletionMechanism::GsR1);
  CHECK(!promoted.status.dle_eot);
  CHECK(!promoted.status.asb);
  CHECK_EQ(promoted.maxConfidence(), ConfidenceLevel::CutProcessed);
}

PD_TEST(promote_leaves_an_unprobed_facet_alone) {
  CapabilityFindings findings;
  findings.gs_h_process_id = true;
  // Nothing established about ASB, so the profile's own answer survives: silence is
  // never evidence.
  const CapabilityProfile promoted = promote(devices::rongta_rp80(), findings);
  CHECK_EQ(promoted.status.asb, devices::rongta_rp80().status.asb);
  CHECK_EQ(promoted.status.dle_eot, devices::rongta_rp80().status.dle_eot);
  CHECK_EQ(promoted.completion, CompletionMechanism::GsParenH);

  // Empty findings change nothing at all, including the probed flag.
  const CapabilityProfile untouched = promote(devices::rongta_rp80(), CapabilityFindings{});
  CHECK(!untouched.probed);
  CHECK_EQ(untouched.completion, devices::rongta_rp80().completion);
}

PD_TEST(promote_records_an_untrusted_identity_as_a_quirk) {
  CapabilityFindings findings;
  findings.gs_i = true;
  findings.reported.manufacturer = "EPOSN";
  findings.reported.model = "TM-T88V";
  findings.gs_h_process_id = true;
  findings.dle_eot = true;

  const CapabilityProfile promoted = promote(devices::generic_80(), findings);
  CHECK_EQ(promoted.identity.vendor, std::string("Rongta"));
  CHECK_EQ(promoted.identity.model, std::string("TM-T88V"));
  CHECK(!promoted.identity.trusted);
  CHECK(promoted.quirks.unreliable_identity);
  CHECK(promoted.identity.fingerprint_confidence > 0);
}

PD_TEST(completion_from_findings_needs_both_answers_to_conclude_none) {
  CapabilityFindings findings;
  findings.gs_h_process_id = false;
  CHECK(!completionFrom(findings).has_value());  // GS r was never tried

  findings.gs_r1 = false;
  CHECK(completionFrom(findings).has_value());
  CHECK_EQ(*completionFrom(findings), CompletionMechanism::None);

  findings.gs_r1 = true;
  CHECK_EQ(*completionFrom(findings), CompletionMechanism::GsR1);
  findings.gs_h_process_id = true;
  CHECK_EQ(*completionFrom(findings), CompletionMechanism::GsParenH);
}

// --- The probe against a device ---------------------------------------------------------

PD_TEST(probe_establishes_capabilities_and_never_sends_a_destructive_command) {
  pdfake::Script script;
  script.answer_identity = true;  // the EPOSN / TM-T88V impersonation by default
  auto device = std::make_shared<pdfake::FakePrinter>(script);
  const CapabilityFindings findings = probeDevice(*device);

  CHECK_EQ(findings.dle_eot.value_or(false), true);
  CHECK_EQ(findings.gs_h_process_id.value_or(false), true);
  CHECK_EQ(findings.gs_r1.value_or(false), true);
  CHECK_EQ(findings.asb.value_or(false), true);
  CHECK_EQ(findings.gs_i.value_or(false), true);
  CHECK_EQ(findings.cutter_error_status.value_or(false), true);
  CHECK(findings.completion.has_value());
  CHECK_EQ(*findings.completion, CompletionMechanism::GsParenH);

  CHECK_EQ(findings.reported.manufacturer, std::string("EPOSN"));
  CHECK_EQ(findings.reported.model, std::string("TM-T88V"));
  CHECK_EQ(findings.reported.firmware, std::string("1.02"));
  CHECK_EQ(findings.reported.model_id.value_or(0), static_cast<uint8_t>(0x20));
  CHECK_EQ(findings.key, std::string("tm_t88v-1_02"));

  CHECK_EQ(device->realtimeRequests().size(), static_cast<size_t>(4));
  CHECK_EQ(device->identityRequests().size(), static_cast<size_t>(7));
  CHECK_EQ(device->asbEnables(), static_cast<size_t>(1));
  // Strictly non-destructive: no cut, and none of the recovery or power commands.
  CHECK_EQ(device->cuts(), static_cast<size_t>(0));
  CHECK(!device->receivedContains(std::string("\x10\x05", 2)));  // DLE ENQ
  CHECK(!device->receivedContains(std::string("\x10\x14", 2)));  // DLE DC4
}

PD_TEST(probe_of_a_silent_device_concludes_no_ordered_fence) {
  pdfake::Script script;
  script.answer_realtime = false;
  script.answer_process_id = false;
  script.answer_queued_status = false;
  script.answer_asb = false;
  script.answer_identity = false;
  auto device = std::make_shared<pdfake::FakePrinter>(script);
  const CapabilityFindings findings = probeDevice(*device);

  CHECK_EQ(findings.dle_eot.value_or(true), false);
  CHECK_EQ(findings.gs_h_process_id.value_or(true), false);
  CHECK_EQ(findings.gs_r1.value_or(true), false);
  CHECK(findings.completion.has_value());
  CHECK_EQ(*findings.completion, CompletionMechanism::None);

  // A write-only device caps every job at TransportAccepted, and the promoted
  // profile says so instead of the shipped default's promise.
  const CapabilityProfile promoted = promote(devices::xprinter_s_series(), findings);
  CHECK_EQ(promoted.completion, CompletionMechanism::None);
  CHECK_EQ(promoted.maxConfidence(), ConfidenceLevel::TransportAccepted);
}

PD_TEST(probe_reads_identity_from_clone_framing_without_the_header) {
  pdfake::Script script;
  script.answer_identity = true;
  script.gs_i_header = false;
  script.gs_i_manufacturer = "RONGTA";
  script.gs_i_model = "RP326";
  script.gs_i_serial = "RT99001";
  auto device = std::make_shared<pdfake::FakePrinter>(script);
  const CapabilityFindings findings = probeDevice(*device);

  CHECK_EQ(findings.reported.manufacturer, std::string("RONGTA"));
  CHECK_EQ(findings.reported.model, std::string("RP326"));
  CHECK_EQ(findings.reported.serial, std::string("RT99001"));
  CHECK_EQ(findings.key, std::string("rp326-1_02-rt99001"));

  const IdentityAssessment assessment =
      identify(findings.reported, IdentityHints{}, findings.behaviour());
  CHECK_EQ(assessment.vendor_guess, std::string("Rongta"));
  CHECK_EQ(assessment.profile_guess, std::string("rongta_rp3xx"));
  CHECK(!assessment.identity_trusted);
}

// --- Findings persistence -----------------------------------------------------------------

PD_TEST(findings_serialize_and_parse_back_identically) {
  CapabilityFindings findings;
  findings.key = "tm_t88v-1_02-x5k0032";
  findings.endpoint = "tcp://192.0.2.10:9100";
  findings.recorded_unix_ms = 1754600000000ull;
  findings.dle_eot = true;
  findings.dle_eot_1 = true;
  findings.dle_eot_2 = true;
  findings.dle_eot_3 = true;
  findings.dle_eot_4 = false;
  findings.gs_r1 = true;
  findings.gs_h_process_id = true;
  findings.asb = false;
  findings.gs_i = true;
  findings.cutter_error_status = true;
  findings.completion = CompletionMechanism::GsParenH;
  findings.reported.manufacturer = "EPOSN";
  findings.reported.model = "TM-T88V";
  findings.reported.firmware = "1.02";
  findings.reported.serial = "X5K0032";
  findings.reported.model_id = 0x20;
  findings.unclassified = {0x11, 0xF0};

  const std::optional<CapabilityFindings> back =
      parseFindings(serializeFindings(findings));
  CHECK(back.has_value());
  CHECK_EQ(back->key, findings.key);
  CHECK_EQ(back->endpoint, findings.endpoint);
  CHECK_EQ(back->recorded_unix_ms, findings.recorded_unix_ms);
  CHECK_EQ(back->dle_eot_4.value_or(true), false);
  CHECK_EQ(back->asb.value_or(true), false);
  CHECK_EQ(back->gs_h_process_id.value_or(false), true);
  CHECK(back->completion.has_value());
  CHECK_EQ(*back->completion, CompletionMechanism::GsParenH);
  CHECK_EQ(back->reported.manufacturer, std::string("EPOSN"));
  CHECK_EQ(back->reported.serial, std::string("X5K0032"));
  CHECK_EQ(back->reported.model_id.value_or(0), static_cast<uint8_t>(0x20));
  CHECK_EQ(back->unclassified.size(), static_cast<size_t>(2));
  // Fields nobody established stay unestablished across the round trip.
  CHECK(!back->reported.type_id.has_value());
  CHECK(parseFindings("") == std::nullopt);
  CHECK(parseFindings("garbage without a key") == std::nullopt);
}

PD_TEST(findings_store_persists_across_a_restart_and_replaces_by_key) {
  pdfake::TempDir dir("findings-store");
  CapabilityFindings findings;
  findings.key = "rp326-1_02-rt99001";
  findings.endpoint = "tcp://192.0.2.11:9100";
  findings.gs_h_process_id = true;
  findings.gs_r1 = true;
  findings.dle_eot = true;

  {
    FindingsStore store(dir.path());
    CHECK(store.persistent());
    store.save(findings);
    CHECK_EQ(store.size(), static_cast<size_t>(1));
  }
  {
    FindingsStore reopened(dir.path());
    CHECK_EQ(reopened.size(), static_cast<size_t>(1));
    const std::optional<CapabilityFindings> found = reopened.find(findings.key);
    CHECK(found.has_value());
    CHECK_EQ(found->gs_h_process_id.value_or(false), true);
    // Found by address too, which is the only handle available before a device has
    // been interrogated.
    CHECK(reopened.findByEndpoint(findings.endpoint).has_value());
    CHECK(!reopened.find("no-such-printer").has_value());

    // Re-probing the same device replaces its record instead of appending.
    CapabilityFindings updated = findings;
    updated.gs_h_process_id = false;
    reopened.save(updated);
    CHECK_EQ(reopened.size(), static_cast<size_t>(1));
    CHECK_EQ(reopened.find(findings.key)->gs_h_process_id.value_or(true), false);
  }

  // An empty directory means no file and no persistence, matching StorageConfig.
  FindingsStore memory("");
  CHECK(!memory.persistent());
  memory.save(findings);
  CHECK_EQ(memory.size(), static_cast<size_t>(1));
}
