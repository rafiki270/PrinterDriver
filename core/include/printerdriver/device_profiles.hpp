#pragma once

#include <string>
#include <vector>

#include "printerdriver/capability_profile.hpp"

// The device database (docs/device-database.md "Device database layout",
// docs/compatibility-brief.md §26). Shipped as code rather than as a data file because
// the core must build inside iOS, Android, Flutter and Linux-agent toolchains with no
// package manager and no runtime asset loading anywhere in the loop.
//
// Three rules govern every entry:
//
//  1. **The profile does not dictate the capability — it provides defaults; the probe
//     overrides them.** An entry marked GsR1 is a starting point for a device nobody
//     has interrogated yet, not a claim that GS ( H is absent.
//  2. **Media facts are facts.** Roll width, printable width in millimetres and
//     printable width in dots are recorded separately and none is derived from another
//     (docs/compatibility-brief.md §18).
//  3. **Every capability carries its provenance** (§28). Documented means the
//     manufacturer's own command documentation says so; Probed means this driver asked
//     the hardware; Unverified means neither. "ESC/POS compatible" on a datasheet is
//     Unverified, and the whole brief exists because that distinction kept collapsing.
//
// Entries whose completion mechanism the ESC/POS engine cannot drive (Star's checked
// block, ePOS JobID) are carried as data so the fleet can be described honestly, and so
// are the entries that are not ESC/POS at all (Zebra ZPL/CPCL, Brother raster/ESC-P).
// The engine refuses such a job with FailureReason::Unsupported, before a single byte
// reaches the link, rather than printing confetti and reporting success.
//
// Family entries and model entries coexist. `epson_tm_t88` describes what the family
// shares; `epson_tm_t88vi` describes the unit on the counter. A caller that knows only
// the family picks the family; nothing is lost either way, because both are defaults.

