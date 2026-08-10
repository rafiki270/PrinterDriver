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

### Tier 2b — Receipt-DSL documents and templates (normative, M19)

The document tier's richer form: a JSON document or a template with a bound parameter
model ([receipt-dsl.md](receipt-dsl.md)), rendered against the printer's own capability
profile.

```
printer.printDocument(documentJson, model: modelJson, options: …)  -> PrintJob
printer.renderDocument(documentJson, model: modelJson)             -> { bytes, meta, report }
```

Two entry points, in the C ABI as `pd_print_document_json` and `pd_render_document`, and
bound in all five wrappers. Three properties are normative:

1. **`printDocument` is `print`.** The rendered bytes go through the identical engine
   path: same worker, same preflight, same completion fence, same confidence grading,
   same idempotency-key dedupe, same `PrintJob`. There is no second engine.
2. **`renderDocument` prints nothing.** It returns the bytes a printer would receive and
   the render report, so a caller can inspect the declared degradations before committing
   paper. No job exists.
3. **Degradations are declared, refusals are explained.** A missing model path, an unknown
   formatter, a barcode a profile has no `GS k` for: all of these RENDER, and each becomes
   a typed render-report entry (`kind`, where, `requested`, `delivered`, `path`, note).
   Only three things stop bytes being produced — JSON that is not JSON, a structure that
   is not a document, and a template submitted with no model — and each returns an error
   **plus** a report entry, submitting nothing. A malformed document never becomes a blank
   receipt.

The document's `meta` reaches the job under the precedence of
[receipt-dsl.md](receipt-dsl.md): the caller's `JobOptions` wins, the document's `meta.cut`
and `meta.margins` fill in what the caller left alone, and the printer's profile answers
what neither said. The engine's blade-clearance floor is applied on top and is
unconditional.

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
(whatever state it is in; a key whose job terminally **failed** starts a fresh attempt
instead, attempt+1, no banner — nothing printed the first time). Printing the same key
again after a `done`/`unknown` outcome is only possible via `forceReprint`.

### The reprint banner (normative)

`forceReprint` marks the ticket in the core, on every platform identically:

```
*** REPRINT / POSSIBLE DUPLICATE ***
ORDER: <key>
PRINT ATTEMPT: <n>
```

- Prepended as text lines for **all three payload tiers** (raster included — the banner
  precedes the image).
- The exact strings are exported constants (`kReprintBannerLine`,
  `kReprintAttemptPrefix`) so wrapper tests assert the same bytes the wire carries.
- **Configurable, enabled by default.** `ReprintOptions.banner: true` (default) |
  `false`. Disabling is a per-call, deliberate act for receipts where the banner is
  inappropriate (e.g. a customer-facing copy); kitchen tickets should never disable it —
  the banner is what lets staff bin the duplicate instead of cooking it twice. A
  driver-level default (`DriverConfig.reprintBannerDefault`) may tighten this to
  always-on for a deployment, never loosen it silently.
- Fresh attempts after a terminal `failed` never carry the banner (no duplicate risk
  exists); the attempt counter still increments.

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
logic. The **Flutter/Dart wrapper is first-wave too** (one consumer app is Flutter,
using the document tier) and follows the same shape via FFI: `Stream<JobEvent>`,
`Future<JobResult>`, sealed result classes.

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
4. Discovery API (`driver.discover() → stream of found printers`) — v1 or later? Demand
   exists on both sides: the web POS's scanner is a stub, and the native suite has
   Android-only /24 scanning with no iOS equivalent.
