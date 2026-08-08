#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/escpos_encoder.hpp"

// Printer identification (docs/capability-profiles.md "Identification: multi-signal
// fingerprinting").
//
// The governing fact: `GS I` can lie. Rongta's own command manual documents its
// printers answering Manufacturer "EPOSN", Printer name "TM-T88V" — identify a device
// by GS I alone and a Rongta silently receives the Epson profile, including a
// completion mechanism it may not implement. So identity here is UNTRUSTED by default
// and only ever promoted when an independent signal agrees: MAC OUI, a caller-supplied
// hint, or observed command behaviour.

namespace pd {

// One GS I text answer. Epson frames these Header(0x5F) + data + NUL; clone firmware
// variously drops the header, drops the terminator, or pads with spaces, so all three
// shapes are accepted and everything non-printable is discarded. Returns nullopt when
// no printable content could be recovered at all.
std::optional<std::string> parseGsIString(const uint8_t* data, size_t size);
std::optional<std::string> parseGsIString(const std::vector<uint8_t>& bytes);

// A single-byte GS I 1/2/3 answer, which carries no framing whatsoever.
std::optional<uint8_t> parseGsIByte(const std::vector<uint8_t>& bytes);

// What the device says about itself. Every field is what it *reported*, never what it
// is: the whole struct is evidence, not truth.
struct GsIStrings {
  std::string manufacturer;  // GS I 66
  std::string model;         // GS I 67
  std::string firmware;      // GS I 65
  std::string serial;        // GS I 68
  std::optional<uint8_t> model_id;     // GS I 1
  std::optional<uint8_t> type_id;      // GS I 2
  std::optional<uint8_t> rom_version;  // GS I 3

  bool answered() const noexcept;
};

// Behaviour the probe established first-hand. A device claiming to be an Epson TM that
// cannot answer DLE EOT is contradicting itself, and that contradiction is a stronger
// signal than any string it returned.
struct BehaviourSignals {
  std::optional<bool> gs_h_process_id;
  std::optional<bool> gs_r1;
  std::optional<bool> dle_eot;
  std::optional<bool> asb;
  std::optional<bool> cutter_error_status;
};

// Anything the caller already knows. The MAC accepts any separator or none.
struct IdentityHints {
  std::string mac;
  std::string vendor_hint;
};

struct OuiEntry {
  const char* prefix;  // "00:00:48", upper case, colon separated
  const char* vendor;
};

// Deliberately tiny. An OUI mapped to the wrong vendor is worse than no mapping at
// all, because it manufactures a confident wrong answer, so only assignments that
// could be confirmed are compiled in and the rest is expected to arrive as data from
// the caller (see the table-taking overloads below).
const std::vector<OuiEntry>& builtinOuiTable();

// "00-00-48-11-22-33", "000048112233" and "00:00:48" all normalise to "00:00:48".
// Returns an empty string when there are not six usable hex digits.
std::string normalizeOui(const std::string& mac);

std::string ouiVendor(const std::string& mac);
std::string ouiVendor(const std::string& mac, const std::vector<OuiEntry>& table);

struct IdentityAssessment {
  std::string vendor_guess = "Unknown";
  std::string profile_guess = "generic_80";
  uint8_t confidence_percent = 0;
  // Only ever true when a signal independent of GS I agrees with GS I.
  bool identity_trusted = false;

  std::string reported_manufacturer;
  std::string reported_model;
  std::string firmware;
  std::string serial;
  std::string oui_vendor;
  // Whether the reported identity is believed to be borrowed from another vendor.
  bool impersonation_suspected = false;

  // Human-readable reasons, in report order. This is what `pdctl probe` prints and
  // what an operator needs when the guess is wrong.
  std::vector<std::string> signals;
};

IdentityAssessment identify(const GsIStrings& reported, const IdentityHints& hints,
                            const BehaviourSignals& behaviour);
IdentityAssessment identify(const GsIStrings& reported, const IdentityHints& hints,
                            const BehaviourSignals& behaviour,
                            const std::vector<OuiEntry>& oui_table);

// Device-database profile name for a reported model string, "" when nothing matches.
std::string profileForModel(const std::string& vendor, const std::string& model);

// Persistence key for probe findings: model + firmware + serial when the device gave
// them, else the endpoint. Keyed on identity rather than address so a printer that
// moves to a new DHCP lease keeps its probe results (docs/capability-profiles.md).
std::string identityKey(const GsIStrings& reported, const std::string& endpoint);

}  // namespace pd
