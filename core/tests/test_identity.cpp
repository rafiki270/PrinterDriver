#include "printerdriver/identity.hpp"

#include <string>
#include <vector>

#include "test_harness.hpp"

using namespace pd;

namespace {

std::vector<uint8_t> bytesOf(std::initializer_list<int> values) {
  std::vector<uint8_t> out;
  for (const int value : values) {
    out.push_back(static_cast<uint8_t>(value));
  }
  return out;
}

std::vector<uint8_t> framed(const std::string& text) {
  std::vector<uint8_t> out{0x5F};
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0x00);
  return out;
}

// The pair Rongta's own command manual prints for GS I.
GsIStrings impersonatingRongta() {
  GsIStrings reported;
  reported.manufacturer = "EPOSN";
  reported.model = "TM-T88V";
  reported.firmware = "1.02";
  return reported;
}

}  // namespace

// --- GS I response parsing ----------------------------------------------------------

PD_TEST(gs_i_parses_the_epson_header_data_nul_frame) {
  const auto answer = parseGsIString(framed("EPSON"));
  CHECK(answer.has_value());
  CHECK_EQ(*answer, std::string("EPSON"));
}

PD_TEST(gs_i_parses_clone_variants_without_header_or_terminator) {
  // No 0x5F header, terminator present.
  std::vector<uint8_t> no_header;
  const std::string text = "RP80";
  no_header.insert(no_header.end(), text.begin(), text.end());
  no_header.push_back(0x00);
  const auto first = parseGsIString(no_header);
  CHECK(first.has_value());
  CHECK_EQ(*first, std::string("RP80"));

  // Neither header nor terminator: whatever printable bytes arrived before the
  // per-field timeout expired.
  const std::vector<uint8_t> bare(text.begin(), text.end());
  const auto second = parseGsIString(bare);
  CHECK(second.has_value());
  CHECK_EQ(*second, std::string("RP80"));

  // Padded with spaces, which several clones do to a fixed field width.
  const auto third = parseGsIString(framed("TM-T88V   "));
  CHECK(third.has_value());
  CHECK_EQ(*third, std::string("TM-T88V"));
}

PD_TEST(gs_i_rejects_empty_and_unprintable_answers) {
  CHECK(!parseGsIString(std::vector<uint8_t>{}).has_value());
  CHECK(!parseGsIString(bytesOf({0x5F, 0x00})).has_value());
  CHECK(!parseGsIString(bytesOf({0x01, 0x02, 0x03})).has_value());
  CHECK(!parseGsIByte(std::vector<uint8_t>{}).has_value());
  const auto id = parseGsIByte(bytesOf({0x20}));
  CHECK(id.has_value());
  CHECK_EQ(static_cast<int>(*id), 0x20);
}

// --- OUI table ------------------------------------------------------------------------

PD_TEST(oui_normalises_every_common_mac_spelling) {
  CHECK_EQ(normalizeOui("00:00:48:11:22:33"), std::string("00:00:48"));
  CHECK_EQ(normalizeOui("00-00-48-11-22-33"), std::string("00:00:48"));
  CHECK_EQ(normalizeOui("000048112233"), std::string("00:00:48"));
  CHECK_EQ(normalizeOui("00:00:48"), std::string("00:00:48"));
  CHECK_EQ(normalizeOui("0:0:4"), std::string(""));
  CHECK_EQ(normalizeOui(""), std::string(""));
}

PD_TEST(oui_lookup_answers_only_for_entries_in_the_table) {
  CHECK_EQ(ouiVendor("00:00:48:aa:bb:cc"), std::string("Epson"));
  CHECK_EQ(ouiVendor("00:26:AB:00:00:01"), std::string("Epson"));
  // An unknown prefix answers "unknown", never a guess.
  CHECK_EQ(ouiVendor("DE:AD:BE:EF:00:01"), std::string(""));
  CHECK(!builtinOuiTable().empty());

  // The table is data: a caller with its own registry extract passes one in.
  const std::vector<OuiEntry> extended{{"DE:AD:BE", "Bench Rig"}};
  CHECK_EQ(ouiVendor("de:ad:be:00:00:01", extended), std::string("Bench Rig"));
}