5. ePOS transport in core vs ESC/POS-mode-only for Epson TM models —
   [sdk-spec §11.6](sdk-spec.md#11-open-questions).

## 11. Planned addon: print queue

Layered on this API, not part of it — design and safety rules in
[sdk-spec §12](sdk-spec.md#12-print-queue-addon-planned). Sketch:

```swift
let queue = PrintQueue(driver, policy: .init(holdWhileOffline: true,
                                             defaultTTL: .minutes(5)))
let job = queue.enqueue(on: kitchen, .document(doc),
                        options: .init(key: "order-7F3A-92C1#kitchen-1"))
// `job` is a normal PrintJob — same event stream, same tri-state result.
// Extra pre-send state while held: .heldOffline
// Extra failure reasons: .expired (TTL passed while held), .queueOverflow
```

The queue drains through the same core submit path (fenced, confidence-graded), never
re-drives a job that reached `.unknown`, and returns the existing job when a key is
re-enqueued.

## 12. Send-with-closure ergonomics (wrappers)

Every wrapper exposes, besides the stream/await forms, a closure form:

```swift
printer.send(receipt, key: "order-7F3A-92C1#kitchen-1",
             onProgress: { event in ticketUI.update(event.state) }) { result in
  switch result {                       // terminal, exactly once
  case .done(let confidence): markPrinted(confidence)
  case .failed(let reason):   showFailure(reason)
  case .unknown:              askOperator()
  }
}
```

Kotlin: trailing lambda + optional `onProgress`; Dart: `Future<JobResult>` +
`onProgress` callback. Rules: the terminal closure fires exactly once, always (crash
recovery included via findJob); `onProgress` receives every JobEvent in order; both are
sugar over the same core event stream — no separate code path.

## 13. explore(): printer metadata as data

`printer.explore()` (and `driver.explore(transport)` pre-add) returns the full
`PrinterInfo` produced by the fingerprint + probe machinery
([capability-profiles.md](capability-profiles.md)) as one metadata object:

```
PrinterInfo
├─ identity: vendor, model, firmware, serial?, identityTrusted, confidencePercent
├─ media: nominalPaperMm, printableWidthDots, dpi, charsPerLine (per font),
│         sensors {paperEnd, nearEnd, cover, blackMark}, cutter {full, partial}
├─ completion: mechanism, confidenceCeiling, grade (A–E), authority
├─ features: codepages, barcodeSymbologies, qrMax, drawerKick, buzzer,
│            maintenanceCounters, recoveryCommands (data only)
└─ transports: available + recommended ranking
```

Serializable (JSON) like everything else; cached from the persisted probe, refreshed on
demand with `explore(refresh: true)`. The DSL renderer consumes `media` — apps consume
the rest (e.g. showing "80 mm / 576 dots / cutter OK" in settings UIs).

## 14. Verification identifiers: the wire token as a first-class ID

The `GS ( H` fence token — 4 printable chars from the 94⁴ (~78 M) space, laid out as
`[2-char random per-instance nonce][2-char job sequence]` — is promoted from throwaway
correlation to a **receipt verification identifier (RVI)**:

- **Journaled.** Each job's print-fence and cut-fence tokens are persisted in its `J`
  record alongside key and UUID. The instance nonce is persisted in the store at first
  start, so tokens remain resolvable across restarts.
- **Printed.** The ticket footer carries it next to the order id — `ORDER: <key>
  V:<token>` — and inside the QR payload. Toggleable per job
  (`JobOptions.printVerificationId`), **enabled by default**.
- **Resolvable — paper → job.** `driver.jobByToken("K73F") → PrintJob?` returns the job
  (most recent first on sequence wrap; 94² = 8 836 sequences per instance, and a token is
  never reused while outstanding, so same-shift lookups are unambiguous in practice —
  the journal timestamp disambiguates the rest). `pdctl verify <token>` prints the job's
  full journal history: states, timestamps, grade, authority, method, attempts.
- **Targetable — job → action.** `forceReprint`, status queries, and operator flows
  accept the token anywhere a key is accepted.
- **Attributable echoes.** Because the printer echoes these exact bytes at physical
  print-completion, holding a receipt whose `V:` code matches a journaled
  `PrintConfirmed` token is end-to-end evidence: this paper is the output of that job,
  and the printer acknowledged finishing it. A foreign echo's token (multi-writer case,
  `ForeignWriterDetected`) identifies which instance and job it belonged to.

The idempotency key remains the fleet-wide business identity; the RVI is the
per-print physical-evidence identity. Key ↔ UUID ↔ RVI resolve in both directions
through the journal.

### 14.1 No four-character literal is a valid test probe

A consequence of the layout that has already cost us one intermittent failure across
three suites. Because the nonce is drawn at random and the sequence starts at 0, **every
four-character string in the alphabet is a token some instance mints**, and the two
literals a human reaches for first are the worst two available:

| Literal  | What it actually is                | Fails when the run draws |
|----------|------------------------------------|--------------------------|
| `"!!!!"` | sequence 0 — the *first* print token a driver hands out | nonce `!!` |
| `"~~~~"` | sequence 8835 — the last of the space | nonce `~~` |

Used as "a token nobody minted", `"!!!!"` resolves to the job under test on 1 run in
8 836; used as "another instance's token", `"~~~~"` is ours on the same odds and no
`ForeignWriterDetected` is raised. Both reproduce on demand by pinning the nonce — write
the two characters to `<store>/instance.nonce` before constructing the driver.

Build probes from `instanceNonce()` (`pd_instance_nonce`) instead:

- **Unminted, ours:** `instanceNonce() + "~~"` — sequence 8835, which a rig that has
  printed a handful of jobs is thousands of leases short of reaching.
- **Foreign:** bump the first nonce character within the alphabet (`~` wraps to `!`) and
  append any sequence.

## 15. Self-test and auto-detection

Two composition APIs over existing machinery (discovery, identify, probe, DSL, fences):

**`driver.autoDetect(subnet?, options?)`** → the one-call path from "I know nothing" to
configured printers: LAN discovery (non-printing DLE EOT sweep) → multi-signal identify
per candidate → non-destructive capability probe (respecting the stored-findings cache)
→ returns `[DetectedPrinter{printer, identity{vendor, model, trusted, confidence%},
profileId, provenance summary, completionMechanism, grade ceiling}]`. Nothing prints;
nothing fires. Wrappers surface it as an async stream (candidates arrive as found).
`pdctl autodetect [cidr]` prints the table.

**`printer.selfTest(options?)`** → prints ONE diagnostic ticket through the full fenced
engine — the paper is the detection report:

```
PRINTERDRIVER SELF-TEST
sdk <version> · <date time>
────────────────────────────
IDENTITY   vendor/model (GS I, trusted YES/NO, NN%)
PROFILE    <id> — selected by DOCUMENTED|PROBED|DEFAULT
MEDIA      <dots> dots · <chars> cols · <dpi> dpi
COMPLETION <mechanism> — grade ceiling <A+..E>, provenance
CHARSET    příliš žluťoučký kůň / árvíztűrő / zażółć
BARCODE    [Code128 sample]   QR [V-token]
DRAWER     <port standard> · <voltage> — <provenance>
FENCE      this ticket's own GS(H)/fence round-trip: the
           terminal result below proves the mechanism live
ORDER: selftest-<ts>   V:<token>
```

Ends with the ordinary tri-state result — a `Done(grade A)` self-test IS the proof the
stack works end-to-end on this unit; blocks the profile can't do appear as declared
degradations on the ticket itself (e.g. "BARCODE: not supported on this path"). The
printer's own built-in self-test (`GS ( A`, `DC2 T`) remains separately reachable as
`pdctl test-print` — vendor firmware's view vs. this, the SDK's view. Agent endpoints:
`POST /printers/<id>/self-test`, `POST /autodetect`.

## 16. Custom method registration (extensibility)

Integrators can extend the SDK at runtime without forking it. Registration points, all
per-driver-instance, all data-plus-callbacks (no subclassing across the ABI):

- **Custom completion mechanism** — `driver.registerCompletionMethod(id, {fenceBytes:
  fn(jobToken) -> bytes, matcher: fn(incoming bytes) -> Matched(token)|NotMine,
  grade, authority, method-name})`: the engine sends the fence behind the payload and
  attributes the matched response exactly like GS ( H. This is how a vendor-specific
  idle/ack scheme (e.g. a confirmed Xprinter ESC x contract) becomes a first-class
  grade-A path without a core release. Registered mechanisms appear in profiles as
  `CompletionMechanism::VendorIdle` + the registered id.
- **Custom probe step** — `registerProbeStep(id, {request bytes (MUST be
  non-printing — declared, and enforced by a printable-byte lint), classify: fn(response)
  -> finding})`: extends `probe`/`autoDetect` fingerprinting for house-specific quirks.
- **Custom document block** — `registerBlockHandler(kind, fn(block json, profile) ->
  encoder ops | degradation entry)`: new DSL block types (loyalty stamps, local fiscal
  fields) render through the same pipeline and report degradations the same way.
- **Custom formatter** — `registerFormatter(name, fn(value, args, locale) -> string)`
  for `{{v|name:...}}` in templates.
- **Custom drawer kick method** — `registerDrawerKick(id, {kickBytes: fn(channel,
  pulseMs), statusRead?: ...})` filling `DrawerKickMethod::VENDOR`.
- **Custom transports** — already available via the vtable ABI (§ M13b) — this section
  completes the set.

Rules: registrations are process-local (never persisted into shared journals beyond
their ids), ids are namespaced strings (`"acme.x-idle"`), and anything a registration
claims (grade, authority) is attributed to it by id in results and `pdctl verify`
output, so a custom method's claims are auditable like the built-ins.

**Where each one fires.** The completion mechanism, the probe step and the drawer kick are
driven by the engine every `print` goes through. The block handler and the formatter are
consulted by the receipt-DSL render path, which since M19 has entry points of its own:
`pd_render_document` / `pd_print_document_json` (§3, tier 2b) wire this driver's registries
into the parser, the binder and the renderer, so a formatter registered from any wrapper
backs `{{ v | name }}` on a template printed from that wrapper. Before M19 those two were
accepted and stored but reachable only from C++, and §17.1 said so.

**Callbacks and the wrapper that cannot serve them.** Every callback above is invoked on a
core thread and must answer there and then — a fence before the payload is flushed, a
matcher before the next chunk arrives. Swift, Kotlin and .NET can do that (a `@convention(c)`
thunk, a JNI upcall on an attached thread, a rooted delegate), and their registration APIs
take ordinary closures. `dart:ffi` cannot, for the same reason the custom-transport vtable
is native there: `NativeCallable.isolateLocal` may only be invoked on its own isolate's
mutator thread, `.listener` supports `void` returns only, and `.isolateGroupBound` runs with
no isolate entered. A matcher whose return value came from whatever the trampoline left in
the register would be a completion claim made by accident. So the Dart surface takes native
function pointers (`CustomCompletionMethod.fromLibrary(...)` and its four siblings) — the
shape a vendor plugin already has — and that asymmetry is a property of the runtime, not a
gap in the wrapper.

React Native takes a third shape, and for a third reason. JavaScript is single-threaded and
may only be touched on the JS thread, so its TurboModule marshals each question onto that
thread through the `CallInvoker` and blocks the core thread on a condition variable until
`respond` comes back — with a deadline, after which the registration's own documented
failure is used (a short write, a `notMine` verdict, a declined formatter). That is only
safe because no blocking `pd_*` call ever runs on the JS thread: every one of them,
`pd_render_document` and `pd_print_document_json` included, runs on a worker and surfaces
as a `Promise`. A JS thread parked inside the ABI could be the very thread a core worker is
waiting on, and the two would deadlock. The registration APIs still take ordinary
closures.

## 17. Wrapper parity contract

**Every capability of the C++ core is available in every wrapper, through an idiomatic
interface.** No wrapper is a subset. The chain is: C++ core → C ABI (`pd.h`) → each
wrapper (Swift, Dart, .NET, Kotlin, React Native). Two obligations:

1. **Every core capability is exposed in the C ABI.** A feature that only exists as a
   C++ method, unreachable through `pd.h`, is incomplete — the wrappers bind `pd.h`, not
   the C++ headers.
2. **Every public `pd.h` function is bound in every wrapper**, presented naturally for
   the language (async streams for subscriptions, sealed types for the tri-state result,
   `Flow`/`AsyncStream`/`Stream`/`IAsyncEnumerable` for event and discovery feeds — not a
   raw 1:1 translation of C signatures).

Enforced, not trusted: a parity check (`scripts/check_parity.sh`) enumerates the public
`pd_*` functions in `pd.h` and asserts each wrapper references every one, mirroring how
the enum-bridge tests already enforce enum parity. A `pd_` function added to the ABI
without a binding in all five wrappers fails the check. Idiomatic wrappers may satisfy a
function through a property or a higher-level method; those cases are listed explicitly
in the check's allowlist (`scripts/parity_allowlist.txt`) with the member that covers
them, so the mapping stays visible rather than silently absent.

The check searches each wrapper's SOURCE tree only — `wrappers/swift/Sources`,
`wrappers/dart/lib`, `wrappers/dotnet/PrinterDriver`, `wrappers/android/src/main`, and
both halves of the React Native package (`wrappers/react-native/cpp` for the TurboModule
that calls `pd.h`, `wrappers/react-native/src` for the TypeScript API an app imports) —
never its tests, which can bind the ABI directly and would satisfy every line of the
contract while the public surface stayed empty. A wrapper split across two languages is
graded on both halves for the same reason: `cpp/` alone would grade it by its glue, `src/`
alone would look for C function names in a language that never spells them.

Two further properties keep it honest: when a built C ABI library is present, every `pd_`
symbol it exports must appear in the list parsed out of the header, so a declaration the
parser misses cannot quietly excuse itself; and `scripts/check_parity.sh --self-test` runs
four negative controls (the plain run must be green; a function nobody has bound must fail
all five wrappers; one binding hidden from one wrapper must fail that wrapper alone; and
the same, aimed at the multi-directory wrapper, since the third control never touches it
and would stay green if that lane matched everything or nothing), because a check nobody
has watched go red proves nothing. CI runs the self-test before the check itself.

The React Native package carries the same three questions locally, so a developer who never
leaves `wrappers/react-native/` still finds a missing binding: `npm run abi:check` compares
a generated mirror of `pd.h` against the checked-in one, and `test/parity.test.ts` asserts
that every `pd_*` function has a TurboModule method, that the codegen spec declares it, and
that `cpp/PrinterDriverModule.cpp` really calls it. `scripts/check_parity.sh` is still the
gate; that is the same question asked where the work happens.

Two consequences are worth stating, because they are what the contract costs. First,
"every wrapper" includes the ones whose tests cannot run on the build machine: the Android
wrapper is held to the same list, verified through the JNI glue's types and its
external-symbol check rather than through a JVM (`wrappers/android/README.md`), and the
React Native package the same way, through a `clang++ -fsyntax-only` pass over its
TurboModule against the real `pd.h` rather than through a built app
(`wrappers/react-native/README.md`). Second, a wrapper is allowed to be shaped by its
runtime — §16's Dart registration surface is the standing example, and React Native's
rule that no blocking `pd_*` call may run on the JS thread is another — but never to be
smaller.

### 17.1 What the C ABI does not reach yet

Obligation 2 above is now enforced. Obligation 1 — *every core capability is exposed in
the C ABI* — is not yet, and the difference is a list rather than a feeling. A reverse
audit of the public C++ headers against `pd.h` found the following stranded, in rough order
of what it costs a wrapper. Nothing here is a wrapper's fault and no wrapper can fix it;
each item is an ABI addition, and adding one means binding it in five wrappers, which is
why they are written down instead of rushed.

Items are struck through as they are closed, rather than deleted: a list that only ever
shrinks silently is a list nobody can audit.

- ~~**The receipt DSL and the template layer**~~ — **closed by M19.** `pd_render_document`
  and `pd_print_document_json` (§3, tier 2b) parse, bind and render a DSL document against
  a printer's profile, returning the ESC/POS bytes and a typed render report read back with
  `pd_render_report_at`; all five wrappers bind both. **The consequence for §16 is closed
  with it**: the block handler and the formatter now have a reachable call site, so all
  five registration points fire end to end from every wrapper. What is still C++-only in
  this layer is narrower and worth naming: `dsl::Json` as a value type, `documentToJson` /
  `serializeDocument` (a wrapper composes JSON in its own language instead), `bindString`
  for one-off labels, `renderText`'s character-cell preview, and named `ImageAsset`s for
  `image` blocks — a document must carry its pixels inline to cross this ABI.
- **`CapabilityProfile` is opaque in C.** Only flattened facets come back (width,
  completion, provenance, language, the drawer facet, `pd_detection_summary`). The
  Bluetooth capability record is not among them, so an iOS wrapper cannot ask the SDK for a
  model's MFi `ExternalAccessory` protocol string — the one fact it needs to open an
  `EASession` — although the database records it and records whether it is vendor-gated.
  `devices::byName` is likewise unreachable: all the shipped profiles are selectable by id
  and inspectable by nobody.
- **The reasoning behind a probe or an identification.** `pd_detection_summary` carries the
  verdict; `IdentityAssessment::signals` (the ordered human-readable reasons `pdctl probe`
  prints), the per-step `CapabilityFindings`, the findings cache (`FindingsStore`) and
  `probePath` / `classifyPath` / `explainPath` — which answer "does *this* interface path
  forward status bytes at all?" — stay in C++. A wrapper can show that a guess is 35%
  confident but never why. The label a registered probe step's `classify` returns has no way
  back to C either.
- **Two of four transports.** `transport.hpp`'s serial factory and `transport_bluez.hpp`'s
  RFCOMM factory are C++-only; a C wrapper wanting either must reimplement the link behind
  `pd_transport_vtable`. **Two of three engines**, likewise: `pd_printer_language` refuses
  anything but ESC/POS, so `star.hpp` and `epos.hpp` are unreachable from C even though the
  core implements both.
- **The encoder's richness.** `escpos::Encoder` has underline, text scaling, per-document
  code-page switches, cut-with-feed, the maintenance and memory-switch queries and
  `transliterate`; `pd_op` has text, line, align, bold and feed.
- **Enumeration and history.** `PrinterDriver::printers()`, `PrintJob::history()`,
  `PrintQueue::waiting()`, `JobStore::all()` and `readJournal` have no C form, so a wrapper
  can resolve one job by key or token but cannot audit the journal or list a lane.
- **Smaller, safe, and simply not done yet**: `PrinterConfig`'s `ProbePolicy` /
  `ProbeOptions` / `IdentityHints` (a wrapper cannot say "always re-probe this bench unit"
  or "never touch this device"), `Printer::probeNow`, `discovery::probeHost` and the CIDR
  helpers, `isNonPrintingRequest` / `isValidRegistrationId` as pre-flight validators, and
  the `to_string` overloads on DSL and parser enums that have no `pd_*_name` twin.
