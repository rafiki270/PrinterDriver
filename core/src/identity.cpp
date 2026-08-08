#include "printerdriver/identity.hpp"

#include <algorithm>
#include <cctype>

namespace pd {
namespace {

// Epson's framing for GS I 65-68 answers.
constexpr uint8_t kInfoHeader = 0x5F;

std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool has(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// Comparison key for identity strings: case and punctuation differ between firmware
// revisions of the same model ("TM-T88V", "TM T88V", "tm_t88v").
std::string squash(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

uint8_t clampConfidence(int value) {
  if (value < 0) {
    return 0;
  }
  // Never 100: no fingerprint built from a lying command set is certain.
  return static_cast<uint8_t>(value > 99 ? 99 : value);
}

std::string vendorFromManufacturer(const std::string& manufacturer,
                                   const std::string& model) {
  const std::string key = squash(manufacturer);
  const std::string both = key + squash(model);
  if (key == "EPOSN") {
    return "EPOSN";  // handled as impersonation by the caller, never as a vendor
  }
  if (has(key, "EPSON") || has(key, "SEIKO")) {
    return "Epson";
  }
  if (has(key, "RONGTA")) {
    return "Rongta";
  }
  if (has(key, "XPRINTER") || has(key, "XPRT")) {
    return "Xprinter";
  }
  if (has(key, "BIXOLON")) {
    return "Bixolon";
  }
  if (has(key, "CITIZEN")) {
    return "Citizen";
  }
  if (has(key, "STAR")) {
    return "Star";
  }
  if (has(key, "SEWOO") || has(key, "AROOT")) {
    return "Sewoo";
  }
  if (has(key, "PARTNER")) {
    return "Partner Tech";
  }
  if (has(key, "SNBC") || has(key, "BEIYANG")) {
    return "SNBC";
  }
  // Nothing usable in the manufacturer field: some clones leave it blank and put
  // everything in the model name.
  if (has(both, "SLKTS")) {
    return "Sewoo";
  }
  if (has(both, "RP110")) {
    return "Partner Tech";
  }
  return "";
}

}  // namespace

std::optional<std::string> parseGsIString(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return std::nullopt;
  }
  size_t begin = 0;
  if (data[0] == kInfoHeader) {
    begin = 1;
  }
  std::string out;
  for (size_t i = begin; i < size; ++i) {
    const uint8_t value = data[i];
    if (value == 0x00) {
      break;
    }
    if (value >= 0x20 && value <= 0x7E) {
      out += static_cast<char>(value);
    }
  }
  out = trim(out);
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::string> parseGsIString(const std::vector<uint8_t>& bytes) {
  return parseGsIString(bytes.data(), bytes.size());
}

std::optional<uint8_t> parseGsIByte(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  return bytes.front();
}

bool GsIStrings::answered() const noexcept {
  return !manufacturer.empty() || !model.empty() || !firmware.empty() ||
         !serial.empty() || model_id.has_value() || type_id.has_value() ||
         rom_version.has_value();
}

const std::vector<OuiEntry>& builtinOuiTable() {
  static const std::vector<OuiEntry> table{
      // Seiko Epson Corporation. The only assignments in this file that could be
      // confirmed against the IEEE registry; every other vendor in the device
      // database is left to caller-supplied data rather than guessed at here.
      {"00:00:48", "Epson"},
      {"00:26:AB", "Epson"},
  };
  return table;
}

std::string normalizeOui(const std::string& mac) {
  std::string digits;
  for (const char c : mac) {
    if (std::isxdigit(static_cast<unsigned char>(c))) {
      digits += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (digits.size() == 6) {
      break;
    }
  }
  if (digits.size() < 6) {
    return "";
  }
  return digits.substr(0, 2) + ":" + digits.substr(2, 2) + ":" + digits.substr(4, 2);
}

std::string ouiVendor(const std::string& mac, const std::vector<OuiEntry>& table) {
  const std::string prefix = normalizeOui(mac);
  if (prefix.empty()) {
    return "";
  }
  for (const OuiEntry& entry : table) {
    if (prefix == normalizeOui(entry.prefix)) {
      return entry.vendor;
    }
  }
  return "";
}

std::string ouiVendor(const std::string& mac) {
  return ouiVendor(mac, builtinOuiTable());
}

std::string profileForModel(const std::string& vendor, const std::string& model) {
  const std::string key = squash(model);
  const std::string brand = squash(vendor);
  if (key.empty() && brand.empty()) {
    return "";
  }

  // Order matters: model strings overlap across vendors, and the more specific
  // families have to be tested before the two-letter ones. "SRP330III" contains
  // "RP3", which is a Rongta family, so Bixolon has to be matched first.
  if (has(key, "TMT20") || has(key, "TMT82")) return "epson_tm_t20";
  if (has(key, "TMT88")) return "epson_tm_t88";
  if (has(key, "TMT70")) return "epson_tm_t70";
  if (has(key, "TMU220")) return "epson_tm_u220";
  if (has(key, "TMM10") || has(key, "TMM30") || has(key, "TMM50")) return "epson_tm_m";
  if (has(key, "TMI") || has(key, "TMDT")) return "epson_tm_i";

  if (has(key, "SLKTS")) return "sewoo_slk_ts";
  if (has(key, "RP110")) return "partner_rp110";

  if (has(key, "TSP100") || has(key, "TSP143")) return "star_tsp100";
  if (has(key, "TSP65")) return "star_tsp650";
  if (has(key, "MCPRINT")) return "star_mcprint";

  if (has(key, "SRP330") || has(key, "SRP332")) return "bixolon_srp330";
  if (has(key, "SRP350") || has(key, "SRP352")) return "bixolon_srp350";
  if (has(key, "SRP380") || has(key, "SRP382")) return "bixolon_srp380";
  if (has(key, "SRPQ")) return "bixolon_q_series";

  if (has(key, "CTS4500") || has(key, "CTS4000")) return "citizen_cts_wide";
  if (has(key, "CTS801") || has(key, "CTS851")) return "citizen_cts_fast";
  if (has(key, "CTS") || has(key, "CTE")) return "citizen_cts_58_80";

  if (has(key, "S260") || has(key, "S200") || has(key, "S300")) return "xprinter_s_series";
  if (brand == "XPRINTER") {
    return has(key, "58") ? "xprinter_pos58" : "xprinter_pos80";
  }

  // Rongta last, and only for a model string that starts with RP: as a substring
  // "RP3" appears in half the Bixolon range.
  if (key.rfind("RP", 0) == 0 || brand == "RONGTA") {
    if (has(key, "RP58")) return "rongta_rp58";
    if (has(key, "RP80") || has(key, "RP8")) return "rongta_rp80";
    if (has(key, "RP3")) return "rongta_rp3xx";
  }
  return "";
}

IdentityAssessment identify(const GsIStrings& reported, const IdentityHints& hints,
                            const BehaviourSignals& behaviour,
                            const std::vector<OuiEntry>& oui_table) {
  IdentityAssessment out;
  out.reported_manufacturer = reported.manufacturer;
  out.reported_model = reported.model;
  out.firmware = reported.firmware;
  out.serial = reported.serial;

  const std::string oui_prefix = normalizeOui(hints.mac);
  out.oui_vendor = ouiVendor(hints.mac, oui_table);
  if (!oui_prefix.empty()) {
    out.signals.push_back("MAC OUI " + oui_prefix + " -> " +
                          (out.oui_vendor.empty() ? std::string("not in table")
                                                  : out.oui_vendor));
  }

  const std::string claimed = vendorFromManufacturer(reported.manufacturer, reported.model);
  const std::string model_key = squash(reported.model);
  // The exact pair Rongta's manual prints. Seeing it is not proof of a Rongta, but it
  // is proof that the identity is borrowed from somewhere.
  const bool impersonation_target = has(model_key, "TMT88V");
  // An Epson TM answers DLE EOT and ASB; a device that claims to be one and cannot is
  // contradicting its own identity, which outranks any string it returned.
  const bool behaviour_contradicts_epson = behaviour.dle_eot.value_or(true) == false ||
                                           behaviour.asb.value_or(true) == false;

  int score = 0;

  if (claimed == "EPOSN") {
    out.impersonation_suspected = true;
    out.identity_trusted = false;
    out.signals.push_back(
        "GS I manufacturer \"EPOSN\" is Rongta's documented Epson impersonation");
    out.vendor_guess = out.oui_vendor.empty() ? "Rongta" : out.oui_vendor;
    out.profile_guess = "rongta_rp80";
    score = 60;
    if (!out.oui_vendor.empty() && out.oui_vendor != "Rongta") {
      // Two strong signals disagreeing is not a reason to pick one of them: the
      // device is something this fingerprint cannot name, and the report says so
      // rather than shipping a vendor and a profile that contradict each other.
      out.signals.push_back("OUI names " + out.oui_vendor + ", not Rongta");
      out.profile_guess = "generic_80";
      score -= 20;
    }
    if (behaviour.gs_h_process_id.value_or(false)) {
      out.signals.push_back("GS ( H answered, matching the RP80 family command manual");
      score += 20;
    }
    if (behaviour.dle_eot.value_or(false)) {
      score += 10;
    }
  } else if (claimed == "Epson") {
    // An OUI that resolves to nobody is weaker evidence than one that resolves to
    // another vendor, but against the one manufacturer this table does cover it is
    // still a reason not to take an Epson claim at face value.
    const bool unattributable_oui = !oui_prefix.empty() && out.oui_vendor.empty();
    const bool conflicting_oui = !out.oui_vendor.empty();
    if (out.oui_vendor == "Epson") {
      out.identity_trusted = true;
      out.vendor_guess = "Epson";
      out.signals.push_back("GS I manufacturer agrees with the MAC OUI");
      score = 70;
    } else if (conflicting_oui || behaviour_contradicts_epson ||
               (unattributable_oui && impersonation_target)) {
      out.impersonation_suspected = true;
      out.signals.push_back(
          conflicting_oui
              ? "claims Epson but the MAC OUI belongs to " + out.oui_vendor
              : (unattributable_oui
                     ? std::string("claims Epson from an OUI Epson is not known to own")
                     : std::string(
                           "claims Epson but does not behave like one: no DLE EOT/ASB")));
      if (impersonation_target) {
        out.vendor_guess = out.oui_vendor.empty() ? "Rongta" : out.oui_vendor;
        out.profile_guess = "rongta_rp80";
        out.signals.push_back(
            "reported model TM-T88V is the model Rongta firmware impersonates");
        score = 55;
        if (behaviour.gs_h_process_id.value_or(false)) {
          score += 10;
        }
      } else {
        out.vendor_guess = out.oui_vendor.empty() ? "Unknown" : out.oui_vendor;
        out.profile_guess = "generic_80";
        score = 35;
      }
    } else {
      // Nothing contradicts it and nothing corroborates it either. GS I alone is
      // never enough to trust an identity (docs/capability-profiles.md).
      out.vendor_guess = "Epson";
      out.signals.push_back("identity rests on GS I alone: unverified");
      score = unattributable_oui ? 40 : 50;
    }
  } else if (!claimed.empty()) {
    out.vendor_guess = claimed;
    if (out.oui_vendor == claimed) {
      out.identity_trusted = true;
      out.signals.push_back("GS I manufacturer agrees with the MAC OUI");
      score = 70;
    } else if (!out.oui_vendor.empty()) {
      out.vendor_guess = out.oui_vendor;
      out.impersonation_suspected = true;
      out.signals.push_back("GS I says " + claimed + ", MAC OUI says " + out.oui_vendor);
      score = 35;
    } else {
      out.signals.push_back("identity rests on GS I alone: unverified");
      score = 50;
    }
  } else if (!out.oui_vendor.empty()) {
    out.vendor_guess = out.oui_vendor;
    out.signals.push_back("vendor from MAC OUI only: the model is still unknown");
    score = 40;
  } else if (!hints.vendor_hint.empty()) {
    out.vendor_guess = hints.vendor_hint;
    out.signals.push_back("vendor from caller hint only");
    score = 20;
  } else {
    out.signals.push_back("no usable identification signal: unknown ESC/POS device");
    score = 0;
  }

  if (out.profile_guess == "generic_80" || out.profile_guess.empty()) {
    const std::string matched = profileForModel(out.vendor_guess, reported.model);
    if (!matched.empty() && !out.impersonation_suspected) {
      out.profile_guess = matched;
      out.signals.push_back("reported model maps to profile " + matched);
      score += 15;
    } else if (out.profile_guess.empty()) {
      out.profile_guess = "generic_80";
    }
  }

  if (!hints.vendor_hint.empty() &&
      squash(hints.vendor_hint) == squash(out.vendor_guess)) {
    out.signals.push_back("caller hint agrees with the derived vendor");
    score += 10;
  }

  if (behaviour.gs_h_process_id.has_value()) {
    out.signals.push_back(*behaviour.gs_h_process_id ? "probe: GS ( H fn48 answered"
                                                     : "probe: GS ( H fn48 silent");
  }
  if (behaviour.gs_r1.has_value()) {
    out.signals.push_back(*behaviour.gs_r1 ? "probe: GS r 1 answered"
                                           : "probe: GS r 1 silent");
  }

  out.confidence_percent = clampConfidence(score);
  if (out.impersonation_suspected) {
    out.identity_trusted = false;
  }
  return out;
}

IdentityAssessment identify(const GsIStrings& reported, const IdentityHints& hints,
                            const BehaviourSignals& behaviour) {
  return identify(reported, hints, behaviour, builtinOuiTable());
}

std::string identityKey(const GsIStrings& reported, const std::string& endpoint) {
  std::string key;
  const auto append = [&key](const std::string& part) {
    if (part.empty()) {
      return;
    }
    if (!key.empty()) {
      key += '-';
    }
    for (const char c : part) {
      key += std::isalnum(static_cast<unsigned char>(c))
                 ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                 : '_';
    }
  };
  append(reported.model);
  append(reported.firmware);
  append(reported.serial);
  if (key.empty()) {
    // No usable identity: the endpoint is the only stable handle there is, and it
    // stops being stable the moment the printer changes address.
    append(endpoint);
    return key.empty() ? std::string("unidentified") : "endpoint-" + key;
  }
  return key;
}

}  // namespace pd
