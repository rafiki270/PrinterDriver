# Capability Profiles & Printer Identification

Research addendum (2026-08-08) to [sdk-spec.md](sdk-spec.md) §8/§9. Core conclusion:
**stop treating "ESC/POS" as one protocol.** Build model/firmware capability profiles,
identify hardware with multiple fingerprints (never `GS I` alone — see the Rongta trap),
and probe before trusting. Several "generic ESC/POS" printers expose commands their
marketing manuals never mention — including the completion mechanism this whole SDK is
built on.

## Findings summary

| Printer/family | Completion mechanism | Status/error feedback | Obscure capabilities | Confidence |
|---|---|---|---|---|
| **Epson TM-T20III** | **`GS ( H` fn=48 confirmed by Epson docs** | DLE EOT, GS r, ASB | IDs, maintenance counters, settings readback, buffer clear, power-off, test print, NV memory queries | **Officially confirmed** |
| **Epson TM-T88VI** | **`GS ( H` fn=48 confirmed** | DLE EOT, GS r, ASB + extended ASB | Bluetooth config read/write, thermal-head control, graphics drawing, maintenance counters, settings introspection | **Officially confirmed** |
| **TM-T88VII / newer Epson TM** | `GS ( H`; newer models add job/batch primitives | Excellent | batch printing, end-job commands on some models, extended realtime status | **Officially confirmed per model** |
| **Epson TM-i/ePOS** | **Proper JobID + queryable print result** | Rich error codes/status | spooler, print forwarding, persistent result log until power-off | **Strongest software API** |
| **Rongta RP80-family** | ~~`GS ( H` fn=48 is in Rongta's own command manual~~ — **withdrawn, see §5 and [compatibility-brief.md](compatibility-brief.md) §13**; `GS ( H` is `UNVERIFIED / PROBE` | DLE EOT, DLE ENQ, GS r, ASB | fake Epson IDs, cutter error, recovery/restart, macros, test page, barcode positioning | **Probe per unit; ESC/POS + OPOS documented, the extension is not** |
| **Partner RP-110** | Likely Epson-like ESC/POS status set | paper/cover sensors known | OEM-related to **Sewoo SLK-TS200** | **Needs hardware probe** |
| **Xprinter XP-S260M** | **`GS ( H` proven on our hardware** | status + errors | Xprinter one-ticket-control / working-state mechanism | **Hardware confirmed** |
| Generic unknown ESC/POS | `GS r 1` safest initial ordered fence | DLE EOT if available | wildly inconsistent | **Probe only** |

The TM-T20III and TM-T88VI entries are not educated guesses: Epson publishes
model-specific command lists and explicitly lists `GS ( H` Function 48 on both.

## 1. Epson TM-T20III — full-featured reference profile

Epson's model-specific command reference gives it a surprisingly large command set,
explicitly including `GS ( H` fn=48 process-ID responses.

Physical: 203×203 dpi; 80 mm paper = 72 mm / **576 dots** (58 mm config = 52.5 mm /
420 dots); up to 250 mm/s; head rated 150 km; cutter rated 1.5 M cuts; ~208 KB
downloaded-graphics buffer; 256 KB NV graphics. Ethernet models speak **both ESC/POS and
ePOS-Print XML**.

Completion/status: `DLE EOT 1–4` (real-time, not FIFO), `GS r n` (queued), `GS ( H`
fn=48 (process ID / completion response) — all documented.

Officially exposed extras worth surfacing through `pdctl`:

- `DLE ENQ` — recover/restart after certain errors
- `DLE DC4 fn=1` — realtime peripheral pulse; `fn=2` — **controlled power-off**;
  `fn=3` — realtime buzzer; `fn=8` — **clear buffers**
- `GS ( A` — built-in test print; `GS ( D` — enable/disable realtime commands
- `GS ( E fn=4` — **read memory-switch settings**; `fn=6` — **read customized
  settings**; `fn=12` — read serial-interface config; `fn=16` — read USB conditions
- `GS ( K fn=48` — print-control mode; `fn=50` — print-speed control
- `GS I` — printer identification; `GS g 2` — **read maintenance counters**; `GS a` — ASB
- `ESC u` / `ESC v` — obsolete peripheral/paper status; `ESC i` / `ESC m` — old cutters
- NV/download graphics storage querying + enumeration
- QR, PDF417, MaxiCode, Aztec, DataMatrix, GS1 and Composite Symbology

## 2. Epson TM-T88VI — even richer

`GS ( H` fn=48 explicitly in its command table, plus `DLE EOT`, `DLE ENQ`, `DLE DC4`,
`GS r`, `GS a`, `GS I`, `GS g`. Additional:

- **Extended ASB:** `FS ( e` — status back for optional/extended devices
- **Bluetooth config:** `GS ( E fn=13` (set) / `fn=14` (read)
- **Thermal-head energising control:** `GS ( K fn=97` — number of parts head
  energisation is divided into (heat/load/throughput experiments)
- **Printer-side vector graphics:** `GS ( Q fn=48` (line) / `fn=49` (rectangle)
- `GS g 2` maintenance counters — the device can report **cutter/head usage** itself
  rather than us tracking receipt counts

