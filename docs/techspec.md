# Tech Spec: Print-Completion Feedback for ESC/POS Thermal Printers

Companion to [docs/brief.md](brief.md). This document contains the full technical detail:
protocol commands, byte sequences, architecture, job state machine, CUPS integration,
duplicate-handling policy, and alternative vendor APIs. See
[docs/testing-plan.md](testing-plan.md) for the capability probe script and fault-injection
test matrix.

Target hardware: **Xprinter XP-S260M** (USB, serial, LAN interfaces; partial cutter;
128 KB input buffer; ESC/POS compatible; Windows/JPOS/OPOS/Linux/Android/macOS drivers;
vendor SDK available).

---

## 1. Diagnosis: why current signals are weak

- A successful TCP write does not mean the printer consumed the data.
- A TCP acknowledgement does not mean the print mechanism ran.
- CUPS defines backend success as the file being successfully transmitted to the device or
  remote server — not that it printed.
- Windows explicitly documents that `JOB_STATUS_COMPLETE` may be set while the job is not
  yet printed, and that port monitors without TrueEndOfJob support can mark a job printed
  immediately after submission.
- The XP-S260M has a **128 KB input buffer** — an entire batch of receipts could be
  accepted into that buffer long before the print mechanism reaches the end of any of
  them. This is another reason a completed `send()`/`sendall()` call is weak evidence.

## 2. Evidence ladder

| Level | Evidence | What you can honestly claim |
|---|---|---|
| Transport accepted | `send()`, TCP ACK, USB write | Bytes reached a buffer somewhere |
| Printer currently healthy | `DLE EOT`, ASB | Online, paper present, cover closed, no current cutter error |
| Ordered receipt completion | `GS r 1` or `GS ( H` response | Preceding print data was processed by the print engine |
| Cut command processed | Ordered marker after cut | Firmware processed the cut command |
| Cut reported fault-free | Ordered marker + cutter status | No cutter error was reported after processing |
| Paper physically emerged | Exit sensor | Paper moved through the exit |
| Receipt contains readable content | QR/optical verification | Physical paper exists with identifiable printed content |
| Receipt was taken | Presenter/taken sensor | Someone removed the receipt |

---

## 3. ESC/POS command reference used in this design

| Command | Hex | Type | Purpose |
|---|---|---|---|
| Initialize | `1B 40` (`ESC @`) | — | Reset printer state |
| `DLE EOT 1` — printer status | `10 04 01` | **Real-time** | Online/offline, cover, paper, error summary |
| `DLE EOT 2` — offline cause | `10 04 02` | **Real-time** | Why the printer is offline |
| `DLE EOT 3` — error cause | `10 04 03` | **Real-time** | Error detail; bit 3 = autocutter error (Epson-compatible implementations) |
| `DLE EOT 4` — paper status | `10 04 04` | **Real-time** | Paper sensor state |
| `GS r 1` — paper sensor status | `1D 72 01` | **Queued** | Response sent only after preceding print data completes; documented by Epson as usable to recognize print completion |
| `GS ( H` Function 48 — process ID | `1D 28 48 06 00 30 30 d1 d2 d3 d4` | **Queued** | Attaches a 4-byte printable-ASCII ID to preceding print data; printer echoes it back once that data has printed |
| ASB enable | `1D 61 0E` | Config | Enables automatic status back (online/offline, error, paper) pushed on state change |

**Critical distinction:** `DLE EOT *` commands are *real-time* — the printer may answer
them immediately, out of order with respect to buffered print data. They tell you current
device state, not "the preceding job is done." `GS r 1` and `GS ( H` are *queued* — their
responses are only sent after the printer has actually processed everything ahead of them
in the command stream. This is the mechanism that makes them usable as completion fences.

### 3.1 `GS ( H` Function 48 — process ID (preferred mechanism)

Epson documents a process-ID command that associates four printable ASCII bytes
(`0x20`–`0x7E`) with the data immediately preceding it in the stream.

