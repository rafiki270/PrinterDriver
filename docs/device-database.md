# Device Database, Transports & Print-Server Semantics

Research addendum (2026-08-08), part 2 — extends [capability-profiles.md](capability-profiles.md).
Target: not "Epson + a few clones" but **the major POS-printer ecosystems plus a
capability-driven generic layer**. The useful split: printer family/model · paper/media
geometry · physical interface · transport protocol · command language · feedback
mechanism · **whether feedback survives an intermediate print server** · quirks
discovered by probing.

## Expanded mainstream printer matrix

| Manufacturer / family | Models worth profiling | Paper | Common connections | Command/API | Best completion strategy |
|---|---|---:|---|---|---|
| **Epson budget** | TM-T20II/III/IV, T82III | 80 / often 58 guide | USB, Ethernet, serial per SKU | ESC/POS, some ePOS | **`GS ( H`**, `GS r`, DLE EOT, ASB |
| **Epson high-end** | TM-T88IV/V/VI/VII | 80 / 58 | USB, serial, Ethernet, Wi-Fi/BT per model | ESC/POS + ePOS newer | **`GS ( H`**; ePOS JobID where available |
| **Epson compact mPOS** | TM-m10, m30/II/III, m50/II | 80 / 58 variants | USB(-C), Ethernet, Wi-Fi, BT | ESC/POS, ePOS | `GS ( H` per model; ePOS preferred when suitable |
| **Epson front-exit** | TM-T70II/III | 80 | USB, serial, Ethernet | ESC/POS | Epson ordered-completion/status stack |
| **Epson kitchen/impact** | TM-U220 family | 76 mm impact | serial, USB, Ethernet | ESC/POS | status/queued completion; separate profile (impact mechanics differ) |
| **Star TSP100** | TSP143III/IV | 80 / 58 | USB, Ethernet; newer Wi-Fi + BT | StarPRNT | **Star checked-block API** |
| **Star TSP650** | TSP650II | 80 / 58 | serial, parallel, USB, Ethernet, BT | Star Line + ESC/POS | Star checked-block preferred |
| **Star mC-Print** | mC-Print2/3 | 58; 80/58 on mC-Print3 | USB, Ethernet, BT, opt. Wi-Fi | StarPRNT | **beginCheckedBlock/endCheckedBlock** |
| **Bixolon SRP-330** | SRP-330III/332III | 80, some 58 | USB + serial/parallel/Ethernet | ESC/POS-compatible vendor language | vendor status SDK first; probe `GS(H)` |
| **Bixolon SRP-350** | 350III/V, 352III/V | 80, 2"/3" media | USB, Ethernet, serial; newer wireless | ESC/POS-compatible | probe ordered fence + Bixolon status |
| **Bixolon SRP-350plus** | plusIII/plusV | 80 | USB/Ethernet + serial, WLAN, BT, Powered USB | ESC/POS-compatible | vendor SDK + raw probe |
| **Bixolon SRP-380** | 380/382/380plus | 80 | USB/Ethernet/wireless | ESC/POS-compatible | vendor SDK/status + raw probe |
| **Bixolon Q-series** | SRP-Q200, Q300/Q300II | 58 / 80 | USB, Ethernet, Wi-Fi, BT per model | Bixolon ESC/POS-compatible | model probe |
| **Citizen mainstream** | CT-S310II, E301/E351/E601/E651, S751 | 58/80, some 60/83 | USB, serial, Ethernet, Wi-Fi, BT, Powered USB | **ESC/POS emulation** | probe ordered ESC/POS mechanism |
| **Citizen fast retail** | CT-S801III, S851III | 80 | USB + options | ESC/POS | probe + Citizen status API |
| **Citizen wide** | **CT-S4500** | **58–112 mm** | USB; opt. Ethernet/serial/Wi-Fi/BT | ESC/POS | separate wide-media profile |
| **Citizen older wide** | CT-S4000 | 80 / 82.5 / 112 | USB, serial, parallel, Ethernet | ESC/POS | separate wide profile |
| **Rongta RP80** | RP80/802/803/807/820/850 | 80 / often 58 | USB, serial, Ethernet, Wi-Fi/BT per unit | ESC/POS | **probe `GS ( H` first** |
| **Rongta RP3xx** | RP325/326/327/330/331/332/335/336… | mostly 80 | USB, serial, Ethernet | ESC/POS/OPOS | `GS(H)` capability probe |
| **Rongta RP58** | RP58/E/A/B/581/582 | 58 | USB, serial, Ethernet per model | ESC/POS | conservative 58-mm profile |
| **Xprinter 80 mm** | S200/S260/S300, V320/V330, A260/A300… | 80 | USB, serial, Ethernet, Wi-Fi/BT varies | ESC/POS | `GS(H)` probe; **S260M confirmed** |
| **Xprinter 58 mm** | C58, 58IIK, T58, 581… | 58 | USB, serial, Ethernet/BT varies | ESC/POS | conservative until probed |
| **Sewoo/Aroot** | SLK-TS200 + related | 80 | USB, serial, Ethernet | ESC/POS-compatible | probe |
| **Partner Tech** | RP-110 | 80 | USB, serial, Ethernet + wireless variants | ESC/POS | Sewoo-derived profile + probe |
| **SNBC** | BTP-R880/R880NP + related | 80 | USB, serial, Ethernet | ESC/POS-style/vendor | probe/vendor SDK |
| **Custom** | TG2460, Kube, VKP kiosk family | mostly 80; ticket widths | USB, serial, Ethernet | ESC/POS-compatible/vendor | model-specific |
| **Generic OEM 80C/POS-80** | hundreds of no-name units | 80 | USB, serial, Ethernet | "ESC/POS" | start conservative; capability-promote |
| **Generic POS-58** | hundreds | 58 | USB, serial, BT, sometimes LAN | "ESC/POS" | ultra-conservative |

