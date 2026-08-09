# Printer Feedback & Compatibility Brief

Re-verified against current online manufacturer documentation (2026-08-09), prioritising
technical references, command manuals, SDK documentation and standards over model
knowledge. **Where this brief conflicts with
[capability-profiles.md](capability-profiles.md) or
[device-database.md](device-database.md), this brief wins.**

**The correction:** Epson is the only major ESC/POS family here whose `GS ( H`
process-ID completion is verifiable directly from the manufacturer's public command
documentation. The XP-S260M has independently passed the hardware probe, but Xprinter's
public documentation does not document `GS ( H`. Rongta advertises ESC/POS/OPOS, but no
current Rongta-hosted programming manual verifying `GS ( H` was found. **These
distinctions must exist in the database** — see §28 (DOCUMENTED / PROBED / UNVERIFIED).

## 1. Fundamental architecture

Keep four concepts independent per printer: **DEVICE** (TM-T20III, mC-Print3,
SPP-R310…) · **PHYSICAL INTERFACE** (Ethernet, USB, RS-232, Wi-Fi, BT Classic, BLE) ·
**TRANSPORT/API** (raw TCP 9100, USB bulk, serial stream, IPP, LPD, ePOS-Print, StarIO,
vendor SDK, BT SPP, BLE GATT) · **COMMAND LANGUAGE** (ESC/POS, StarPRNT, ZPL, CPCL,
ESC/P, raster). An Ethernet Epson exposes several simultaneously; a Bluetooth mobile
printer may use classic serial, BLE, or an SDK-specific transport.

## 2. Epson desktop — the reference implementation