## 3. Newer Epson TM models — one profile per model, not "epson"

Epson publishes exact per-model command matrices; use them. Examples: **TM-T88VII** adds
`ESC ( Y` **batch printing**; some newer mechanisms support **`FF` in Standard Mode as an
explicit end-job**; **TM-m30III-H** adds `DLE DC4 fn=7` (transmit selected status in
realtime). Discovery should identify exact model + firmware, then select
`epson_t20iii` / `epson_t88vi` / `epson_t88vii` / `epson_m30ii` / `epson_m30iii` / …
Epson now publishes one model-specific ESC/POS capability database covering these.

## 4. Epson ePOS / TM-i — decides sdk-spec §11.6

**Worth supporting eventually.** It provides what raw ESC/POS fundamentally lacks: a
printer-side **JobID** with a queryable result.

- Spooler **disabled**: result returned **after printing**.
- Spooler **enabled**: initial `JobID + result=true` means **spooled, not printed**;
  querying the JobID later yields effectively PRINT SUCCESS / PRINT FAILED + status /
  STILL PRINTING. Documented by Epson.

```
POS ── submit job ABC123 ──▶ printer spooler ── queued / printing / success / failed
```

**Caveat:** power-off clears the spooler and its print-result log on these TM-i/DT
implementations — so `UNKNOWN` survives for power-loss windows even here.

Decision: implement both `transport: escpos_tcp` and `transport: epson_epos`; prefer
ePOS on known TM-i hardware.

## 5. Rongta RP80 family — the major surprise

> **CORRECTED, and this section is now wrong.**
> [compatibility-brief.md](compatibility-brief.md) §13 supersedes it and wins on
> conflict. The claim below rests on a copy of an RP80 manual that is not hosted by
> Rongta; **no currently manufacturer-hosted Rongta command reference proving `GS ( H`
> fn 48 was found.** The device database was changed to match: every Rongta entry now
> carries `GS ( H` as `Provenance::Unverified` and stays on the queued `GS r 1` fence
> until a probe promotes it per unit. `ESC/POS` and `OPOS` remain documented; the Epson
> feedback extension does not, and "ESC/POS compatible" never implied it.
>
> The paragraph is kept rather than deleted because the mistake is instructive: an
> unverified secondary source read as a primary one is exactly the failure mode the
> provenance system exists to make visible.

Rongta's own RP80 command set includes `GS ( H pL pH fn m d1 d2 d3 d4, fn=48` — the
manual calls it "Set the process ID response", storing the four-byte process ID for the
data processed immediately before it. **The Rongta fleet likely supports exactly the
completion scheme already proven on the Xprinter.**

Status set: `DLE EOT n`, `DLE ENQ n`, `DLE DC4`, `GS r`, `GS a`, `GS I`, `GS ( H`, plus
cutter and recovery. `DLE EOT 3` explicitly: bit 3 = autocutter error, bit 5 =
unrecoverable error, bit 6 = auto-recoverable (documented to indicate printing stopped
from excessive head temperature or cover opened during printing).

**Error recovery (never automatic — kitchen duplication risk; expose as deliberate
`pdctl recover`):**
- `DLE ENQ 1` — recover from error and **resume from the line where the error occurred**
- `DLE ENQ 2` — recover while **clearing the receive and print buffers**

**The identity trap:** Rongta's `GS I` implementation returns
`Manufacturer: "EPOSN"`, `Printer name: "TM-T88V"` — their own manual shows it
impersonating an Epson TM-T88V. **Never identify printers by `GS I` alone**, or a Rongta
gets the Epson profile.

More obscure Rongta commands for the research database: `ESC B n t` buzzer; `ESC i` /
`ESC m` / `GS V` cutter variants; `GS x` barcode left-space; `GS :` / `GS ^`
define/execute macros (store up to **2048 bytes**); `DC2 T` (12 54) built-in **test
page**; sizeable page-mode and Chinese/Kanji extensions (88-page manual).

Profile change — from `rongta_escpos: completion=gs_r` to:

```
rongta_rp80_family:
    tryGSProcessId = true      tryGSrFence = true
    supportsASB = true         supportsDLEEOT = true
    supportsDLEENQ = true      detectCutterError = true
    gsIIsNotTrusted = true
```

## 6. Partner Tech RP-110 = Sewoo SLK-TS200 platform

Partner documents: 203 dpi, 72 mm print area, 250 mm/s, USB + serial + Ethernet
standard (Bluetooth 5/BLE and Wi-Fi variants), paper-end + cover-open sensors,
full/partial guillotine cut, 1.5 M cutter cycles, ESC/POS-compatible.

**ENERGY STAR certification records group `SEWOO SLK-TS200`, `RP-110`, `RP-110B`,
`RP-110W`, `SLK-TS200B`, `SLK-TS200H`, … under one certified product family** — the
RP-110 appears to be a Partner-branded Sewoo/Aroot SLK-TS200. Sewoo's SLK-TS200 docs:
203 dpi, 72 mm / 576 dots, 250 mm/s, ESC/POS, PDF417 + QR; Windows/Linux/Mac/OPOS/
JavaPOS/mobile SDKs.

