#pragma once

#include <string>
#include <vector>

#include "printerdriver/capability_profile.hpp"

// The device database (docs/device-database.md "Device database layout"). Shipped as
// code rather than as a data file because the core must build inside iOS, Android,
// Flutter and Linux-agent toolchains with no package manager and no runtime asset
// loading anywhere in the loop.
//
// Two rules govern every entry:
//
//  1. **The profile does not dictate the capability — it provides defaults; the probe
//     overrides them.** An entry marked GsR1 is a starting point for a device nobody
//     has interrogated yet, not a claim that GS ( H is absent.
//  2. **Media facts are facts.** Roll width and printable dots are recorded
//     separately and neither is derived from the other.
//
// Entries whose completion mechanism the ESC/POS engine cannot drive (Star's checked
// block, ePOS JobID) are carried as data so the fleet can be described honestly. The
// engine refuses such a job with FailureReason::Unsupported rather than silently
// printing without a fence.

namespace pd::devices {

// --- Epson (docs/capability-profiles.md §1-§4) ------------------------------------
// GS ( H fn 48 is in Epson's own model-specific command tables for these families.

CapabilityProfile epson_tm_t20();
CapabilityProfile epson_tm_t88();
CapabilityProfile epson_tm_m();
CapabilityProfile epson_tm_t70();
// 76 mm dot impact. Different mechanics, different geometry, its own profile.
CapabilityProfile epson_tm_u220();
// TM-i / DT: ESC/POS plus a printer-side spooler with a queryable JobID.
CapabilityProfile epson_tm_i();

// --- Rongta (docs/capability-profiles.md §5) --------------------------------------
// The RP80 command manual contains GS ( H fn 48, and the same manual documents GS I
// answering as an Epson TM-T88V.

CapabilityProfile rongta_rp58();
CapabilityProfile rongta_rp80();
CapabilityProfile rongta_rp3xx();

// --- Xprinter (docs/capability-profiles.md §7) ------------------------------------

CapabilityProfile xprinter_pos58();
CapabilityProfile xprinter_pos80();
// S-series, including the XP-S260M on which GS ( H is hardware-confirmed.
CapabilityProfile xprinter_s_series();

// --- Sewoo / Partner Tech (docs/capability-profiles.md §6) ------------------------

CapabilityProfile sewoo_slk_ts();
// Partner-branded Sewoo/Aroot SLK-TS200 platform; GS ( H still needs a hardware probe.
CapabilityProfile partner_rp110();

// --- Star (docs/device-database.md "Vendor stacks") -------------------------------
// StarPRNT and the checked-block API, neither of which this core implements: these
// are profile data only, so a Star unit in the fleet can be described rather than
// misdriven through Star's ESC/POS emulation.

CapabilityProfile star_tsp100();
CapabilityProfile star_tsp650();
CapabilityProfile star_mcprint();

// --- Bixolon ----------------------------------------------------------------------
// "ESC/POS compatible" vendor language; the vendor status SDK is the documented
// primary path, so these carry prefer_vendor_sdk and never assume GS ( H.

CapabilityProfile bixolon_srp330();
CapabilityProfile bixolon_srp350();
CapabilityProfile bixolon_srp380();
CapabilityProfile bixolon_q_series();

// --- Citizen ----------------------------------------------------------------------

CapabilityProfile citizen_cts_58_80();
CapabilityProfile citizen_cts_fast();
// CT-S4500 class: takes 58-112 mm media and prints at most 104 mm.
CapabilityProfile citizen_cts_wide();

// --- Generic ----------------------------------------------------------------------
// docs/capability-profiles.md §8: "generic ESC/POS" is not a profile, it is UNKNOWN
// ESC/POS DEVICE. Assume nothing beyond printable bytes and promote after a probe.

CapabilityProfile generic_80();
CapabilityProfile generic_58();

// Database lookup by the names used in the layout above, e.g. "rongta_rp80".
bool exists(const std::string& name);
CapabilityProfile byName(const std::string& name);
// Every entry, in database order. Used by pdctl and by the tests that assert the
// media facts of the whole table at once.
std::vector<std::string> names();

}  // namespace pd::devices
