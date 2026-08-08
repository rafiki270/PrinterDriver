# API Design: The Public Interface

Companion to [docs/sdk-spec.md](sdk-spec.md). This defines the surface apps program
against — designed to be as close as possible to a drop-in replacement for the existing
POS printing stack (see [§8 migration mapping](#8-drop-in-mapping-for-the-existing-pos-stack)),
while staying generic enough for any other app. The SDK's consumers are **several
in-house apps** — the web-based POS monorepo and the native POS suite — plus anything
built later, so nothing app-specific may leak into the interface.

---

## 1. Design principles

1. **Data in, events out.** An app hands the SDK a payload and gets back a job handle
   whose event stream tells the truth about what happened. No app ever touches sockets,
   chunking, pacing, ESC/POS bytes, or status polling.
2. **One submit call.** Everything is `print(payload, options) → Job`. Payload tiers
   (§3) cover different app styles; feedback (§4) is identical for all of them, because
   the core appends the completion fences regardless of what the payload was.
3. **Closed enums everywhere.** `JobState`, `ConfidenceLevel`, `DeviceEvent`,
   `FailureReason` are defined once in the core and re-exported verbatim by every
   wrapper. Wrappers cannot add, drop, or partially implement values.
4. **Tri-state outcome — the one deliberate non-drop-in change.** A job ends `done`,
   `failed`, or `unknown`. There is intentionally no `isSuccess` boolean: collapsing
   `unknown` into either bucket is exactly the bug that produces duplicate kitchen
   tickets. Apps must handle three outcomes; the compiler makes them
   (exhaustive `switch`/`when`).

## 2. Object model

Three nouns:

```
PrinterDriver   — the service. Owns connections, queues, the persistent job store.
Printer         — a handle to one configured device (transport + width + profile).
PrintJob        — one submitted job: event stream + queryable state, durable across restarts.
```

```
PrinterDriver
 ├─ addPrinter(config) → Printer          // fleet-style, stable printerId
 ├─ printer(transport, width) → Printer   // ad-hoc convenience, keyed by endpoint
 ├─ job(idempotencyKey) → PrintJob?       // look up any job, incl. after app restart
 └─ events → stream of (printerId, DeviceEvent)

Printer
 ├─ print(payload, options) → PrintJob
 ├─ forceReprint(key, options) → PrintJob // explicit duplicate; renders REPRINT banner
 ├─ status() → DeviceStatus               // snapshot: online, paper, cover, errors
 ├─ events → stream of DeviceEvent
 └─ openCashDrawer()

PrintJob
 ├─ id / idempotencyKey
 ├─ events → stream of JobEvent           // ends with a terminal event, always
 ├─ state → JobState                      // current, queryable any time
 └─ result (awaitable) → JobResult        // terminal state + confidence + reason
```

Printer config: `transport` (`tcp(host, port=9100)` | `bluetooth(address)` |
`usb(descriptor)` | `serial(device, baud)`), `widthDots` (384 | 504 | 576), optional
`profile` hint (else auto-probed and cached — the capability probe from
[testing-plan.md](testing-plan.md) run once per printer, results persisted).

## 3. Payload tiers

All three produce identical feedback. Pick per call, mix freely.

### Tier 1 — Raster (the web-POS drop-in path)

```
Payload.raster(image)        // platform bitmap: UIImage/CGImage, Android Bitmap,
                             // or raw RGBA + width for the C ABI
```

The core does dithering, scaling/fit to the printer's dot width, banding for tall images
(the 1024 px Epson split), and pacing. The app keeps rendering receipts however it wants
(the existing web POS: React → canvas) and hands over the finished image.
`canvasToEscposCommands`, chunk delays, and dithering code in the app get deleted.

### Tier 2 — Document builder (for apps without a renderer)

```
Payload.document(
  Receipt(width: .dots576)
    .align(.center).bold().line("MY RESTAURANT")
    .line("Order 7F3A-92C1")
    .qr("7F3A-92C1", size: .medium)
    .feed(3)
)
```

Standard receipt semantics only — text, style, alignment, feed, barcode/QR, raster
inline. Encodes to the conservative ESC/POS subset; code pages (e.g. CP852) handled per
capability profile.

### Tier 3 — Raw bytes (escape hatch)

```
Payload.raw(bytes)
```

For anything the tiers don't cover. The core still wraps it: preflight before, `GS ( H`
fence + cut + fence after — so even raw jobs get real completion feedback. Constraint:
raw payloads must not embed their own realtime status tricks or cuts (documented; the
core's trailing cut/fence assumes it owns job termination).

### Job options (all optional)

```
key:        idempotency key — caller-supplied stable ID (order/ticket UUID).
            Omitted → SDK generates one (feedback still works, but no dedupe
            protection across restarts; fleet/POS apps should always pass one).
cut:        .partial (default) | .full | .none
openDrawer: false (default)
preflight:  .strict (default: refuse on cover-open/paper-out → failed(preflight…))
            | .skip
timeoutMs:  per-phase completion-wait budget (default per profile)
```

Re-submitting an existing `key` does **not** print — it returns the existing `PrintJob`
(whatever state it is in). Printing the same key again is only possible via
`forceReprint`, which marks the ticket (`*** REPRINT / POSSIBLE DUPLICATE ***`,
`PRINT ATTEMPT: n` — prepended as a text banner even for raster payloads).

## 4. Feedback: one stream, honest terminal states

`JobEvent` carries `(JobState, ConfidenceLevel, FailureReason?, timestamp)` using the
enums from [sdk-spec.md §5](sdk-spec.md#5-the-single-feedback-point) — the states are
exactly techspec §5.1's machine (`Queued → PreflightOk → SendStarted → BytesSent →
PrintConfirmed → CutCommandProcessed → DoneSoftware`, plus `FailedKnown` / `Unknown`).

Terminal result, the tri-state:

```
JobResult
 ├─ .done(confidence)     // reached DoneSoftware (or PhysicallyVerified later);
 │                        // confidence says what that claim rests on — e.g.
 │                        // .cutFaultFree on a GS(H) printer vs .transportAccepted
 │                        // on a write-only printer. The SDK never inflates this.
 ├─ .failed(reason)       // FailedKnown: nothing printed or failure confirmed
 │                        // (preflight refusal, transport unreachable, cutter fault…)
 └─ .unknown              // bytes sent, no acknowledgement (timeout, crash, link drop).
                          // NOT success, NOT failure. Surface to an operator;
                          // resolve via forceReprint or manual confirmation.
```

Two subtleties, both deliberate:

- **A completion-wait timeout ends in `unknown`, not `failed`** — bytes were sent; the
  receipt may well be printing. This is precisely where today's 5-second-timeout code
  goes wrong.
- **Crash recovery:** on startup the core re-reads its job store; anything that reached
  `SendStarted` without an acknowledgement resurfaces as `unknown`
  (`driver.job(key)` after restart). Never auto-retried.

`DeviceEvent` (per printer, from ASB + status queries): `online/offline`,
`coverOpen/Closed`, `paperOut/NearEnd/Ok`, `cutterError`, `recoverable/unrecoverableError`,
`connectionLost/Restored`. This stream replaces availability ping-polling.

## 5. Swift surface (sketch)

```swift
import PrinterDriver

let driver = PrinterDriver(storage: .default)
let kitchen = driver.printer(.tcp(host: "192.168.1.101"), width: .dots576)

// Drop-in style: print a rendered receipt image with an order-derived key.
let job = try await kitchen.print(.raster(receiptImage),
                                  options: .init(key: "order-7F3A-92C1#kitchen-1"))

for await event in job.events {
    updateTicketUI(event.state, event.confidence)
}

switch try await job.result {
case .done(let confidence):   markTicketPrinted(confidence)
case .failed(let reason):     showFailure(reason)        // safe to resubmit same key
case .unknown:                askOperator(job)           // forceReprint or confirm
}
```

## 6. Kotlin surface (sketch)

```kotlin
val driver = PrinterDriver(context, Storage.DEFAULT)
val kitchen = driver.printer(Transport.Tcp("192.168.1.101"), Width.DOTS_576)

val job = kitchen.print(Payload.Raster(bitmap), JobOptions(key = "order-7F3A-92C1#kitchen-1"))

scope.launch { job.events.collect { updateTicketUI(it.state, it.confidence) } }

when (val result = job.await()) {
    is JobResult.Done    -> markTicketPrinted(result.confidence)
    is JobResult.Failed  -> showFailure(result.reason)
    JobResult.Unknown    -> askOperator(job)
}
```

Both wrappers are generated-thin: enum bridging + async adapters over the C ABI. No
logic.

## 7. Web bridge (how the existing web-POS shell consumes it)

The existing web POS runs inside a native shell; today the web side pushes raw TCP
bytes through `TcpModule`. The SDK replaces that module with a `PrinterModule` exposing
the same three nouns over the existing webNativeBridge, with TypeScript types generated
from the core enums:

```ts
const jobId = await printerModule.print({
  printer: { transport: { tcp: { host } }, widthDots: 576 },
  payload: { rasterPngBase64 },
  options: { key: `order-${orderId}#kitchen-1` },
});
printerModule.onJobEvent(jobId, (e) => { /* state, confidence, reason */ });
const result = await printerModule.result(jobId);   // 'done' | 'failed' | 'unknown'
```

The web app keeps its React → canvas rendering and its own routing of the 15 printable
types to printer roles. What it deletes: `canvasToEscposCommands.ts`, chunking/delays,
`escposFunctions.ts` transmission paths, the DLE EOT pseudo-check, `enqueue-task`
per-host queues, and availability ping-polling.

## 8. Drop-in mapping for the existing POS stack

| Today (existing POS monorepo) | With the SDK |
|---|---|
| `renderToCanvas()` → `canvasToEscposCommands()` → chunked TCP with 9–21 ms delays | `print(.raster(image), key)` — conversion, banding, pacing in core |
| `enqueue-task` per-host serial queue | Core's one-active-job-per-printer queue (required for fencing anyway) |
| `DLE EOT` after send + 5 s timeout ⇒ boolean | Job event stream ⇒ tri-state result with `ConfidenceLevel` |
| `Device` availability via ping every 5 s | `printer.events` (`online/offline/paperOut/coverOpen/…`) + `status()` snapshot |
| `PrinterType` `escpos` \| `epos` | Transport + capability profile (`epos` transport = open question [sdk-spec §11.6](sdk-spec.md#11-open-questions)) |
| Rongta early-cut 6-newline hack in app code | Profile quirk flag in core |
| CP852 hardcoded, emoji filtered | Encoding per profile (document tier); raster tier unaffected |
| Manual-reprint duplicate warnings for some doc types | `key` dedupe + `forceReprint` with attempt banner, uniformly |
| Android `TcpModule` stub (no Android printing) | Same core, both platforms |
| `PrinterDiagnostics.tsx` test sheet | Keep — render it as raster through the SDK like any job |

Migration is per-printer-path, not big-bang: the raster tier means the first integration
step is only swapping the transport+feedback layer under the existing renderer.

## 9. C ABI (what every other wrapper binds)

```c
pd_driver*   pd_create(const pd_config*);                 // storage path, log hook
pd_printer   pd_add_printer(pd_driver*, const pd_printer_config*);
pd_job       pd_print(pd_driver*, pd_printer, const pd_payload*, const pd_job_options*);
pd_job       pd_force_reprint(pd_driver*, pd_printer, const char* key, const pd_job_options*);
pd_job       pd_find_job(pd_driver*, const char* key);    // incl. after restart
void         pd_subscribe_job(pd_driver*, pd_job, pd_job_event_cb, void* ctx);
void         pd_subscribe_device(pd_driver*, pd_printer, pd_device_event_cb, void* ctx);
pd_status    pd_printer_status(pd_driver*, pd_printer);
void         pd_open_cash_drawer(pd_driver*, pd_printer);
void         pd_destroy(pd_driver*);
```

Payloads: `PD_PAYLOAD_RASTER_RGBA { const uint8_t* pixels; uint32_t w, h; }`,
`PD_PAYLOAD_DOCUMENT { const pd_op* ops; size_t count; }`, `PD_PAYLOAD_RAW { bytes }`.
Enums are plain C enums with an explicit `_COUNT`; adding a value is a core release that
regenerates every wrapper — the mechanism that keeps "not implemented on platform X"
impossible.

## 10. Open API questions

1. Is `key` required (safer for POS) or optional-with-generated-default (friendlier for
   casual apps)? Current lean: optional in core, **required by lint/convention in the
   POS integrations**.
2. Reprint banner on raster payloads: prepend as text lines (current plan) or composite
   into the image?
3. Does `Printer.print` accept multiple copies natively, or is that app-side looping
   with distinct keys? Lean: app-side (`#kitchen-1`, `#kitchen-2` suffixes), keeps
   dedupe semantics obvious.
4. Discovery API (`driver.discover() → stream of found printers`) — v1 or later? The
   monorepo's scan is a stub today, so nothing depends on it yet.
5. ePOS transport in core vs ESC/POS-mode-only for Epson TM models —
   [sdk-spec §11.6](sdk-spec.md#11-open-questions).
