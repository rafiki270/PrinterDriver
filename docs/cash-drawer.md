# Cash Drawer Support

Research-verified spec (2026-08-09). Cash drawers are easier to control reliably than
print completion: on good implementations we can fire the drawer **and detect that it
physically changed from closed to open**. Architectural rule: **the drawer is a separate
printer peripheral capability** — own electrical profile, command method, feedback
method. Never a generic ESC/POS feature.

## How printer-driven drawers work

A static POS drawer is a passive solenoid latch plus a microswitch. The printer does two
jobs: **output** (energise the solenoid ~100–300 ms, typically 24 V) and **input** (read
the drawer microswitch where wired). UnifiedPOS/POS-for-.NET formalises exactly this:
every cash-drawer service supports `OpenDrawer()`; status-capable devices expose
`DrawerOpened` + a status event. So our API returns not `{sent: true}` but:

```json
{ "result": "OPEN_VERIFIED", "previousState": "CLOSED", "currentState": "OPEN",
  "channel": 1, "pulseMs": 200 }
```

## 1. Epson — reference implementation (fully documented)

Queued pulse: **`ESC p m t1 t2`** (`1B 70 m t1 t2`): `m = 0/48` → pin 2, `m = 1/49` →
pin 5; ON = t1×2 ms, OFF = t2×2 ms. Example `1B 70 00 64 C8` = channel 1, 200 ms ON,
400 ms OFF. Status: **`GS r 2`** (`1D 72 02`) or `GS r 50` — drawer kick-out connector
status, bit 0 = pin 3 HIGH/LOW. **ASB can report drawer-connector changes automatically**
(no polling). Ideal flow: `GS r 2` (CLOSED) → `ESC p` → wait for status change → OPEN →
`OPEN_VERIFIED`.

**Epson DK connector** (modular): 1 = frame ground · 2 = drive 1 · **3 = open/close
input** · 4 = +24 V · 5 = drive 2 · **6 = signal ground**. Solenoid between +24 V and a
drive pin; switch between sense input and signal ground. **≥24 Ω solenoid, ~≤1 A.**
Profile id: `epson_dk`. `ESC p` applies across T20, T70, T82, T88, m10/m30/m50 and many
more per Epson's applicability tables. For ePOS installations, drive the drawer through
**the ePOS peripheral API**, never raw bytes smuggled into print XML (Epson documents
drawer pulse/status via ePOS SDK alongside ESC/POS/JavaPOS/OPOS).

## 2. Bixolon — Epson-like electrics, own SDK surface

SRP-350V/352V pinout matches Epson's layout (1 FG · 2 drive 1 · 3 open/close input ·
4 +24 V · 5 drive 2 · 6 signal ground); **≥24 Ω, max 1 A**, documented pulse/recovery
limits. WebPrint SDK: `makeDKout({connector:'pin2'|'pin5', duration: 50|100|150|200|
250|300|400|500})`, **default pin2/200 ms** → use 200 ms as the Bixolon profile default.
Implement SRP-330/350/350plus/380/Q per the exact model manual or Bixolon API — do not
assume every firmware implements every Epson extension identically.

## 3. Citizen — documented, with a serialization quirk

Common command reference (CT-S280/300/310/2000/801/851/601/651/310II/251/D/E/751/4500…)
explicitly includes **`ESC p`** and **`GS r 2` / `GS r 50`**. CT-S4500 pinout: 1 FG ·
2 DRAWER1 · 3 DRSW switch input · 4 VDR drive supply · 5 DRAWER2 · 6 GND; 24 V, ≥24 Ω,
~≤1 A; **drawer 1 and 2 cannot be driven simultaneously**. Quirk for affected models:
**the drawer output cannot fire while printing** → `canKickDuringPrint = false`; the
daemon serializes receipt completion → drawer pulse.

## 4. Star — SAME-LOOKING CONNECTOR, DIFFERENT ELECTRICS