Epson publishes a **model-by-model ESC/POS applicability database** (not just "ESC/POS
compatible") covering T20/T70/T80-series equivalents, T88IV–VII, m-series, P-series,
U220 and others. Latest indexed technical-manual revision: February 2026.

**TM-T20III** (verified): direct thermal; 203×203 dpi; 80 mm = 72 mm/576 dots; 58 mm =
52.5 mm/420 dots; 250 mm/s; USB/serial/Ethernet per model, optional WLAN; USB 2.0 Full
Speed **bidirectional bulk IN/OUT**; ESC/POS + ePOS-Print XML. Status/completion:
`DLE EOT`, `GS r`, ASB, **`GS ( H` fn 48 explicitly in Epson's model command table**
("Specifies the process ID response"). Rating: `GS_H_FUNCTION_48 = DOCUMENTED`, `GS_R =
DOCUMENTED`, `DLE_EOT = DOCUMENTED`, `ASB = DOCUMENTED`. No probe needed to establish
the *model* supports it; the probe verifies the *interface path* passes responses back.

Documented beyond printing: `DLE EOT` (realtime state), `DLE ENQ` (error recovery),
`DLE DC4` fn 1 (realtime pulse) / fn 2 (**controlled power-off**) / fn 8 (**clear
buffers**), `GS a` (ASB), `GS r`, `GS ( H` fn 48, `GS I`, `GS g` (maintenance counters),
`GS ( E` (settings introspection), `GS ( L` (NV/downloaded graphics incl.
remaining-capacity queries), macros, self-test; PDF417, QR, MaxiCode, DataMatrix,
Aztec, GS1, composite symbols.

## 3. Epson TM-T88 family — three profiles, not one

- **TM-T88V** — huge installed base; Epson maintains an explicit command table →
  `epson_tm_t88v`.
- **TM-T88VI** — USB bulk bidirectional; dedicated current TRG + command table;
  **included in Epson's `GS ( H` applicability**; `GS ( E` serial/USB config readback;
  OmniLink docs list ESC/POS + ePOS-Print Service. `GS(H)/DLE EOT/GS r/ASB/USB IN+OUT =
  DOCUMENTED; ePOS on relevant network/OmniLink configs`.
- **TM-T88VII** — up to 500 mm/s with appropriate energising configuration (450 mm/s
  standard high-speed; 58 mm limited to 450); **covered by `GS ( H`**.

## 4. Epson mPOS family

Profiles: TM-m10, m30, m30II, m30III, m50, m50II (+H/NT/S variants where behaviour
differs). **TM-m30III** (official): 80/58 mm rolls; max 83 mm roll diameter; ~300 mm/s;
USB-A, USB-B, **USB-C/USB-PD**, 10/100 Ethernet; Wi-Fi on applicable configs;
**Bluetooth 5.0 Dual Mode** on the Wi-Fi/BT config; microSD there; cash-drawer port —
which is why interfaces are represented separately from model identity.

## 5. Epson ePOS / intelligent printers — separate transport

ePOS-Print has real **JobIDs with spooler result retrieval**: with the spooler enabled,
submission returns without waiting; the application later sends an empty print request
with the JobID to retrieve the result. Documented states/errors include Printing,
JobNotFound, cutter errors, cover open, paper empty, mechanical and unrecoverable
faults. Support both `epson: raw_escpos` and `epson: epos_print`; select dynamically.

## 6. Epson portable (Bluetooth/Wi-Fi)

**TM-P20II**: 58 mm paper, 48 mm width, 203 dpi, manual tear; optical paper-end +
cover detectors; USB-C; Wi-Fi model 802.11 a/b/g/n/ac 2.4/5 GHz; BT model **Bluetooth
5.0 Dual Mode (Classic + BLE)**, Class 2, up to 8 stored pairings, **one simultaneous
connection**; **4 KB receive buffer normally, 64 KB for Bluetooth**; 384 KB NV
graphics; USB 2.0 FS bulk IN+OUT. Epson publishes a model-specific P20II command table.
Bluetooth naming: Classic `TM-P20II-xxxxxx`, BLE `TM-P20II-xxxxxx-L`; BLE supported on
iOS and Android (several newer LE PHY features unsupported). **Represent
`bluetoothClassic = true` and `bluetoothLE = true`, never `bluetooth = true`.**

**TM-P80II**: 80 mm, direct thermal, USB-C, Wi-Fi a/b/g/n/ac (Wi-Fi unit), BT 5 Dual
Mode (BT unit); paper-end, black-mark, cover detectors; 4 KB/64 KB buffers as above;
384 KB NV; barcode + PDF417/QR/MaxiCode/DataMatrix/Aztec/GS1. Normal unit is manual
tear; the separately-sold **Auto Cutter Model is a distinct hardware capability
profile**.

## 7–8. Star — native stack, never an ESC/POS clone

**`beginCheckedBlock()` … `endCheckedBlock()`** (StarPRNT SDK): checks whether the whole
data completely printed; `endCheckedBlock` monitors status and returns on completion,
offline, or timeout. **A-grade completion.**

- **TSP100IV**: 80/58 mm (guide), direct thermal, StarPRNT, 250 mm/s, USB-C host,
  USB-A peripheral/Android, Ethernet; wireless variant adds 2.4/5 GHz Wi-Fi + BT.
- **mC-Print3**: 80/58 mm, front exit, 72/~51/48 mm print areas by paper setup, USB,
  Ethernet, BT on variants, up to 400 mm/s on newer versions.
- **Portables** (SDK-listed): SM-S210i/S220i/S230i, SM-T300i/T400i, SM-L200/L300 —
  Bluetooth transports, model-specific emulations; several support StarPRNT **and**
  EscPosMobile mode. **SM-S230i**: 58 mm/48 mm, 80 mm/s, **Bluetooth 5.2** (current
  model), USB 2.0 FS, ~217 g with battery, ~15 h at Star's 5-minute-interval test.
  **SM-L200**: 58 mm, BT 3.0/4.0 dual-mode generation, USB 2.0, ~220 g; current SDK
  identifies it as BLE + StarPRNT-capable. Prefer Star SDK → CheckedBlock over raw
  Bluetooth streams.

## 9–10. Bixolon — first-class, with a strict evidence policy

Families: SRP-330/330II/330III, SRP-350III, SRP-350V/352V, SRP-350plus, SRP-380,
SRP-Q200/Q300, SRP-B300, SRP-F310, SRP-275III (impact). Bixolon publishes **actual
command manuals per model** (e.g. SRP-330II, SRP-Q300) plus Windows/Linux SDKs, Web
Print SDK, Android UPOS. SRP-350V docs list USB, Ethernet, serial, dual serial,
parallel. **Policy until each family's manual is loaded and validated:** `vendorStatus =
AVAILABLE; DLE_EOT & GS_r = probe/document-per-model; GS_H = DO NOT ASSUME` —
"ESC/POS-compatible" does not prove Epson's newer `GS ( H` extension.

Mobile: **SPP-R200III** (2"/58 mm; user + command + Bluetooth manuals published;
serial/USB + BT Class 2 v3.0+EDR). **SPP-R310** (3"/80 mm class, 203 dpi, ≤72 mm width,
≤100 mm/s receipt, USB 2.0 FS + serial standard, BT or WLAN variants, NFC-assisted
pairing, paper-end, cover-open, optional black-mark/gap, 8 MB SDRAM/4 MB flash,
Android/iOS/Windows/Linux SDKs, **published `Command Manual_SPP-R310`** + Bluetooth
manual) — first-class, not generic Bluetooth ESC/POS.

## 11–12. Citizen — first-class

Families: CT-S310II, CT-E301/E351, CT-E601/E651, CT-S751, CT-S801III, CT-S851III,
CT-S4000, CT-S4500. SDKs: Windows, Linux, Android, iOS Swift/Obj-C, UWP.

- **CT-S801III/S851III**: 500 mm/s; top exit (801III) / front exit (851III); USB
  standard; Wi-Fi/serial/BT/Ethernet/parallel optional; LCD status UI.
- **CT-S4500** (wide): media 58–112 mm, **max print width 104 mm**, 203 dpi,
  ≤200 mm/s, USB 2.0 FS standard, optional BT incl. **Apple MFi**, optional
  serial/LAN/Wi-Fi; gap + black-mark + paper-end sensing; guillotine full/partial cut;
  ESC/POS. Do not derive raster width from roll width.
- **CMP-20II** (portable): 58 mm/48 mm, 203 dpi, 80 mm/s, RS-232 + USB, BT Class 2
  (4.2 config), MFi option, Wi-Fi option, **ESC/POS + CPCL + ZPL2**, paper-end,
  7.4 V/1800 mAh. Citizen publishes the actual **CMP20II ESC/POS CMD REF**.
- **CMP-30II**: media 25–80 mm, max width 72 mm, 203 dpi, 100 mm/s, RS-232 + USB, BT
  Class 2/4.2 variants, MFi, Wi-Fi, ESC/POS + CPCL + ZPL2, paper-end + reflective
  black mark, 7.4 V/2600 mAh, **IP42** rating. Published: CMP30II ESC/POS CMD REF,
  CMP30II CPCL CMD REF, CMP ZPL CMD REF — unusually good for protocol-level work.

## 13. Rongta — corrected

Families: RP58, RP80, RP326, RP331, RP3xx, RP8xx. **RP326** (current official page):
79.5±0.5 mm roll, 72/48 mm width, 576/384 dots, 203 dpi, 250 mm/s, USB/serial/Ethernet,
optional BT/Wi-Fi customs, 1.5 M-cut cutter, ESC/POS + OPOS. RP331 similar (72/48 mm).
**Correction: no currently manufacturer-hosted Rongta command reference proving
`GS ( H` fn 48 was found.** The defensible database entry: `ESC_POS = DOCUMENTED; OPOS
= DOCUMENTED; GS_H_FN48 = UNKNOWN / PROBE`.

## 14. Xprinter XP-S260M — probed beats marketing

Official: 79.5±0.5 mm media, ≤72 mm width, 576 dots, 203 dpi, ≤260 mm/s, USB + serial +
Ethernet, paper-end + cover-open + black-mark, partial cutter, **128 KB input buffer in
the current table but 64 KB on another page revision** (why firmware/hardware identity
belongs in the profile), 256 KB NV flash, ESC/POS, Windows/JPOS/Linux/Android/macOS,
iOS/Android/Windows SDK. **Public documentation does not prove `GS ( H`.** Database:
`vendor documentation: ESC/POS = yes, GS(H) = undocumented; actual hardware: GS(H) =
PROBED TRUE` — stronger than manufacturer marketing anyway.

## 15. Partner Tech RP-110

Current manufacturer page: 80 mm max media, 72±0.5 mm width, 203 dpi, direct thermal,
USB-B + RS-232 + Ethernet all standard; project-specific Bluetooth Smart Ready/BT 5 +
BLE; project-specific 802.11 a/b/g/n; paper-end, cover-open, black-mark-capable;
full/partial guillotine, 1.5 M cuts; ESC/POS-compatible. Not enough public programmer
documentation for the Epson completion extensions: `DLE EOT / GS r / GS(H) / ASB = probe`
— exactly what `pdctl probe` exists for.

## 16. Zebra portables — never the generic ESC/POS codepath

Families: ZQ310/320 Plus, ZQ511/521, ZQ610/620/630 Plus. Link-OS ecosystem; languages
**ZPL, CPCL, EPL** by model (ZQ630 Plus documents all three). ZQ630 Plus offers BT
Classic/BLE variants and newer configs with **Wi-Fi 6 + Bluetooth 5.3**. Link-OS SDK
has a separate **`StatusConnection`** — including `BluetoothStatusConnection` and
`BluetoothLeStatusConnection` — a status-only channel that doesn't block the print
channel. Worth implementing natively.

## 17. Brother mobiles — also not ESC/POS

Families: RJ-2030/2050/2140/2150, RJ-3230B/BL, RJ-4230B, RJ-4250WB. Languages: Brother
**Raster, ESC/P, P-touch Template**, and on models/configs **ZPL II, CPCL**. Brother
publishes Raster Command References and ESC/P references. RJ-4250WB: 4-inch class, USB,
WLAN, Bluetooth documented. `Brother != generic_escpos`.

## 18. Paper/media classes

| Class | Common paper | Typical print width |
|---|---|---|
| 2-inch | 58 mm | ~48–52 mm |
| 3-inch | 76–80 mm | commonly ~72 mm |
| Epson 58 mode | 57.5/58 mm | e.g. 420 dots on T20III |
| 3¼" legacy | ~82.5/83 mm | device-specific |
| 4-inch | 104–112 mm media | ~104 mm common max |
| Impact | e.g. TM-U220 | separate mechanics |

Store actual `mediaWidthMm, printWidthMm, printWidthDots, dpiX, dpiY` (T20III: 576 dots
on 80 mm but 420 in 58-mm configuration; CT-S4500: 112 mm media, 104 mm image).

## 19–23. Print servers — verified behaviour

- **Raw 9100** (CUPS: AppSocket/JetDirect): best case — if the socket truly passes
  responses both ways, the strong printer-level acknowledgement survives.
- **CUPS `waiteof=true`** waits for the printer to complete before returning — still
  not a correlated `GS(H)`. Keep distinct: `GS(H) verified = DEVICE_CONFIRMED`,
  `CUPS socket waiteof = BACKEND_CONFIRMED`.
- **Dumb USB/Ethernet servers**: never equate TCP completion with printer completion.
  Probe whether `DLE EOT`'s return byte traverses printer→USB→server→TCP→host, then a
  correlated fence; if responses don't survive: `completionAuthority =
  PRINT_SERVER_ONLY`.
- **IPP (RFC 8011)**: jobs are first-class, but a gateway that cannot determine
  downstream state may mark completed with `job-state-reasons = queued-in-device`.
  Never consume `job-state == completed` alone — always with `job-state-reasons`.
- **Windows**: `JOB_STATUS_COMPLETE` means sent, possibly not printed; without
  TrueEndOfJob a port monitor can mark `JOB_STATUS_PRINTED` immediately after
  submission. `Windows PRINTED != physically printed` absent a true end-of-job monitor.

## 24. Completion confidence hierarchy (formalised)

- **A+** — durable/queryable printer job: Epson ePOS JobID/result (best recovery
  semantics)
- **A** — explicit device completion: Epson `GS(H)` fn 48, Star CheckedBlock,
  documented vendor equivalent
- **B** — ordered status fence: `GS r`, documented vendor idle/completion (no
  correlated durable ID)
- **C** — current printer status: `DLE EOT`, ASB, Zebra status connection, vendor
  status APIs (diagnostics, not per-receipt proof)
- **D** — spooler/server status: CUPS completed, Windows Printed, IPP completed +
  queued-in-device
- **E** — transport only: TCP/USB/Bluetooth write succeeded

## 25. Bluetooth transport hierarchy

Never `bluetooth: true`. Store `BluetoothTransport { classicSPP, classicVendor, BLE,
MFi, vendorSDK }` — Epson P20II documents Classic + BLE; Star SM-S230i uses its SDK
path; Citizen CMP offers MFi variants; Zebra has separate Classic and BLE status
connections.

## 26. Initial catalogue

```
EPSON   T20/II/III/IV · T70/II · T82 · T88IV/V/VI/VII · m10 · m30/II/III ·
        m50/II · P20/P20II · P60/II · P80/P80II · U220/II
STAR    TSP100III/IV · TSP650II · mC-Print2/3 · SM-S210/220/230 · SM-L200/300 ·
        SM-T300/400
BIXOLON SRP-330 · 350 · 350plus · 380 · Q200/Q300 · F310 · 275 ·
        SPP-R200/R210/R310/R410
CITIZEN CT-S310 · E300 · E600 · S751 · S801/851 · S4000 · S4500 · CMP-20II · CMP-30II
RONGTA  RP58 · RP80 · RP3xx · RP8xx
XPRINTER generic 58 · generic 80 · S200/S260/S300 · V-series · portable
PARTNER RP-110 · RP-710
ZEBRA   ZQ300 Plus · ZQ500 · ZQ600 Plus
BROTHER RJ-2000 · RJ-3000 · RJ-4000
+ generic_unknown escape hatch
```

## 27. Capabilities over model names

The runtime must not depend on model names. `PrinterCapabilities { identity; media
{widthMm, printableWidthDots, dpi, blackMark, gap}; connection {tcp9100, usb, serial,
wifi, bluetoothClassic, bluetoothLE, ipp, vendorAPI}; completion {epsonProcessId,
starCheckedBlock, eposJobId, vendorJobId, queuedStatusFence}; status {realtimeStatus,
automaticStatus, paperEnd, paperNearEnd, cover, cutter, headTemperature, battery};
mechanism {cutter, fullCut, partialCut, tearBar}; quirks {feedBeforeCut, responseDelay,
oneJobOnly, fakeIdentity, unreliableStatus} }`. A model profile provides documented
defaults; a probe confirms what the installed hardware/firmware/interface path can do.

## 28. Probe provenance: DOCUMENTED / PROBED / UNVERIFIED

`pdctl probe` reports three distinct things per capability:

```
XP-S260M                                 Epson TM-T20III
────────────────────────────             ────────────────────────────
ESC/POS                                  GS(H) fn48
  manufacturer documentation  YES          Epson command documentation  YES
GS(H) fn48                                 current path probe           YES/NO
  manufacturer documentation  NO         Result:
  actual device probe         YES          capability       DOCUMENTED
DLE EOT / TCP RX backchannel               transport path   NEEDS PROBE
  actual device probe         YES
Result:
  completion grade  A
  evidence          DEVICE PROBE
```

This stops the classic ESC/POS mistake: assuming that recognising the print commands
implies the Epson feedback extensions.

## Bottom line

Three strong ecosystems for "did this receipt actually finish?": **Epson** (`GS ( H`
manufacturer-documented across many TM models; ePOS adds queryable JobIDs), **Star**
(CheckedBlock exists precisely to determine whole-data completion), **Zebra** (not
ESC/POS, but Link-OS gives independent status channels incl. BT/BLE). **Bixolon and
Citizen** publish genuine per-model command manuals and SDKs — integrate per model, not
as mystery clones. **Rongta, Partner and generic ESC/POS**: capability-probe before
promoting to strong completion. **Any print server changes the evidence boundary** —
TCP/CUPS/Windows success is never silently promoted to physical completion; IPP
standardises the `queued-in-device` caveat; Microsoft warns its complete/printed states
may precede printing. Architecture: **documentation-seeded, probe-confirmed,
transport-aware** — not hardcoded printer names.