Reference points: Star officially supports 80/58 mm on TSP100IV and mC-Print3; TSP650II
exposes serial/parallel/USB/Ethernet/BT and both Star Line and ESC/POS. Bixolon's
current 80-mm range spans SRP-330III/350III/350V/350plusV/380/380plus/F310II + Q-series.
Citizen's range runs 58/80 up to the 112-mm CT-S4500, all ESC/POS emulation, with
CUPS/JavaPOS/OPOS support. Rongta ships one large driver family across RP58/RP80/RP3xx/
RP8xx (RP326: 203 dpi, 576 dots at 72 mm, USB/serial/Ethernet; RP820 adds Wi-Fi, CUPS,
JavaPOS, OPOS).

## Media is a capability, not a model assumption

```
MediaProfile: nominalRollWidth · printableWidthDots · dpi · maxRollDiameter
  · paperGuideInstalled · blackMarkSensor · gapSensor · nearEndSensor
  · cutter · fullCut · partialCut
```

| Nominal paper | Typical usable width | Typical 203-dpi raster |
|---|---:|---:|
| 58 mm | ~48–52 mm | ~384–420 dots |
| 76 mm | varies (impact) | model-specific |
| 80 mm | ~72 mm | **576 dots** |
| 82.5/83 mm | ~72–80 mm | model-specific |
| 112 mm / 4" | ~104 mm | ~832 dots |

**Never infer raster width from roll width.** CT-S4500 accepts 58–112 mm media but
prints max 104 mm; Star TSP650II: 80 mm paper → 72 mm printable, 58 mm → 52 mm. Receipt
rendering needs `paperWidth=80mm` AND `printWidth=576dots` as separate facts.

## Interface ≠ transport ≠ language

One Ethernet printer may simultaneously expose TCP 9100, IPP 631, LPR 515, HTTP/ePOS,
SNMP 161, and web config. Model separately:

```
physicalInterface: usb · ethernet · wifi · serial · bluetooth · parallel
transport:  raw_tcp · serial_stream · usb_bulk · ipp · ipps · lpr
            · vendor_http · vendor_sdk · bluetooth_spp · bluetooth_ble
language:   escpos · starprnt · star_line · epson_epos_xml · raster
```

### Transport notes

1. **Raw TCP 9100** (CUPS calls it AppSocket/JetDirect) — preferred generic route:
   minimal moving parts, full byte control, potential full-duplex, completion markers
   return, no driver interference. Recommended: exclusive connection, keep open, one job
   in flight, continuous RX parser.
2. **Serial RS-232** — genuinely bidirectional, no print-server firmware in the way,
   flow control possible. Store baud/dataBits/parity/stopBits/hardwareFlowControl; never
   assume baud (9600/19200/38400/115200 all common).
3. **USB** — three cases: OS printer-class (reverse status harder), direct
   libusb bulk OUT/IN (much better with a bidirectional endpoint), vendor SDK. Feedback
   preference: **vendor SDK > direct USB > OS spooler**.
4. **Bluetooth** — SPP/classic ≈ RS-232 byte stream (good); BLE is vendor-specific — do
   not tunnel arbitrary ESC/POS unless the vendor supports it (Star provides its own SDK
   abstraction).
5. **Wi-Fi** — changes nothing by itself: `ip:9100` = wired semantics; IPP / vendor API /
   CloudPRNT = that transport's semantics.

## Print servers: six cases, six guarantees

- **A — dumb Ethernet→USB print server** (TP-Link/D-Link/router): the socket terminates
  at the *server*; send/ACK/close only proves "server received data" — it may buffer,
  lose, fail to open the printer, hit paper-out, or drop return status. Rule:
  `feedbackCapability = TRANSPORT_ONLY` unless printer→host responses are proven.
- **B — transparent USB-over-IP tunnel**: usable IF probed. Acceptance: `DLE EOT` →
  exact response AND receipt + `GS(H)` → exact marker, both through the box →
  `bidirectionalTransparent = true`, else no.
- **C — CUPS**: two job boundaries (app job → CUPS job → printer buffer → paper). CUPS
  submission success ≠ physical print. Routes: AppSocket 9100 / IPP 631 / LPD 515 (IPP
  recommended over LPD). AppSocket's `waiteof=true` waits for job completion — but do
  NOT equate it with our `GS(H)` token; EOF semantics vary by adapter. Better: **CUPS →
  custom pd backend → printer**, so CUPS completion is tied to our semantic completion
  (preflight → send → wait GS(H) → cut → verify cutter → return).
