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
  // Language and mechanism are separate reasons to refuse. M13b: the Star desktops are
  // driven in Star Line Mode with a documented raw fence, so they are refused by the
  // *ESC/POS* engine and accepted by the Star one — which is the distinction this pair of
  // predicates exists to keep.
  CHECK_EQ(devices::star_tsp100().language, CommandLanguage::StarLine);
  CHECK(devices::star_tsp100().languages.has(CommandLanguage::StarPrnt));
  CHECK(!devices::star_tsp100().drivableByEscposEngine());
  CHECK(devices::star_tsp100().drivableByStarEngine());
  // The SDK-first portables are still refused by both: beginCheckedBlock is an SDK call,
  // not a wire primitive.
  CHECK(!devices::star_sm_s230().drivableByEscposEngine());
  CHECK(!devices::star_sm_s230().drivableByStarEngine());
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

  // Rongta RP80: the probe should still ask about GS ( H, and the family documents
  // GS I lying. What the family does NOT do any more is claim the extension —
  // docs/compatibility-brief.md §13 withdrew that, see the provenance test below.
  const CapabilityProfile rp80 = devices::rongta_rp80();
  CHECK(rp80.completion_caps.try_process_id_gs_h);
  CHECK(rp80.quirks.unreliable_identity);
  CHECK(rp80.recovery.dle_enq_resume);
  CHECK(rp80.recovery.dle_enq_clear);
  CHECK_EQ(rp80.code_page, escpos::CodePage::PC852);
  CHECK_EQ(static_cast<int>(rp80.final_feed_lines), 6);
  CHECK_EQ(devices::rongta_rp58().media.printable_width_dots, escpos::kWidth58mm);
  CHECK(devices::rongta_rp3xx().completion_caps.try_process_id_gs_h);

  // Xprinter S series, CORRECTED per docs/compatibility-brief.md §14 and §28. Xprinter
  // does not document GS ( H, so the *family* default must not claim it — one probed
  // XP-S260M is not a family, and firmware varies across the line. The hardware finding
  // lives in xp_s260m(), which carries Provenance::Probed, and in the findings store.
  // The family still asks the question on every probe.
  CHECK_EQ(devices::xprinter_s_series().completion, CompletionMechanism::GsR1);
  CHECK(!devices::xprinter_s_series().completion_caps.process_id_gs_h);
  CHECK(devices::xprinter_s_series().completion_caps.try_process_id_gs_h);
  CHECK_EQ(xp_s260m().completion, CompletionMechanism::GsParenH);
  CHECK(xp_s260m().completion_caps.process_id_gs_h);
  CHECK_EQ(devices::xprinter_pos58().media.printable_width_dots, escpos::kWidth58mm);
  CHECK(devices::xprinter_pos80().completion_caps.try_process_id_gs_h);

  // Partner RP-110 is the Sewoo platform, with GS ( H still a probe question.
  CHECK_EQ(devices::partner_rp110().media.printable_width_dots, escpos::kWidth80mm);
  CHECK(devices::partner_rp110().completion_caps.try_process_id_gs_h);
  CHECK(!devices::partner_rp110().completion_caps.process_id_gs_h);
  CHECK_EQ(devices::sewoo_slk_ts().identity.vendor, std::string("Sewoo"));

  // Star desktops (M13b): the documented session-scoped raw fence, a Star parser, and no
  // ESC/POS status assumptions anywhere. ETB is recorded as a capability and deliberately
  // not selected — its counter is broadcast to every host on TCP 9100.
  for (const CapabilityProfile& star :
       {devices::star_tsp100(), devices::star_tsp650(), devices::star_mcprint()}) {
    CHECK_EQ(star.completion, CompletionMechanism::StarEscGsEtx);
    CHECK_EQ(star.quirks.response_parser, ResponseParserVariant::StarPrnt);
    CHECK(!star.status.dle_eot);
    CHECK(star.completion_caps.prefer_vendor_sdk);
    CHECK(star.star.esc_gs_etx);
    CHECK(star.star.etb_counter);
    CHECK(!star.star.exclusive_single_session);
    CHECK_EQ(star.transport.bluetooth.mfi_protocol, std::string("jp.star-m.starpro"));
    CHECK(star.transport.bluetooth.ble_profile_unknown);
  }
  // The portables keep the SDK path, which this core still does not speak.
  for (const CapabilityProfile& star :
       {devices::star_sm_s230(), devices::star_sm_l200(), devices::star_sm_t400()}) {
    CHECK_EQ(star.completion, CompletionMechanism::StarCheckedBlock);
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

// --- Capability provenance (docs/compatibility-brief.md §28) ------------------------

PD_TEST(provenance_defaults_are_documented_only_where_the_manufacturer_documents_them) {
  // Epson is the reference implementation and the only family whose shipped defaults
  // claim Documented: Epson publishes a model-by-model ESC/POS applicability database
  // in which `GS ( H` fn 48 appears by name, alongside GS r, DLE EOT and ASB (§2).
  for (const CapabilityProfile& epson :
       {devices::epson_tm_t20iii(), devices::epson_tm_t88vi(), devices::epson_tm_t88vii(),
        devices::epson_tm_m30iii(), devices::epson_tm_p20ii(), devices::epson_tm_p80ii()}) {
    CHECK_EQ(epson.completion_caps.process_id_gs_h_provenance, Provenance::Documented);
    CHECK_EQ(epson.completion_caps.queued_gs_r_provenance, Provenance::Documented);
    CHECK_EQ(epson.status.dle_eot_provenance, Provenance::Documented);
    CHECK_EQ(epson.status.asb_provenance, Provenance::Documented);
    CHECK(epson.completion_caps.process_id_gs_h);
  }

  // Everyone else is Unverified until something establishes otherwise. These three are
  // named in the brief for three different reasons, and all three end in the same
  // place: an advertised standard is not a documented command table.
  //
  //   Xprinter (§14)  — the XP-S260M answers GS ( H on our bench, but Xprinter's public
  //                     documentation does not contain it, so the family cannot claim it.
  //   Rongta (§13)    — THE CORRECTION. An earlier revision of this database assumed
  //                     GS ( H from an RP80 manual; no manufacturer-hosted Rongta
  //                     reference proving it was found, so the claim is withdrawn.
  //   Partner (§15)   — not enough public programmer documentation for any of the Epson
  //                     completion extensions: DLE EOT, GS r, GS ( H and ASB are all
  //                     probe questions.
  for (const CapabilityProfile& unverified :
       {devices::xprinter_s_series(), devices::xprinter_pos80(), devices::rongta_rp80(),
        devices::rongta_rp58(), devices::rongta_rp3xx(), devices::rongta_rp8xx(),
        devices::partner_rp110(), devices::partner_rp710(), devices::sewoo_slk_ts()}) {
    CHECK_EQ(unverified.completion_caps.process_id_gs_h_provenance,
             Provenance::Unverified);
    CHECK_EQ(unverified.completion_caps.queued_gs_r_provenance, Provenance::Unverified);
    CHECK_EQ(unverified.status.dle_eot_provenance, Provenance::Unverified);
    CHECK_EQ(unverified.status.asb_provenance, Provenance::Unverified);
    // Unverified means "nobody checked", never "absent": the probe still asks.
    CHECK(!unverified.completion_caps.process_id_gs_h);
    CHECK(unverified.completion_caps.try_process_id_gs_h);
  }

  // Bixolon and Citizen publish real per-model command manuals, but those manuals have
  // not been loaded and validated here, so their entries do not borrow credibility from
  // the manuals' existence (§9-§12 policy: GS ( H = DO NOT ASSUME).
  for (const CapabilityProfile& per_model :
       {devices::bixolon_srp_q300(), devices::bixolon_spp_r310(),
        devices::citizen_cmp_20ii(), devices::citizen_cmp_30ii()}) {
    CHECK_EQ(per_model.completion_caps.process_id_gs_h_provenance,
             Provenance::Unverified);
    CHECK(!per_model.completion_caps.process_id_gs_h);
  }

  // Nothing in the shipped database claims Probed: a default has by definition not been
  // measured. Probed appears only on the one hand-written profile for an interrogated
  // unit, and on whatever the probe promotes at runtime.
  for (const std::string& name : devices::names()) {
    const CapabilityProfile profile = devices::byName(name);
    CHECK(profile.completion_caps.process_id_gs_h_provenance != Provenance::Probed);
    CHECK(profile.status.dle_eot_provenance != Provenance::Probed);
  }
  CHECK_EQ(xp_s260m().completion_caps.process_id_gs_h_provenance, Provenance::Probed);
  CHECK_EQ(xp_s260m().status.dle_eot_provenance, Provenance::Probed);
  // generic-escpos is UNKNOWN DEVICE: it knows nothing about anything.
  CHECK_EQ(generic_escpos().completion_caps.process_id_gs_h_provenance,
           Provenance::Unverified);
  CHECK_EQ(generic_escpos().status.asb_provenance, Provenance::Unverified);
}

PD_TEST(provenance_promotes_to_probed_when_the_device_answers) {
  // The Xprinter story end to end. Start from the family default, which is honest about
  // knowing nothing; interrogate a device that does answer; end with Probed — a
  // stronger claim than Xprinter's own marketing, and visibly a different one from
  // Epson's Documented.
  pdfake::Script script;
  script.answer_process_id = true;  // this unit really does answer GS ( H
  pdfake::FakePrinter device(script);
  const CapabilityFindings findings = probeDevice(device);
  CHECK_EQ(findings.gs_h_process_id.value_or(false), true);

  const CapabilityProfile defaults = devices::xprinter_s_series();
  CHECK_EQ(defaults.completion_caps.process_id_gs_h_provenance, Provenance::Unverified);
  CHECK(!defaults.probed);

  const CapabilityProfile promoted = promote(defaults, findings);
  CHECK(promoted.probed);
  CHECK(promoted.completion_caps.process_id_gs_h);
  CHECK_EQ(promoted.completion, CompletionMechanism::GsParenH);
  CHECK_EQ(promoted.completion_caps.process_id_gs_h_provenance, Provenance::Probed);
  CHECK_EQ(promoted.status.dle_eot_provenance, Provenance::Probed);
  CHECK_EQ(promoted.completion_caps.queued_gs_r_provenance, Provenance::Probed);
  // The grade the promoted profile can now claim is A, from the physical printer.
  CHECK_EQ(promoted.evidence().grade, ConfidenceGrade::A_JobLevelConfirmation);

  // An Epson that the probe could not reach keeps its documentation. Silence demotes
  // nothing — that is the difference between "no answer" and "answered no".
  CapabilityFindings silent;
  silent.key = "unreachable";
  silent.gs_i = true;  // non-empty, but says nothing about the completion extensions
  const CapabilityProfile epson = promote(devices::epson_tm_t88vi(), silent);
  CHECK_EQ(epson.completion_caps.process_id_gs_h_provenance, Provenance::Documented);
  CHECK_EQ(epson.status.dle_eot_provenance, Provenance::Documented);

  // A device that answers "no" is also a probe result: an established absence is
  // evidence and demotes a documented default to Probed-false.
  CapabilityFindings denied;
  denied.key = "answered-no";
  denied.gs_h_process_id = false;
  denied.gs_r1 = true;
  const CapabilityProfile demoted = promote(devices::epson_tm_t88vi(), denied);
  CHECK(!demoted.completion_caps.process_id_gs_h);
  CHECK_EQ(demoted.completion_caps.process_id_gs_h_provenance, Provenance::Probed);
  CHECK_EQ(demoted.completion, CompletionMechanism::GsR1);
}

PD_TEST(provenance_survives_a_persisted_findings_round_trip) {
  // Probe results are persisted so a fleet does not re-interrogate every printer on
  // every boot. What comes back out has to promote to the same provenance it did the
  // first time, or the second boot would quietly report a weaker profile than the
  // first (docs/capability-profiles.md, "persisted keyed by model + serial + firmware").
  pdfake::TempDir dir("provenance-round-trip");
  CapabilityFindings findings;
  findings.key = "xp-s260m-probed";
  findings.endpoint = "tcp://192.0.2.44:9100";
  findings.gs_h_process_id = true;
  findings.gs_r1 = true;
  findings.dle_eot = true;
  findings.asb = true;
  findings.cutter_error_status = true;

  {
    FindingsStore store(dir.path());
    store.save(findings);
  }
  FindingsStore reopened(dir.path());
  const std::optional<CapabilityFindings> loaded = reopened.find(findings.key);
  CHECK(loaded.has_value());

  const CapabilityProfile promoted = promote(devices::xprinter_s_series(), *loaded);
  CHECK_EQ(promoted.completion_caps.process_id_gs_h_provenance, Provenance::Probed);
  CHECK_EQ(promoted.completion_caps.queued_gs_r_provenance, Provenance::Probed);
  CHECK_EQ(promoted.status.dle_eot_provenance, Provenance::Probed);
  CHECK_EQ(promoted.status.asb_provenance, Provenance::Probed);
  CHECK_EQ(promoted.status.cutter_error_provenance, Provenance::Probed);
  CHECK_EQ(promoted.completion, CompletionMechanism::GsParenH);
}

// --- Catalogue expansion (docs/compatibility-brief.md §26) --------------------------

PD_TEST(catalogue_covers_every_family_the_brief_lists) {
  // §26's initial catalogue, spelled out. A missing id is a printer somebody cannot
  // select, and the failure mode of that is a fleet silently driven as generic_80.
  const char* const expected[] = {
      // Epson: T20/II/III/IV, T70/II, T82, T88IV/V/VI/VII, m10, m30/II/III, m50/II,
      // P20/P20II, P60/II, P80/P80II (plus the Auto Cutter variant), U220/II.
      "epson_tm_t20", "epson_tm_t20ii", "epson_tm_t20iii", "epson_tm_t20iv",
      "epson_tm_t70", "epson_tm_t70ii", "epson_tm_t82",
      "epson_tm_t88iv", "epson_tm_t88v", "epson_tm_t88vi", "epson_tm_t88vii",
      "epson_tm_m10", "epson_tm_m30", "epson_tm_m30ii", "epson_tm_m30iii",
      "epson_tm_m50", "epson_tm_m50ii",
      "epson_tm_p20", "epson_tm_p20ii", "epson_tm_p60", "epson_tm_p60ii",
      "epson_tm_p80", "epson_tm_p80ii", "epson_tm_p80ii_autocutter",
      "epson_tm_u220", "epson_tm_u220ii",
      // Star: TSP100III/IV, TSP650II, mC-Print2/3, SM-S210/220/230, SM-L200/300,
      // SM-T300/400.
      "star_tsp100iii", "star_tsp100iv", "star_tsp650", "star_mcprint2", "star_mcprint3",
      "star_sm_s210", "star_sm_s220", "star_sm_s230", "star_sm_l200", "star_sm_l300",
      "star_sm_t300", "star_sm_t400",
      // Bixolon: SRP-330/350/350plus/380, Q200/Q300, B300, F310, 275, SPP-R200/210/310/410.
      "bixolon_srp330", "bixolon_srp350", "bixolon_srp380", "bixolon_srp_q200",
      "bixolon_srp_q300", "bixolon_srp_b300", "bixolon_srp_f310", "bixolon_srp_275",
      "bixolon_spp_r200", "bixolon_spp_r210", "bixolon_spp_r310", "bixolon_spp_r410",
      // Citizen: the CT-S families plus the CMP portables.
      "citizen_cts_58_80", "citizen_cts_fast", "citizen_cts_wide", "citizen_cmp_20ii",
      "citizen_cmp_30ii",
      // Rongta, Xprinter, Partner.
      "rongta_rp58", "rongta_rp80", "rongta_rp3xx", "rongta_rp8xx",
      "xprinter_pos58", "xprinter_pos80", "xprinter_s_series", "xprinter_v_series",
      "xprinter_portable", "partner_rp110", "partner_rp710",
      // Zebra and Brother: present precisely so they can be refused rather than guessed.
      "zebra_zq300_plus", "zebra_zq500", "zebra_zq600_plus",
      "brother_rj2000", "brother_rj3000", "brother_rj4000",
      // The escape hatch.
      "generic_unknown",
  };
  for (const char* name : expected) {
    CHECK(devices::exists(name));
    CHECK_EQ(devices::byName(name).name, std::string(name));
  }
  CHECK(devices::names().size() >= sizeof(expected) / sizeof(expected[0]));
}

PD_TEST(catalogue_media_facts_come_from_the_brief_not_from_arithmetic) {
  // TM-T20III (§2): 576 dots on 80 mm, 420 in the 58 mm configuration. 420 is neither
  // 384 (the usual 58 mm figure) nor 576 x 58/80 = 417, which is exactly why it is
  // stored rather than computed.
  const CapabilityProfile t20iii = devices::epson_tm_t20iii();
  CHECK_EQ(t20iii.media.printable_width_dots, escpos::kWidth80mm);
  CHECK_EQ(t20iii.media.printable_width_dots_58mm, 420u);
  CHECK(t20iii.media.paper_guide_58mm);
  CHECK_EQ(static_cast<int>(t20iii.media.printable_width_mm), 72);

  // TM-P20II (§6): 58 mm paper, 48 mm print width, manual tear, and two documented
  // receive buffers — 4 KB normally, 64 KB over Bluetooth. Same printer, two numbers.
  const CapabilityProfile p20ii = devices::epson_tm_p20ii();
  CHECK_EQ(static_cast<int>(p20ii.media.nominal_roll_width_mm), 58);
  CHECK_EQ(static_cast<int>(p20ii.media.printable_width_mm), 48);
  CHECK_EQ(p20ii.media.printable_width_dots, escpos::kWidth58mm);
  CHECK(!p20ii.media.cutter);
  CHECK_EQ(p20ii.cut, CutVariant::None);
  CHECK_EQ(p20ii.transport.bluetooth.receive_buffer_bytes, 4096u);
  CHECK_EQ(p20ii.transport.bluetooth.bluetooth_receive_buffer_bytes, 65536u);

  // TM-P80II: the standard unit tears, the separately-sold Auto Cutter Model cuts.
  // Two hardware profiles, not one profile with a flag (§6).
  CHECK(!devices::epson_tm_p80ii().media.cutter);
  CHECK_EQ(devices::epson_tm_p80ii().cut, CutVariant::None);
  const CapabilityProfile autocut = devices::epson_tm_p80ii_autocutter();
  CHECK(autocut.media.cutter);
  CHECK_EQ(autocut.cut, CutVariant::Partial);
  // A cutter that never clears the head-to-blade gap slices the trailing QR.
  CHECK(autocut.media.head_to_cutter_feed_dots > 0);
  CHECK_EQ(devices::epson_tm_p80ii().media.head_to_cutter_feed_dots, 0);

  // CT-S4500 (§11-§12): 112 mm media, 104 mm image. CMP-30II: 25-80 mm media, 72 mm
  // maximum image. In both cases the roll and the raster are independent facts.
  CHECK_EQ(static_cast<int>(devices::citizen_cts_wide().media.printable_width_mm), 104);
  CHECK_EQ(devices::citizen_cts_wide().media.printable_width_dots, escpos::kWidth104mm);
  CHECK_EQ(static_cast<int>(devices::citizen_cmp_30ii().media.printable_width_mm), 72);
  CHECK_EQ(devices::citizen_cmp_30ii().media.printable_width_dots, escpos::kWidth80mm);
  CHECK_EQ(static_cast<int>(devices::citizen_cmp_20ii().media.printable_width_mm), 48);

  // TM-m30III (§4): 80/58 mm rolls and Bluetooth 5.0 Dual Mode on the Wi-Fi/BT config —
  // Classic and LE, never a single `bluetooth` flag.
  const CapabilityProfile m30iii = devices::epson_tm_m30iii();
  CHECK(m30iii.media.paper_guide_58mm);
  CHECK(m30iii.transport.bluetooth.classic_spp);
  CHECK(m30iii.transport.bluetooth.ble);
  CHECK(m30iii.transport.wifi);

  // Impact mechanisms keep their own geometry and their own patience.
  const CapabilityProfile srp275 = devices::bixolon_srp_275();
  CHECK_EQ(static_cast<int>(srp275.media.dpi), 160);
  CHECK_EQ(srp275.media.printable_width_dots, 400u);
  CHECK(srp275.quirks.delayed_status);
  CHECK_EQ(devices::epson_tm_u220ii().media.printable_width_dots, 400u);
}

PD_TEST(bluetooth_is_faceted_never_a_single_boolean) {
  // docs/compatibility-brief.md §25. Five different stacks hide behind "bluetooth:
  // true", and picking the wrong one is a printer that never connects.
  const CapabilityProfile p20ii = devices::epson_tm_p20ii();
  CHECK(p20ii.transport.bluetooth.classic_spp);  // TM-P20II-xxxxxx
  CHECK(p20ii.transport.bluetooth.ble);          // TM-P20II-xxxxxx-L
  CHECK(!p20ii.transport.bluetooth.vendor_sdk);
  CHECK(p20ii.transport.bluetooth.any());

  // Star drives its portables through the SDK, not a raw socket.
  const CapabilityProfile s230 = devices::star_sm_s230();
  CHECK(s230.transport.bluetooth.vendor_sdk);
  CHECK(s230.transport.bluetooth.classic_vendor);
  CHECK(s230.transport.bluetooth.ble);  // Bluetooth 5.2: both radios
  CHECK(!s230.transport.bluetooth.classic_spp);

  // Citizen's wide unit sells an Apple MFi option, which is a different iOS code path
  // from BLE and cannot be inferred from either.
  CHECK(devices::citizen_cts_wide().transport.bluetooth.mfi);
  CHECK(devices::citizen_cmp_20ii().transport.bluetooth.mfi);

  // Zebra exposes separate Classic and BLE status connections through Link-OS.
  const CapabilityProfile zq600 = devices::zebra_zq600_plus();
  CHECK(zq600.transport.bluetooth.classic_vendor);
  CHECK(zq600.transport.bluetooth.ble);
  CHECK(zq600.transport.bluetooth.vendor_sdk);

  // A desktop Ethernet printer claims none of it, and `any()` says so.
  CHECK(!devices::epson_tm_t88vi().transport.bluetooth.any());
  CHECK(!devices::generic_80().transport.bluetooth.any());

  // The Xprinter handheld documents Classic SPP only. Claiming BLE here would send an
  // iOS app down a path that cannot work.
  CHECK(devices::xprinter_portable().transport.bluetooth.classic_spp);
  CHECK(!devices::xprinter_portable().transport.bluetooth.ble);
}

PD_TEST(zebra_and_brother_are_not_escpos_and_are_never_driven_as_such) {
  // §16, §17. These are ZPL/CPCL and Brother Raster/ESC-P devices. The engine must
  // refuse them on the language alone, before the completion mechanism is even
  // considered — an ESC/POS engine pointed at a ZPL printer emits a metre of text.
  for (const CapabilityProfile& refused :
       {devices::zebra_zq300_plus(), devices::zebra_zq500(), devices::zebra_zq600_plus()}) {
    CHECK_EQ(refused.language, CommandLanguage::Zpl);
    CHECK(refused.languages.has(CommandLanguage::Zpl));
    CHECK(refused.languages.has(CommandLanguage::Cpcl));
    CHECK(!refused.languages.has(CommandLanguage::EscPos));
    CHECK(!refused.drivableByEscposEngine());
    CHECK_EQ(refused.completion, CompletionMechanism::None);
    CHECK_EQ(refused.evidence().grade, ConfidenceGrade::E_TransportOnly);
    CHECK_EQ(refused.identity.vendor, std::string("Zebra"));
  }
  for (const CapabilityProfile& refused :
       {devices::brother_rj2000(), devices::brother_rj3000(), devices::brother_rj4000()}) {
    CHECK_EQ(refused.language, CommandLanguage::BrotherRaster);
    CHECK(refused.languages.has(CommandLanguage::EscP));
    CHECK(!refused.languages.has(CommandLanguage::EscPos));
    CHECK(!refused.drivableByEscposEngine());
    CHECK_EQ(refused.identity.vendor, std::string("Brother"));
  }
  // The RJ-4000 adds ZPL II and CPCL on some configurations — still not ESC/POS.
  CHECK(devices::brother_rj4000().languages.has(CommandLanguage::Zpl));
  CHECK(devices::brother_rj4000().languages.has(CommandLanguage::Cpcl));
  CHECK_EQ(devices::brother_rj4000().languages.count(), static_cast<size_t>(4));

  // Citizen's CMP portables document three languages on one printer and ARE drivable,
  // because ESC/POS is among them and is the one this core picks.
  const CapabilityProfile cmp = devices::citizen_cmp_30ii();
  CHECK_EQ(cmp.language, CommandLanguage::EscPos);
  CHECK(cmp.languages.has(CommandLanguage::EscPos));
  CHECK(cmp.languages.has(CommandLanguage::Cpcl));
  CHECK(cmp.languages.has(CommandLanguage::Zpl));
  CHECK_EQ(cmp.languages.count(), static_cast<size_t>(3));
  CHECK(cmp.drivableByEscposEngine());

  // Every language has a distinct spelling, so a report cannot conflate two of them.
  std::set<std::string> spellings;
  for (const CommandLanguage language : kAllCommandLanguages) {
    CHECK(spellings.insert(to_string(language)).second);
  }
  CHECK_EQ(spellings.size(), kAllCommandLanguages.size());
  CHECK_EQ(std::string(to_string(CommandLanguage::EscP)), std::string("EscP"));
  CHECK_EQ(std::string(to_string(CommandLanguage::BrotherRaster)),
           std::string("BrotherRaster"));
}

PD_TEST(generic_unknown_prints_and_claims_nothing) {
  // §26's escape hatch. generic_80 says "an ordinary 80 mm ESC/POS printer";
  // generic_unknown says "we do not know what this is", and those were previously the
  // same entry. It still prints — refusing to print unidentified hardware would be
  // worse — but every job on it terminates at grade E.
  const CapabilityProfile unknown = devices::generic_unknown();
  CHECK(unknown.drivableByEscposEngine());
  CHECK_EQ(unknown.completion, CompletionMechanism::None);
  CHECK_EQ(unknown.maxConfidence(), ConfidenceLevel::TransportAccepted);
  CHECK_EQ(unknown.evidence().grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(unknown.evidence().authority, CompletionAuthority::TransportOnly);
  CHECK(!unknown.status.dle_eot);
  CHECK(!unknown.status.asb);
  CHECK_EQ(unknown.completion_caps.process_id_gs_h_provenance, Provenance::Unverified);
  // generic_80 still claims the queued fence, which is the difference.
  CHECK_EQ(devices::generic_80().completion, CompletionMechanism::GsR1);
}