// --- Multi-signal identification -------------------------------------------------------

PD_TEST(identify_trusts_epson_only_when_the_oui_agrees) {
  GsIStrings reported;
  reported.manufacturer = "EPSON";
  reported.model = "TM-T20III";
  reported.firmware = "4.00";

  IdentityHints hints;
  hints.mac = "00:00:48:12:34:56";
  BehaviourSignals behaviour;
  behaviour.dle_eot = true;
  behaviour.asb = true;
  behaviour.gs_h_process_id = true;

  const IdentityAssessment assessment = identify(reported, hints, behaviour);
  CHECK(assessment.identity_trusted);
  CHECK(!assessment.impersonation_suspected);
  CHECK_EQ(assessment.vendor_guess, std::string("Epson"));
  CHECK_EQ(assessment.profile_guess, std::string("epson_tm_t20"));
  CHECK(assessment.confidence_percent > 80);
  CHECK(assessment.confidence_percent < 100);
}

PD_TEST(identify_never_trusts_gs_i_on_its_own) {
  GsIStrings reported;
  reported.manufacturer = "EPSON";
  reported.model = "TM-T20III";

  BehaviourSignals behaviour;
  behaviour.dle_eot = true;
  behaviour.asb = true;

  // Same strings, no MAC: nothing corroborates them, so the identity stays untrusted
  // however plausible it reads.
  const IdentityAssessment assessment = identify(reported, IdentityHints{}, behaviour);
  CHECK(!assessment.identity_trusted);
  CHECK_EQ(assessment.vendor_guess, std::string("Epson"));
  CHECK(assessment.confidence_percent < 80);
}

PD_TEST(identify_catches_the_rongta_eposn_impersonation) {
  BehaviourSignals behaviour;
  behaviour.dle_eot = true;
  behaviour.asb = true;
  behaviour.gs_h_process_id = true;

  const IdentityAssessment assessment =
      identify(impersonatingRongta(), IdentityHints{}, behaviour);
  CHECK(!assessment.identity_trusted);
  CHECK(assessment.impersonation_suspected);
  CHECK_EQ(assessment.vendor_guess, std::string("Rongta"));
  CHECK_EQ(assessment.profile_guess, std::string("rongta_rp80"));
  // The reported strings are still carried: they are evidence, not truth.
  CHECK_EQ(assessment.reported_manufacturer, std::string("EPOSN"));
  CHECK_EQ(assessment.reported_model, std::string("TM-T88V"));
  CHECK(assessment.confidence_percent > 60);
}

PD_TEST(identify_flags_a_correctly_spelled_epson_claim_contradicted_by_the_oui) {
  GsIStrings reported;
  reported.manufacturer = "EPSON";
  reported.model = "TM-T88V";

  IdentityHints hints;
  hints.mac = "DE:AD:BE:00:00:01";  // not in the table, and not Epson's
  BehaviourSignals behaviour;
  behaviour.dle_eot = true;
  behaviour.asb = true;
  behaviour.gs_h_process_id = true;

  // The strings alone would hand this device the Epson profile, including a
  // completion mechanism it may not implement. It must not be trusted.
  const IdentityAssessment assessment = identify(reported, hints, behaviour);
  CHECK(!assessment.identity_trusted);
  CHECK(assessment.impersonation_suspected);
  CHECK(assessment.profile_guess != std::string("epson_tm_t88"));
}