- **D — IPP**: explicit job states (pending/processing/stopped/canceled/aborted/
  completed) + queryable attributes. Native-printer IPP is potentially strong; CUPS-IPP
  proxy → 9100 means `completed` reflects only what the backend knew. Hence record
  `completionAuthority: physical_printer | vendor_spooler | pd_agent | print_server |
  transport_only`.
- **E — Windows spooler**: "Printed" ≠ physically completed unless the port
  monitor/vendor driver gets true end-of-job. Distinct enums: `SERVER_COMPLETED` vs
  `DEVICE_CONFIRMED`.
- **F — Raspberry Pi as OUR print server (recommended)**: POS → HTTP/WebSocket/TCP →
  pd-agent on Pi → raw TCP/serial/USB/vendor SDK → printer. A *semantic* print server
  returning `{job, state, evidence:{transportAccepted, printFence:"GS(H)",
  fenceResponse, cutterStatus, paperStatus}}`.

## Vendor stacks that are first-class, not ESC/POS-emulated

- **Star**: use StarIO/StarXpand/StarPRNT `beginCheckedBlock`/`endCheckedBlock` (Star
  documents it as determining whether the whole data completely printed) rather than
  Star's ESC/POS emulation. Star's SDK also exposes model name, MAC, USB serial and
  firmware — far cleaner identification than clone fingerprinting.
- **Epson**: two paths — ESC/POS direct (`GS(H)`; simple, great for LAN TM-T20/T88) and
  ePOS (JobID/spooler result; better for TM-i/server-side jobs). Support both.
- **Bixolon**: first-class family (`bixolon_srp330/350/350plus/380/f310/q200/q300`):
  vendor SDK status + raw ESC/POS probe; never assume `GS(H)` from "ESC/POS-compatible".
- **Citizen**: first-class (`citizen_cts_58_80`, `citizen_cts_fast`,
  `citizen_cts_4inch`), split further by model where probe behaviour differs.

## Probe expansion

`pdctl probe` grows into a full Printer Discovery Report: Identity (vendor guess,
reported manufacturer/model, firmware, MAC OUI, confidence %), Media (nominal paper,
printable width, raster dots, dpi, 58-mm guide, near-end/paper-end/cover sensors),
Interfaces (9100/631/515/HTTP/SNMP open), ESC/POS capability list (DLE EOT 1–4, GS r,
ASB, **GS(H) fn48**, GS I, DLE ENQ, maintenance counters), Completion (method, ordered
fence, correlated ID, one-job-in-flight recommendation), Cutter (full/partial/error
status), **Recommended transports ranked** (e.g. direct 9100 ★★★★★ · ePOS ★★★★☆ · CUPS
custom backend ★★★★☆ · generic CUPS socket ★★☆☆☆ · LPD ★☆☆☆☆), and the selected profile.

Plus **path probing** for intermediate servers:

```
pdctl probe-path --server 192.168.1.20:9100 --printer usb:04b8:0e27
Host → print server   YES
Server → printer      YES
Printer → server      YES
Server → host status  NO
──────────────────────────
End-to-end status     NO
```

## Confidence grades for every route

- **A** — physical/job-level confirmation: Star CheckedBlock, Epson `GS(H)`, ePOS JobID
  result
- **B** — ordered device response, weaker semantics: `GS r`, vendor idle
- **C** — device status around transmission: DLE EOT, ASB, SNMP
- **D** — spooler says completed: CUPS, Windows spooler, generic IPP gateway
- **E** — transport only: TCP/USB write succeeded, print server accepted

Every result carries `{confidence: "A", authority: "physical-printer", method: "GS(H)"}`
— never a bare `{success: true}`.

## Device database layout

```
epson/    tm_t20 tm_t70 tm_t82 tm_t88 tm_m tm_u220 tm_i
star/     tsp100 tsp650 mcprint2 mcprint3 portable
bixolon/  srp330 srp350 srp350plus srp380 srpf310 q200 q300
citizen/  cts310 cte300 cte600 cts751 cts801 cts851 cts4000 cts4500
rongta/   rp58 rp80 rp3xx rp8xx
xprinter/ pos58 pos80 s_series v_series a_series
sewoo/    slk_ts
partner/  rp110
snbc/  custom/  generic/
```

**The profile doesn't dictate the capability — it provides defaults; the probe
overrides them.** An unknown £35 no-name printer can be promoted to
`generic_escpos + gsHCompletion + asb + cutterStatus + 576Dots + earlyCutFeed=0`, while
a "identical" unit on other firmware lands at
`generic_escpos + gsRFence + noASB + 576Dots + earlyCutFeed=6`. That is what makes this
robust for the "whatever thermal printer happens to be installed at the venue" reality —
not an endless hard-coded compatibility list.

> **2026-08-09:** extended and corrected by [compatibility-brief.md](compatibility-brief.md) — adds Zebra/Brother (non-ESC/POS), portables, the A+ grade, Bluetooth transport hierarchy, and capability provenance.