```
Command:  GS ( H  pL pH fn m d1 d2 d3 d4
Hex:      1D 28 48 06 00 30 30 d1 d2 d3 d4
```

Example, token `P123`:

```
TX: 1D 28 48 06 00 30 30 50 31 32 33
Expected RX: 37 22 50 31 32 33 00
```

When the marker relates to printing data, Epson states its response is transmitted **when
printing is completed**. The response contains the same process ID, giving a per-job
correlation token — not just an anonymous status byte.

**Why it's powerful:** because the command stream is FIFO, when the marker's response
comes back, every print operation queued ahead of it (i.e. the whole receipt) has
necessarily completed. Sequence:

```
[receipt content]
[final line feed / print-and-feed operation]
[GS ( H marker P123]
```

When `P123` comes back: the final printing operation completed, and — because of FIFO
ordering — all preceding receipt content necessarily completed too. The response
identifies the specific marker, so it's usable as a correlation token, not just a status
bit.

**Important limitation:** Epson warns that when several process-ID responses are pending
and cannot all be transmitted, the printer may return only the newest one. Therefore:

- keep a continuous reader running;
- use only one receipt in flight; **or**
- wait for each completion marker before sending the next one.
- Do not scatter process markers after every line — one marker at the end of each receipt
  is sufficient.

**Using it around the cutter:**

```
[receipt + final feed]
[marker P123]
wait for P123
[cut command]
[marker C123]
wait for C123
[DLE EOT 3]
check cutter-error bit
```

`P123` is the strong print-completion acknowledgement. `C123` proves the command
immediately preceding it — the cut command — was processed in sequence. It does **not**
independently prove the blade physically travelled successfully; that's why it must be
followed by a real-time cutter-status check (`DLE EOT 3`). Because you waited for the
ordered `C123` marker first, the subsequent real-time status request cannot accidentally
overtake the cut operation.

### 3.2 `GS r 1` — fallback completion signal

```
Hex: 1D 72 01
```

Requests paper status — but it's useful because it's a *normal queued command*, not a
real-time one. Epson specifically states that after the print-changing-line operation
ends, the paper status is transmitted, and that placing `GS r 1` after a printing
instruction lets the host recognize print completion from the returned status. Epson also
says the host should not send further data until the response arrives.

```
[receipt]
[final LF or print-and-feed]
1D 72 01
```

When the one-byte response arrives, the final line-print operation has completed.

**Limitations vs. `GS ( H`:**

- No job ID.
- The response is only one byte.
- Only one in-flight receipt is safe.
- Some printers stop processing normal commands when offline or out of paper, so the
  response may never arrive.
- Confirms printing, not necessarily successful cutting.

**Cutter fallback using `GS r 1` as an ordered fence:**

```
[receipt + final feed]
[GS r 1]
wait for response
[cut]
[GS r 1]
wait for response  # ordered fence, not a documented cutter guarantee
[DLE EOT 3]
check cutter error
```

The first `GS r 1` has documented print-completion semantics. Using a second one as a
post-cut ordered fence should be treated as **printer-specific behavior requiring fault
testing**, not a universal guarantee of physical cutter completion.

### 3.3 `DLE EOT` — useful, but not a completion acknowledgement

`DLE EOT` is explicitly a *real-time* command — it may be processed immediately, ahead of
buffered receipt data. This sequence is therefore **unsafe**:

```
[large receipt]
[cut]
[DLE EOT 3]        # UNSAFE as a completion check
```

A response could arrive while the receipt is still buffered, or before the cut command has
executed. It tells you the printer's *current* state, not that the preceding job finished.
Epson also instructs hosts to wait for the corresponding status before sending subsequent
data.

**Use `DLE EOT` for:**
- preflight checks;
- diagnosing a missing completion response;
- checking cover/paper/offline state;
- checking cutter status **after** an ordered completion fence (`GS ( H` or `GS r 1`
  response received first).

**Do not use it as the fence itself.**

