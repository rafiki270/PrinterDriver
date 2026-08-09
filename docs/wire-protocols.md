# Wire Protocols: ePOS Spooler, Star Raw, CloudPRNT, BLE/MFi

Deep-research results (2026-08-09), implementation-grade. Verdicts:

| Target | Result | Grade |
|---|---|---|
| Epson ePOS spooler | wire contract + model matrix resolved | **A+ implementable** |
| Star raw checked printing | ETB resolved; safer Ethernet fence documented | **A implementable** |
| Star CloudPRNT | complete polling/job/confirmation contract | **A implementable** |
| Rongta RP80/RP326 | commands in vendor-authored, mirror-hosted manuals | Documented–provisional |
| Rongta RP331 | no command reference located | Unverified |
| Xprinter S-series / ESC x | no authoritative reference | Unverified |
| BLE generic UART | probe profiles, no universal printer profile | Heuristic transport |
| Epson/Star/Bixolon MFi strings | resolved | Implementable |
| Citizen MFi string | vendor-gated | Blocked pending approval |

## 1. Epson ePOS-Print spooler

`POST /cgi-bin/epos/service.cgi HTTP/1.1` with `Content-Type: text/xml; charset=utf-8`,
`If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT`, `SOAPAction: ""`. WSDL form puts
devid/timeout/printjobid in the SOAP header; older form: `?devid=local_printer&timeout=60000`.
timeout default 60,000 ms, cap 300,000 ms (ePOS-Print XML User's Manual rev. AC).

Spooled print request:
```xml
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">
 <s:Header><parameter xmlns="http://www.epson-pos.com/schemas/2011/03/epos-print">
  <devid>local_printer</devid><timeout>60000</timeout><printjobid>ABC123</printjobid>
 </parameter></s:Header>
 <s:Body><epos-print xmlns="http://www.epson-pos.com/schemas/2011/03/epos-print">
  <text>Hello, World!&#10;</text><cut/></epos-print></s:Body></s:Envelope>
```
Immediate ack: `<response success="true" code="" status="2" battery="0"/>` — **an
enqueue acknowledgement, not printing**: initial status is 0x00000002 or 0x80000000
even though printing has not happened. Poll = same envelope, header only
`<printjobid>`, body `<epos-print/>` (empty). JobIDs need ePOS-Print Service **4.1+**;
1–30 chars of alphanumerics + `_ - .`; service assigns if omitted.

Parse rules: match by XML namespace/local-name (never the `s:` prefix); status is
unsigned 32-bit decimal; `Printing`/`JobSpooling` non-terminal; first success=true on a
spooler printer = accepted-not-printed; keep 0x80000000 unsigned in JS.

Response codes: EPTR_AUTOMATICAL (auto-recovery), EPTR_BATTERY_LOW, EPTR_COVER_OPEN,
EPTR_CUTTER, EPTR_MECHANICAL, EPTR_REC_EMPTY (roll end), EPTR_UNRECOVERABLE,
SchemaError, DeviceNotFound, PrintSystemError, EX_BADPORT, EX_TIMEOUT, EX_SPOOLER
(queue full), JobNotFound, Printing, JobSpooling, TooManyRequests,
RequestEntityTooLarge, ERROR_WAIT_EJECT.

Status mask: 0x1 no response · 0x2 **printing completed** · 0x4 drawer pin 3
high/battery-offline · 0x8 offline · 0x20 cover open · 0x40 feed-switch feeding ·
0x100 waiting online recovery · 0x200 feed/panel held · 0x400 mechanical err · 0x800
cutter err · 0x2000 unrecoverable · 0x4000 auto-recovery err · 0x10000 wait slip
insertion · 0x20000 roll near-end · 0x40000 wait slip ejection · 0x80000 paper end ·
0x1000000 buzzer/label-or-paper-removal (model-dep.) · 0x4000000 no paper at peeler ·
0x80000000 spooler stopped.

**Spooling is a device setting** (TMNet WebConfig / EpsonNet Config / setup utility),
NOT a request attribute; `force="true"` = offline forced transmission, unrelated.
Model matrix: TM-i (fw 4.1+ spooler) · older TM-DT (sw 3.0+) · TM-DT2 yes ·
TM-T88VI-iHUB yes · **plain network TM-T88VI yes** · **plain TM-m10/m30/m30II NO** ·
**plain TM-T88VII NO** · most current ordinary models often-XML/usually-no-spooler.
"OmniLink" is NOT a capability proxy — select by exact model+firmware.

Auth: the print service documents NO authentication; implement unauthenticated. Never
auto-send WebConfig credentials (legacy Digest epson/epson; newer epson + serial) —
those are admin, not print-service, credentials.

## 2. Star without the SDK

**ETB fence**: byte `0x17`. On consume: waits for all preceding printing, increments a
five-bit counter, sets ETB status, emits ASB if enabled (Line Mode Command Specs).
ASB: `1B 1E 61 00` disable · `1B 1E 61 01` enable · `1B 06 01` request now ·
`1B 1E 45 00` clear ETB counter/status. Counter lives in "printer status 6" = ASB byte
offset 7, bits packed non-contiguously (ASB bit6→ctr4, 5→3, 3→2, 2→1, 1→0):
```c
uint8_t etbCounter(uint8_t b){ return ((b & 0x60) >> 2) | ((b & 0x0e) >> 1); }
```
Wraps 31→0. **TCP 9100 caveat: ASB is broadcast to every connected 9100 host** —
multiple clients can misattribute completion. Star's preferred Ethernet fence is
**ESC GS ETX**: request `1B 1D 03 01 n1 n2` → response `1B 1D 03 01 n1 n2 counter 00`;
waits for prior printing/motor activity, eight-bit print-end counter, **replies only to
the issuing session**; n1/n2 are echoed correlation bytes; keep the socket open.
Recommendation: serial/USB/exclusive-TCP → ETB+ASB; shared Ethernet → ESC GS ETX; ETB
on Ethernet only with a single enforced 9100 session. StarPRNT SDK checked blocks use
ETB counting on Line Mode printers, but don't assume the same primitive per
emulation/interface.

**CloudPRNT** (printer polls a server — embeddable in pd-agent):
poll POST JSON: `{status:"<ASB hex>", statusCode:"200%20OK", printerMAC, uniqueID,
jobToken, printingInProgress}` → server: `{jobReady, mediaTypes[], jobToken,
deleteMethod:"DELETE", jobGetUrl?, jobConfirmationUrl?}` → printer
`GET ?uid&type&mac&token` (**must be idempotent** — re-download after interruption;
results 200/401 optional Basic/404/415) → confirm
`DELETE ?uid&mac&code=<status>&token` (200 OK success; retried, `retry=x`).
Status codes: 200 OK/success · 201 output taken · 211 paper low · 220 printing ·
221 output present · 230 cleaning · 231 maintenance · 410 paper out · 411 jam ·
412 roll position · 420 cover open · 510 incompatible media · 511 decode failure ·
512 unsupported media version · 520 job timeout · 521 job too large · 1000/1001
server-settings JSON errors. Server rules: retain job until confirmed, key by printer
identity + token, never consume on GET alone.

## 3. Rongta and Xprinter provenance

**Rongta**: official 2026 manual index has RP80/RP32X/RP33X *user* manuals (no
commands). Vendor-authored command sets found only on third-party mirrors: RP80
Command Set (88 pp) and RP325/326/327/328 Command Set V1.0 (90 pp) — both define
`GS ( H` fn 48 (`1D 28 48 06 00 30 30 d1..d4`, four printable process-ID bytes, saved
for immediately-preceding data). **Caveats**: obvious Epson-template residue
(manufacturer "EPOSN", printer "TM-T88V") and **no response frame or timing
specified** → presence = Documented–provisional; checked-block transport unproven from
manuals alone; RP331 unverified. `DLE ENQ` is better specified: `10 05 01` recover +
resume from interrupted line · `10 05 02` clear receive+print buffers then recover;
restricted to recoverable cutter/black-mark/platen-open; realtime even offline or with
full receive buffer on serial.

**Xprinter**: no official S-series programmer reference; no ESC x contract anywhere. A
legacy vendor-authored XP-58XX mirror manual defines `ESC v` (`1B 76`): serial 00 =
paper present / 04 = paper out; Ethernet: auto-returned four-byte ASB-style status
(feed, recoverable/unrecoverable error, paper). No ESC x in it. Database: ESC v
(XP-58XX only) = DocumentedLegacyMirror; S-series applicability = Unverified; ESC x =
Unverified; **do not infer ESC x from ESC v**.

## 4. Bluetooth wire details

BLE (CoreBluetooth/GATT, no EA protocol string) and MFi Classic (EAAccessory/EASession,
string in UISupportedExternalAccessoryProtocols) are separate transports.

| Vendor | Public raw GATT | MFi string |
|---|---|---|
| Epson | No — TM-P20II BLE requires ePOS SDK's dedicated profile | `com.epson.escpos` |
| Star | No — SDK `BLE:<device>` names, GATT hidden | `jp.star-m.starpro` |
| Bixolon | No public SPP-R310 GATT map (dual-mode BLE + MFi iAP2) | `com.bixolon.protocol` |
| Citizen | No | vendor-gated: issued only via MFi registration/approval |

Generic BLE-UART probe profiles (detection candidates, not a standard):
- **FFE family**: service `0000FFE0-0000-1000-8000-00805F9B34FB`; TX often FFE1
  (sometimes FFE2); RX often FFE1 Notify or FFE2.
- **Nordic NUS**: service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`; host→printer
  `6E400002-…` Write/WNR; printer→host `6E400003-…` Notify.
- **Microchip Transparent UART**: service `49535343-FE7D-4AE5-8FA9-9FAFD205E455`;
  host→printer `49535343-8841-43F4-A8D4-ECBE34729BB3`; printer→host
  `49535343-1E4D-4BD9-BA61-23C647249616`.

GATT discovery algorithm: (1) prefer explicitly configured service/characteristic
pair; (2) probe Microchip TUS → Nordic NUS → FFE0 family; (3) validate **properties**,
not UUIDs alone (outbound: Write or WriteWithoutResponse; inbound: Notify or Indicate);
(4) chunk by CoreBluetooth `maximumWriteValueLength`, never a fixed 20 bytes; (5)
honour `canSendWriteWithoutResponse` backpressure; (6) keep status/checked-block
disabled unless notification framing is documented or successfully probed. Epson/Star/
Bixolon BLE stay `sdkRequired`/`profileUnknown` — never silently mapped onto FFE1;
Citizen Classic BT blocked until its protocol name + MFi approval.