namespace pd::devices {

// --- Epson (docs/compatibility-brief.md §2-§6) ------------------------------------
// GS ( H fn 48 is in Epson's own model-specific command tables: this is the one family
// here whose completion extension is manufacturer-documented rather than inferred, and
// the only one whose shipped defaults therefore carry Provenance::Documented.

CapabilityProfile epson_tm_t20();  // family
CapabilityProfile epson_tm_t20ii();
CapabilityProfile epson_tm_t20iii();
CapabilityProfile epson_tm_t20iv();
CapabilityProfile epson_tm_t70();  // family
CapabilityProfile epson_tm_t70ii();
CapabilityProfile epson_tm_t82();
CapabilityProfile epson_tm_t88();  // family
CapabilityProfile epson_tm_t88iv();
CapabilityProfile epson_tm_t88v();
CapabilityProfile epson_tm_t88vi();
CapabilityProfile epson_tm_t88vii();
CapabilityProfile epson_tm_m();  // family
CapabilityProfile epson_tm_m10();
CapabilityProfile epson_tm_m30();
CapabilityProfile epson_tm_m30ii();
CapabilityProfile epson_tm_m30iii();
CapabilityProfile epson_tm_m50();
CapabilityProfile epson_tm_m50ii();
// Portables. Manual tear as standard; Bluetooth is Classic *and* BLE, never "bluetooth".
CapabilityProfile epson_tm_p20();
CapabilityProfile epson_tm_p20ii();
CapabilityProfile epson_tm_p60();
CapabilityProfile epson_tm_p60ii();
CapabilityProfile epson_tm_p80();
CapabilityProfile epson_tm_p80ii();
// The Auto Cutter Model is separately sold hardware with a mechanism the standard unit
// does not have, so it is a distinct capability profile rather than a flag (§6).
CapabilityProfile epson_tm_p80ii_autocutter();
// 76 mm dot impact. Different mechanics, different geometry, its own profile.
CapabilityProfile epson_tm_u220();
CapabilityProfile epson_tm_u220ii();
// TM-i / DT: ESC/POS plus a printer-side spooler with a queryable JobID.
CapabilityProfile epson_tm_i();

// --- Rongta (docs/compatibility-brief.md §13) -------------------------------------
// CORRECTED. An earlier revision of this database assumed GS ( H fn 48 from the RP80
// family manual. No currently manufacturer-hosted Rongta command reference proving it
// was found, so every Rongta entry now carries GS ( H as Unverified and leaves the
// promotion to the probe. ESC/POS and OPOS are documented; the Epson feedback
// extension is not, and "ESC/POS compatible" never implied it.

CapabilityProfile rongta_rp58();
CapabilityProfile rongta_rp80();
CapabilityProfile rongta_rp3xx();
CapabilityProfile rongta_rp8xx();

// --- Xprinter (docs/compatibility-brief.md §14) -----------------------------------
// Xprinter's public documentation does not prove GS ( H. The XP-S260M answers it on
// real hardware, which is stronger than the datasheet — but it is a probe result, so
// the shipped default stays Unverified and `pdctl probe` writes Probed.

CapabilityProfile xprinter_pos58();
CapabilityProfile xprinter_pos80();
CapabilityProfile xprinter_s_series();
CapabilityProfile xprinter_v_series();
CapabilityProfile xprinter_portable();

// --- Sewoo / Partner Tech (docs/compatibility-brief.md §15) -----------------------
// Not enough public programmer documentation for the Epson completion extensions:
// DLE EOT, GS r, GS ( H and ASB are all probe questions here, which is exactly what
// `pdctl probe` exists for.

CapabilityProfile sewoo_slk_ts();
CapabilityProfile partner_rp110();
CapabilityProfile partner_rp710();

// --- Star (docs/compatibility-brief.md §7-§8) -------------------------------------
// StarPRNT and beginCheckedBlock()/endCheckedBlock(), neither of which this core
// implements: profile data only, so a Star unit in the fleet can be described rather
// than misdriven through Star's ESC/POS emulation and reported as fenced when it is not.

CapabilityProfile star_tsp100();  // family
CapabilityProfile star_tsp100iii();
CapabilityProfile star_tsp100iv();
CapabilityProfile star_tsp650();
CapabilityProfile star_mcprint();  // family
CapabilityProfile star_mcprint2();
CapabilityProfile star_mcprint3();
CapabilityProfile star_sm_s210();
CapabilityProfile star_sm_s220();
CapabilityProfile star_sm_s230();
CapabilityProfile star_sm_l200();
CapabilityProfile star_sm_l300();
CapabilityProfile star_sm_t300();
CapabilityProfile star_sm_t400();

// --- Bixolon (docs/compatibility-brief.md §9-§10) ---------------------------------
// Bixolon publishes actual per-model command manuals and SDKs, so these are integrated
// per model rather than as mystery clones. Policy until each family's manual is loaded
// and validated: the vendor status SDK is the documented primary path, DLE EOT and
// GS r are probe-or-document per model, and GS ( H is never assumed.

CapabilityProfile bixolon_srp330();
CapabilityProfile bixolon_srp350();
CapabilityProfile bixolon_srp380();
CapabilityProfile bixolon_q_series();  // family
CapabilityProfile bixolon_srp_q200();
CapabilityProfile bixolon_srp_q300();
CapabilityProfile bixolon_srp_b300();
CapabilityProfile bixolon_srp_f310();
CapabilityProfile bixolon_srp_275();  // impact
CapabilityProfile bixolon_spp_r200();
CapabilityProfile bixolon_spp_r210();
CapabilityProfile bixolon_spp_r310();
CapabilityProfile bixolon_spp_r410();

// --- Citizen (docs/compatibility-brief.md §11-§12) --------------------------------

CapabilityProfile citizen_cts_58_80();
CapabilityProfile citizen_cts_fast();
// CT-S4500 class: takes 58-112 mm media and prints at most 104 mm.
CapabilityProfile citizen_cts_wide();
// CMP portables. Citizen publishes ESC/POS, CPCL and ZPL command references for the
// same hardware, which is unusually good for protocol-level work — and the reason
// CapabilityProfile carries a set of languages and not just one.
CapabilityProfile citizen_cmp_20ii();
CapabilityProfile citizen_cmp_30ii();

// --- Zebra (docs/compatibility-brief.md §16) --------------------------------------
// NEVER the generic ESC/POS codepath. Link-OS speaks ZPL and CPCL; sending ESC/POS to
// one produces garbage, so every entry here is refused by the engine with
// FailureReason::Unsupported before any byte is written. They exist so a fleet
// containing them can be inventoried, and so that picking one is an explicit,
// diagnosable refusal instead of a pile of wasted paper.

CapabilityProfile zebra_zq300_plus();
CapabilityProfile zebra_zq500();
CapabilityProfile zebra_zq600_plus();

// --- Brother (docs/compatibility-brief.md §17) ------------------------------------
// Also not ESC/POS: Brother Raster, ESC/P and P-touch Template, plus ZPL II and CPCL on
// some models. `Brother != generic_escpos`, and the engine refuses these the same way.

CapabilityProfile brother_rj2000();
CapabilityProfile brother_rj3000();
CapabilityProfile brother_rj4000();

// --- Generic ----------------------------------------------------------------------
// docs/capability-profiles.md §8: "generic ESC/POS" is not a profile, it is UNKNOWN
// ESC/POS DEVICE. Assume nothing beyond printable bytes and promote after a probe.

CapabilityProfile generic_80();
CapabilityProfile generic_58();
// The escape hatch of §26. Prints and cuts; claims nothing at all — no ordered fence,
// no status, grade E. For hardware nobody has identified, which is a different thing
// from hardware identified as ordinary.
CapabilityProfile generic_unknown();

// Database lookup by the names used in the layout above, e.g. "rongta_rp80".
bool exists(const std::string& name);
CapabilityProfile byName(const std::string& name);
// Every entry, in database order. Used by pdctl and by the tests that assert the
// media facts of the whole table at once.
std::vector<std::string> names();

}  // namespace pd::devices