### 3.4 Automatic Status Back (ASB)

```
Enable: 1D 61 0E
```

On Epson's bit assignments, `0x0E` enables online/offline, error, and paper-status
notifications. Once enabled, the printer transmits its current status and then sends
another status whenever an enabled condition changes. Detects: cover opened, paper
exhausted, printer going offline, recoverable/unrecoverable errors, cutter errors.

ASB is excellent for explaining *why* a completion response never arrived — it's a
device-state stream, not a per-receipt acknowledgement. The connection reader therefore
needs to parse an **interleaved incoming stream** containing all of:

- single-byte real-time statuses (`DLE EOT` responses);
- single-byte `GS r` responses;
- ASB status frames;
- `GS ( H` frames (start with `37 22`, end with `NUL`).

### 3.5 Xprinter-specific: "one ticket one control"

Xprinter has published an (older, translated) article describing a scheme for kitchen
printing:

- in standard mode, a ticket's printing process runs until the cutter instruction
  executes;
- software first sends a command written as `1B 76` (`ESC v`) to query printer state;
- it then sends `1B 78` (`ESC x`) to query whether the printer is working or idle;
- another ticket is sent only once the printer is idle.

Interpreted as:

```
1B 76   ESC v — status query
1B 78   ESC x — task/idle query
```

**This is potentially very relevant** — it appears to be Xprinter's intended kitchen-ticket
control mechanism, and may be better-supported on this hardware than Epson's `GS ( H`
extension. However, the public article does **not** provide: exact response-byte
definitions, a supported-model list, firmware-version requirements, whether it works over
LAN, whether "idle" occurs after the cutter physically completes, or whether the commands
survive current ESC/POS emulation modes. **Do not build production logic from the article
alone.**

**Exact support request to send Xprinter:**

> For XP-S260M, please provide the "one ticket one control" command specification,
> including the request and response formats for `0x1B 0x76` and `0x1B 0x78`, supported
> firmware versions, and confirmation that responses are returned over TCP port 9100.
> Please also confirm support for ESC/POS `GS ( H` Function 48 process-ID responses and
> `GS r 1` print-completion responses.

---

## 4. Which interface to use

### LAN — first choice, but only if the backchannel works

TCP port 9100 is full-duplex at the network level, but the printer's internal LAN adapter
may or may not forward ESC/POS response bytes back on the same socket. **Test it — do not
infer support from the fact that printing works.** Keep the connection open after sending;
closing immediately after `sendall()` can prevent receiving status responses.

### RS-232 — a strong fallback

The XP-S260M includes serial connectivity. A Raspberry Pi with a USB-to-RS-232 adapter can
keep a stable full-duplex connection, use hardware flow control where supported, receive
status directly from the printer controller, and avoid any limitations of the Ethernet
print server. **Do not print over LAN and poll over serial** — the two interfaces may have
separate input buffers, and a status response on one interface does not necessarily fence
data sent through the other.

### Direct USB

May expose a bidirectional endpoint, particularly through the vendor SDK or OPOS/JPOS
service object. Linux `/dev/usb/lp*` behavior varies, so a direct SDK or `libusb` path is
usually preferable when reliable backchannel reads matter.

### OPOS/JPOS — possibly the easiest vendor-supported route

The XP-S260M advertises OPOS and JPOS support. In the UnifiedPOS model, asynchronous
output gets an `OutputID`, and an `OutputCompleteEvent` containing that ID is queued when
the service object says the printer completed processing the request:

```
AsyncMode = true
PrintNormal(...)
capture OutputID
wait for OutputCompleteEvent(OutputID)
```

This is promising, particularly on Windows — but the strength depends entirely on
Xprinter's service object implementation. It must be tested to establish whether the event
means: data copied into driver memory / data sent to the device / device buffer drained /
mechanism actually completed. Run the same paper-out, cutter-jam, network-loss, and
power-loss tests against the event (see [testing-plan.md](testing-plan.md)) — a vendor API
name is not evidence by itself.