TSP100 hardware manual: 1 FG · 2 DRD1 · **3 = +24 V (!!)** · 4 = +24 V · 5 DRD2 ·
**6 = DRSNS sense (!!)** — Epson puts SENSE on 3 and GND on 6; Star puts +24 V on 3 and
SENSE on 6. **A cable that fits is not electrically correct** — APG publishes a
printer-to-cable compatibility matrix (12 V, 24 V, dual-voltage families) for exactly
this reason. Software: use Star's **`appendPeripheral(...)`** (StarPRNT), not Epson
`ESC p`. Status: `drawer1OpenCloseSignal` / `drawer2OpenCloseSignal`; StarXpand exposes
`drawerOpenCloseSignal` across TSP100IV, mC-Print, TSP650II, etc. **Star warns that
true's meaning (open vs closed) depends on the attached drawer/switch** → the daemon
performs one-time calibration ("close drawer [READ], open drawer [READ]") and persists
`highMeansOpen` instead of assuming polarity.

## 5. SNBC / New Beiyang

BTP-R880NP: 6-position modular drawer interface, 24 V, ~≤1 A drive, outputs + switch
input; programming manual documents **`ESC p`** and realtime **`DLE DC4 n m t`** pulse.
Profile `snbc_btp_r880` is genuinely documented. Prefer queued `ESC p` over the realtime
variant in normal operation.

## 6. Xprinter — voltage is a MODEL fact, not a family fact

XP-S260M officially: drawer output **DC 24 V / 1 A** — electrically fine. But no
current Xprinter-hosted programmer reference proves the raw pulse/status commands, so:
`drawerPort/voltage/maxCurrent = DOCUMENTED; ESC p / drawer status / channel 2 = PROBE
REQUIRED`. **Critical: several Xprinter 58-mm models officially specify 12 V / 1 A**
while 80-mm units are commonly 24 V — never make `Xprinter = 24 V` a family assumption.

## 7–9. Rongta, Sewoo, Partner

Rongta RP336S (80 mm): 6-wire socket, 24 V/1 A — but smaller Rongta units differ, so
`rongta_rp80_family: drawerVoltage ≈ 24 V [model-verified]`, **`rongta_rp58_family: DO
NOT INHERIT`**; no trusted manufacturer-hosted `ESC p` + switch semantics across the
range → probe before enabling. Sewoo SLK-TS200: cash-box control, 6-wire, 24 V/1 A —
but do not auto-apply its command implementation to the Partner RP-110 on OEM
resemblance alone. Partner RP-110: `cashDrawer = PROBE_REQUIRED` (establishable in
minutes on a unit).

## The electrical-standard warning (GIANT LETTERS)

**RJ11/RJ12-looking drawer connectors are not a universal electrical standard.**
Discovery must NEVER blindly fire outputs on an unknown drawer. First identify printer
model → drawer port type → expected voltage → correct cable. Then actuate.

## Two drawers, one switch

Many printers expose drive 1 + drive 2 (APG sells dual-drawer adapters). But
Epson-style hardware commonly has **two outputs, ONE switch input** — you can kick
drawer 1 and 2 independently but only learn "some attached drawer is open". Model as
`channels = 2` and `independentStatusChannels = false` — separate capabilities.

## Buzzer conflict

Epson documents that with the optional external buzzer enabled, the pulse that would go
to the drawer connector can instead sound the buzzer; Star's external-device port is
likewise shared. Profile: `peripheralPortSharedWithBuzzer = true` — don't assume both
coexist.

## Implementation model

Separate from PrinterCapabilities:

```
DrawerCapabilities {
  present
  electrical { connector, voltage, maxCurrent, channelCount }
  kick { method, defaultPulseMs, maxPulseMs, cooldownMs, canKickDuringPrint }
  status { available, method, sharedBetweenDrawers, polarity }
  port { sharedWithBuzzer }
  evidence { documented, probed }
}
Methods: EPSON_ESC_P | EPSON_EPOS | STAR_PRNT | BIXOLON_SDK | CITIZEN_ESC_P |
         SNBC_ESC_P | VENDOR | UNSUPPORTED
```

**API states (never a boolean):** `CLOSED · OPEN · OPENING · KICK_SENT_UNVERIFIED ·
OPEN_VERIFIED · FAILED_TO_OPEN · NO_SENSOR · UNKNOWN` — distinguishing "we sent the
command" from "we saw the physical switch change", same principle as printing.