**Not found:** an authoritative public SLK-TS200 programmer's manual comparable to
Epson's. User-manual command list shows normal ESC/POS (`DLE EOT`, `DLE ENQ`, …) but
`GS ( H` must be probed on the RP-110 itself — next interesting hardware probe after the
Rongta.

```
partner_rp110:
    OEM = Sewoo/Aroot SLK-TS200 family
    DLEEOT = likely   DLEENQ = likely   ASB = likely   GSr = likely
    GSHProcessID = unknown (probe)
```

## 7. Xprinter XP-S260M — known-good clone reference

Specs: 203 dpi, 576 dots/line (~72 mm), max 260 mm/s, paper-end + cover-open +
black-mark sensors, USB + serial + LAN, ESC/POS, Windows/JPOS/OPOS/Linux/Android/Mac
drivers, iOS/Android/Windows SDK, cutter 1.5 M cuts, head 150 km. And the stronger fact
from the actual unit: **`GS ( H` = WORKING** (probed + 100-receipt soak).

Xprinter also documents a **one-ticket-one-control** mechanism against missed/overlapping
kitchen tickets, defining the "working state" as receipt processing **up to execution of
the cut instruction** — worth probing those vendor state commands even though `GS ( H`
already covers our primary completion model.

## 8. "Generic ESC/POS" is not a profile

Clones omit commands, implement older Epson semantics, implement newer commands
partially, lie about identity, change behaviour across firmware, and reuse command bytes
for proprietary features. Generic therefore means **UNKNOWN ESC/POS DEVICE**: first
connection runs a **non-destructive capability interrogation**, then the device is
promoted to a real profile.

## Identification: multi-signal fingerprinting

Because `GS I` can lie, identification combines: **MAC OUI · HTTP/server banner · SNMP
(if available) · GS I (untrusted) · command behaviour · known response quirks ·
capability probe.** Example target output:

```
pdctl probe 192.168.1.50

Fingerprint
──────────────────────────────────
TCP/9100                  YES
MAC OUI                   Rongta
GS I manufacturer         EPOSN
GS I model                TM-T88V
Identity trusted          NO

Status
──────────────────────────────────
DLE EOT 1/2/3/4           YES
GS r 1                    YES
GS a ASB                  YES

Job completion
──────────────────────────────────
GS ( H fn48               YES
response frame            Epson-like
ordered completion        YES

Mechanism
──────────────────────────────────
autocutter                YES
cutter error status       YES
cover sensor              YES
paper-end sensor          YES

Recovery
──────────────────────────────────
DLE ENQ support           YES
buffer clear              UNKNOWN

Identification
──────────────────────────────────
probable vendor           RONGTA
probable profile          rongta_rp80
confidence                97%
```

Probe results are **persisted keyed by model + serial + firmware** — no destructive
re-probing on every boot. The automatic probe NEVER sends: `DLE ENQ` (resumes/clears),
`DLE DC4 fn=2` (power-off), `DLE DC4 fn=8` (clear buffers) — those are deliberate
operator actions only.

## Compositional profile hierarchy

Capabilities are compositional, not six flat profiles:

```
PrinterProfile
├── identity      vendor · model · firmware · fingerprintConfidence
├── transport     rawTcp9100 · serial · usb · epos
├── completion    processIdGS_H · queuedGSr · vendorIdle · eposJobId
├── status        DLEEOT · ASB · extendedASB · cutterError
├── recovery      DLEENQResume · DLEENQClear · clearBuffers
└── quirks        extraFeedBeforeCut · unreliableIdentity · delayedStatus
                  · oneJobAtATime · responseParser
```

A TM-T20III becomes a set of capabilities rather than a huge inheritance case.

## Support-matrix status changes (supersedes sdk-spec §9 confidence notes)

| Printer | New status |
|---|---|
| **Epson TM-T20III** | 🟢 `GS ( H` **officially confirmed** (no longer "very likely") |
| **TM-T88VI** | 🟢 `GS ( H` **officially confirmed**; ePOS worthwhile as higher-level alternative |
| **newer Epson TM** | 🟢 Extremely capable; use Epson's exact per-model command matrix |
| **Rongta** | 🟡→🟢 RP80 manual **contains `GS ( H` fn48** — probe models; much better than generic |
| **Partner RP-110** | 🟡 **Sewoo SLK-TS200 family**; status commands likely, `GS ( H` needs probe |
| **XP-S260M** | 🟢 `GS ( H` hardware-proven (probe + 100-receipt soak) |
| **generic ESC/POS** | 🔴 Assume nothing beyond printable bytes; **dynamically promote after probe** |

Potentially **four of the five named printer families give a real completion
acknowledgement** — the feedback architecture is considerably more viable than it first
looked.

> **2026-08-09:** superseded where it conflicts by [compatibility-brief.md](compatibility-brief.md) — notably: Rongta `GS ( H` is **UNVERIFIED** (no manufacturer-hosted manual found), Xprinter `GS ( H` is **PROBED, not documented**, and per-capability provenance (DOCUMENTED/PROBED/UNVERIFIED) is now required.