---

## 5. Recommended architecture

Do not let every till or application connect directly to each printer. Use a small local
**printer agent**, ideally on the Raspberry Pi/Linux device already near the printers:

```
POS applications
      │
      │ job UUID + receipt bytes
      ▼
Local printer agent
  ├─ persistent job database
  ├─ one queue per printer
  ├─ one active job per printer
  ├─ full-duplex status reader
  └─ capability profile per model/firmware
      │
      ▼
TCP 9100, RS-232, or direct USB
      │
      ▼
Thermal printer
```

**The agent must own the connection exclusively.** No other process should write to that
printer connection — not another till, not CUPS in parallel, not a test utility, not
another raw port-9100 client. Otherwise completion responses cannot be reliably associated
with particular receipts.

### 5.1 Persistent job states

Do not model this as a boolean `printed`. Use a state machine:

```
QUEUED
PREFLIGHT_OK
SEND_STARTED
BYTES_SENT
PRINT_CONFIRMED
CUT_COMMAND_PROCESSED
DONE_SOFTWARE
PHYSICALLY_VERIFIED
FAILED_KNOWN
UNKNOWN
```

- Persist `SEND_STARTED` **before** transmitting the first byte.
- After a process crash, any job that reached `SEND_STARTED` but has no completion
  acknowledgement must become `UNKNOWN` — not automatically retried.

### 5.2 Suggested strong sequence (when `GS ( H` is supported)

1. Allocate durable receipt UUID.
2. Map it to a temporary four-character marker, e.g. `P7K2`.
3. Check `DLE EOT 1`, `2`, `3`, and `4`.
4. Refuse to start if cover open, no paper, or hardware error.
5. Send receipt data.
6. Ensure it ends with a genuine print-and-feed operation.
7. Send `GS ( H` marker `P7K2`.
8. Wait for `37 22 50 37 4B 32 00`.
9. Mark `PRINT_CONFIRMED`.
10. Send cut command.
11. Send a second process marker, e.g. `C7K2`.
12. Wait for its response.
13. Send `DLE EOT 3` and verify the cutter-error bit is clear.
14. Mark `DONE_SOFTWARE`.

The four-byte marker is a short-lived correlation token only — map it to a proper UUID in
the job database, and avoid reusing a token while it might still be active.

### 5.3 Fallback sequence (when `GS ( H` is unsupported)

1. Enable ASB continuously.
2. Preflight with `DLE EOT`.
3. Send one receipt.
4. Append final LF/feed and `GS r 1`.
5. Wait for its response.
6. Mark `PRINT_CONFIRMED`.
7. Cut.
8. Use Xprinter `ESC x` idle polling if documented and supported (see §3.5).
9. Check `DLE EOT 3` for cutter errors.

Without either a process marker or a documented vendor idle/cut response, describe the cut
step honestly as: *"Print completion confirmed; cut command sent; no cutter error
subsequently reported."* That is still much stronger than "socket write succeeded," but it
is not physical cut confirmation.

---

## 6. Keeping CUPS while improving its semantics

Stock CUPS is insufficient: `CUPS_BACKEND_OK` only means the file was transmitted. Options:

Retain CUPS for application compatibility, but replace the normal `socket://` backend with
a custom backend or local agent that:

1. receives the complete raw job;
2. performs preflight;
3. sends the receipt;
4. appends the completion fence;
5. reads the printer's response;
6. exits successfully only after confirmation.

### Retry policy

| Situation | Action |
|---|---|
| Preflight failed before any receipt bytes | Safe to retry or hold |
| Known paper/cover error before sending | Hold |
| Completion confirmed | Success |
| Some bytes sent but acknowledgement lost | Hold as `UNKNOWN` |

**Never** return a normal retry code for an ambiguous kitchen ticket — CUPS may print a
duplicate. The backend should expose the ambiguous job to an operator rather than letting
the scheduler silently retry it.

---

## 7. Exactly-once printing is still impossible with an ephemeral raw acknowledgement