**Opening sequence (status-capable drawers):**
1. Read sensor first — already open ⇒ don't pulse again.
2. Acquire the printer/peripheral lock (no receipt/cutter/drawer interleaving).
3. One queued pulse at the profile's duration.
4. Watch/poll the sensor ~1–2 s.
5. Expected transition ⇒ `OPEN_VERIFIED`.
6. Stays closed ⇒ `FAILED_TO_OPEN` (locked drawer, jam, cable, wrong channel…).
7. No status capability/backchannel ⇒ `KICK_SENT_UNVERIFIED`.
8. Enforce the manufacturer cooldown before another pulse.

Default 200 ms for known 24-V printer-driven drawers, but profile-specific (Epson
programmable in 2-ms units; Bixolon's own SDK default is 200 ms).

## Print servers and CUPS

Direct 9100 with a working return channel ⇒ kick + read the switch directly. Through a
cheap USB print server, the kick may travel forward while the sensor response doesn't
come back ⇒ that path supports `KICK_SENT_UNVERIFIED`, not `OPEN_VERIFIED`, until
bidirectionality is proven. Do NOT ship `receipt + ESC p → CUPS` as the primary
mechanism (CUPS filters/backends + scheduling don't serve raw-receipt timing/exclusive
access well) — route `drawer.open()` through pd-agent on the same printer-facing
transport. On Windows/OPOS, use the proper CashDrawer abstraction (`CapStatus` for
open/closed status).

## Fleet profiles

| Printer/family | Kick | Electrical | Verify opened? | Status |
|---|---|---|---|---|
| Epson TM-T20III | ESC p | 24 V Epson DK | Yes — GS r 2 / ASB | 🟢 Fully documented |
| Epson TM-T88 V/VI/VII | ESC p / SDK/ePOS | 24 V Epson DK | Yes | 🟢 Fully documented |
| Epson m30/m50 | ESC p / SDK | 24 V Epson DK | Yes | 🟢 Fully documented |
| Star TSP100/TSP650 | Star peripheral cmd | **Star DK pinout** | Yes via Star status | 🟢 Documented |
| Star mC-Print2/3 | Star SDK | Star DK | Yes | 🟢 Documented |
| Bixolon SRP-350 family | SDK / documented cmds | 24 V Epson-like | per model/API | 🟢 Strong |
| Bixolon SRP-330/380/Q | model SDK/cmd profile | model-specific | model-specific | 🟡 Profile individually |
| Citizen CT-S family | ESC p | 24 V Epson-like | GS r 2 | 🟢 Documented |
| SNBC BTP-R880 | ESC p / DLE DC4 | 24 V | status-capable | 🟢 Documented |
| Xprinter XP-S260M | likely ESC/POS kick | 24 V/1 A documented | needs probe | 🟡 |
| Rongta RP80 family | likely ESC/POS | 24 V/1 A verified models | needs probe | 🟡 |
| Sewoo SLK-TS200 | vendor/ESC-POS | 24 V/1 A | needs command verification | 🟡 |
| Partner RP-110 | unknown until probe | needs exact profile | unknown | 🟠 |
| Generic 80-mm ESC/POS | disabled initially | unknown | unknown | 🔴 Probe |
| Generic 58-mm ESC/POS | disabled initially | **frequently 12 V** | unknown | 🔴 Never assume 24 V |

## pdctl

`pdctl drawer-probe <host>` — does **not** fire unknown drawers. Prints documented
capability (connector/voltage/current), software capability (ESC p / GS r 2
unverified), then a NON-DESTRUCTIVE sensor test: "Close the drawer… status=0; Open the
drawer manually… status=1 ⇒ ✓ switch detected, ✓ HIGH = OPEN" (persists polarity). Only
with a known electrical profile: `pdctl drawer test --channel 1 --pulse 200` →
`Before kick CLOSED · Pulse 200 ms · Sensor changed 143 ms after kick · ✓ OPEN_VERIFIED`
or `✗ FAILED_TO_OPEN (locked / wrong channel / cable / jam)`.

## Order of implementation

Epson first (kick + physical open verification fully testable against documented
protocol), then Star, Citizen, Bixolon, then controlled probes of XP-S260M, Rongta,
RP-110 — covering the overwhelming majority of static installations without unsafe
voltage or pinout assumptions.