PD_TEST(identify_flags_an_epson_claim_contradicted_by_behaviour) {
  GsIStrings reported;
  reported.manufacturer = "EPSON";
  reported.model = "TM-T88V";

  // No MAC at all, but a device claiming to be an Epson TM that answers neither
  // DLE EOT nor ASB is contradicting its own identity.
  BehaviourSignals behaviour;
  behaviour.dle_eot = false;
  behaviour.asb = false;

  const IdentityAssessment assessment = identify(reported, IdentityHints{}, behaviour);
  CHECK(!assessment.identity_trusted);
  CHECK(assessment.impersonation_suspected);
  CHECK_EQ(assessment.vendor_guess, std::string("Rongta"));
  CHECK_EQ(assessment.profile_guess, std::string("rongta_rp80"));
}

PD_TEST(identify_falls_back_through_oui_then_hint_then_nothing) {
  BehaviourSignals behaviour;

  IdentityHints oui_only;
  oui_only.mac = "00:00:48:aa:bb:cc";
  const IdentityAssessment from_oui = identify(GsIStrings{}, oui_only, behaviour);
  CHECK_EQ(from_oui.vendor_guess, std::string("Epson"));
  CHECK(!from_oui.identity_trusted);  // the vendor is known, the model is not

  IdentityHints caller;
  caller.vendor_hint = "Xprinter";
  const IdentityAssessment from_hint = identify(GsIStrings{}, caller, behaviour);
  CHECK_EQ(from_hint.vendor_guess, std::string("Xprinter"));
  CHECK(from_hint.confidence_percent > 0);

  const IdentityAssessment blind = identify(GsIStrings{}, IdentityHints{}, behaviour);
  CHECK_EQ(blind.vendor_guess, std::string("Unknown"));
  CHECK_EQ(blind.profile_guess, std::string("generic_80"));
  CHECK_EQ(static_cast<int>(blind.confidence_percent), 0);
  CHECK(!blind.identity_trusted);
}

PD_TEST(profile_lookup_maps_the_families_in_the_device_database) {
  CHECK_EQ(profileForModel("Epson", "TM-T20III"), std::string("epson_tm_t20"));
  CHECK_EQ(profileForModel("Epson", "TM-T88VI"), std::string("epson_tm_t88"));
  CHECK_EQ(profileForModel("Epson", "TM-m30II"), std::string("epson_tm_m"));
  CHECK_EQ(profileForModel("Epson", "TM-U220B"), std::string("epson_tm_u220"));
  CHECK_EQ(profileForModel("Rongta", "RP80USE"), std::string("rongta_rp80"));
  CHECK_EQ(profileForModel("Rongta", "RP326"), std::string("rongta_rp3xx"));
  CHECK_EQ(profileForModel("Sewoo", "SLK-TS200"), std::string("sewoo_slk_ts"));
  CHECK_EQ(profileForModel("Partner Tech", "RP-110"), std::string("partner_rp110"));
  CHECK_EQ(profileForModel("Star", "TSP143IV"), std::string("star_tsp100"));
  CHECK_EQ(profileForModel("Bixolon", "SRP-330III"), std::string("bixolon_srp330"));
  CHECK_EQ(profileForModel("Citizen", "CT-S4500"), std::string("citizen_cts_wide"));
  CHECK_EQ(profileForModel("Xprinter", "XP-S260M"), std::string("xprinter_s_series"));
  CHECK_EQ(profileForModel("Unknown", "no such thing"), std::string(""));
}

PD_TEST(identity_key_prefers_the_device_over_its_address) {
  GsIStrings reported;
  reported.model = "TM-T88V";
  reported.firmware = "1.02";
  reported.serial = "X5K0032";
  const std::string key = identityKey(reported, "tcp://192.0.2.10:9100");
  CHECK_EQ(key, std::string("tm_t88v-1_02-x5k0032"));

  // A printer that says nothing about itself can only be keyed by where it was
  // found, and that key stops being valid the moment it changes address.
  const std::string fallback = identityKey(GsIStrings{}, "tcp://192.0.2.10:9100");
  CHECK(fallback.find("endpoint-") == 0);
  CHECK_EQ(identityKey(GsIStrings{}, ""), std::string("unidentified"));
}