There is an unavoidable failure case:

1. The printer prints and cuts the receipt.
2. It transmits the completion response.
3. The network connection, Raspberry Pi, or process dies before the application records
   the response.

After restart, the application cannot distinguish:

- **A.** Receipt printed, acknowledgement was lost.
- **B.** Receipt did not print.

A current "idle" status does not resolve this — both cases leave the printer idle.
Therefore a raw ESC/POS system needs an explicit `UNKNOWN` state. It cannot honestly
promise exactly-once printing across arbitrary power/network failures **unless the printer
itself keeps a durable, queryable history of job IDs** (which cheap ESC/POS printers do
not).

### 7.1 Practical duplicate handling

Every kitchen ticket should contain a visible stable identifier:

```
ORDER: 7F3A-92C1
PRINT ATTEMPT: 1
```

A QR code containing the same ID is even better. When an ambiguous ticket is deliberately
reprinted:

```
*** REPRINT / POSSIBLE DUPLICATE ***
ORDER: 7F3A-92C1
PRINT ATTEMPT: 2
```

**At the API level:**
- retries using the same idempotency key should return the existing job, not create
  another print;
- an ambiguous reprint should require an explicit `forceReprint` operation.

For kitchen tickets, duplicate food production can be as damaging as a missing ticket, so
**automatic retries from `UNKNOWN` are usually the wrong policy.**

---

## 8. Physical verification options

For when firmware status isn't enough evidence, a small hardware sidecar is realistic.

| Method | Confirms | Does NOT confirm |
|---|---|---|
| Basic exit sensor (IR break-beam) | Paper moved, approximate timing | Anything was printed on it |
| Printed verification patch + reflective sensor | Paper emerged; region was thermally marked | Receipt contents |
| QR/DataMatrix scanner at exit | Physical media emerged, contains thermal marks, marks include the correct job ID, sufficient contrast/focus for machine readability | — (this is the strongest affordable confirmation) |

For truly critical kitchen/production tickets, QR verification at the exit is the
strongest affordable confirmation method. An ESP32 sidecar could publish, e.g.:

```json
{
  "jobId": "7F3A-92C1",
  "paperSeen": true,
  "verificationPatchSeen": true,
  "qrDecoded": true,
  "cutOrSeparationSeen": true
}
```

Keep only one job in flight so the physical sensor event maps unambiguously to the current
receipt.

---

## 9. Printers with cleaner documented completion APIs (for future purchases)

| Vendor / API | Behavior |
|---|---|
| **Star** SDK — `beginCheckedBlock` / `endCheckedBlock` | Documented to return when the transferred data has printed completely, the printer goes offline during printing, or a timeout occurs. Very close to the API we actually need. |
| **Epson ePOS-Print** | Two modes: with spooler disabled, response returned after printing completes; with spooler enabled, initial response means "queued," and the app can later query the result via the print job ID. Cleaner than a raw status byte because jobs have explicit IDs and are queryable. |
| **Native IPP** | "Completed" is defined as the job reaching final state with media stacked in the output destination. However, a gateway that can't obtain device status is allowed to mark a job completed with reason `queued-in-device` — meaning it will never get better status. `completed` must always be read together with `job-state-reasons`. |

---

## 10. Summary of the detection mechanism

```
Send receipt
    ↓
Printer prints receipt
    ↓
Send completion marker as part of same data stream
    ↓
Printer finishes processing print
    ↓
← Printer returns marker
    ↓
We know printing completed
```

Then send the cut command, place another marker behind it, wait for that, and query
cutter/error status. If `GS ( H` isn't supported, fall back to `GS r 1` (documented by
Epson for exactly this purpose), and/or Xprinter's `ESC v`/`ESC x` busy/idle query once its
exact behavior is confirmed with the vendor.

See [docs/testing-plan.md](testing-plan.md) for the probe script that determines which of
these this specific XP-S260M unit and firmware actually supports, over which interface.
