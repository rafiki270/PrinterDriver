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

## 17. Wrapper parity contract

**Every capability of the C++ core is available in every wrapper, through an idiomatic
interface.** No wrapper is a subset. The chain is: C++ core → C ABI (`pd.h`) → each
wrapper (Swift, Dart, .NET, Kotlin). Two obligations:

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
without a binding in all four wrappers fails the check. Idiomatic wrappers may satisfy a
function through a property or a higher-level method; those cases are listed explicitly
in the check's allowlist (`scripts/parity_allowlist.txt`) with the member that covers
them, so the mapping stays visible rather than silently absent.

The check searches each wrapper's SOURCE tree only — `wrappers/swift/Sources`,
`wrappers/dart/lib`, `wrappers/dotnet/PrinterDriver`, `wrappers/android/src/main` — never
its tests, which can bind the ABI directly and would satisfy every line of the contract
while the public surface stayed empty. Two further properties keep it honest: when a built
C ABI library is present, every `pd_` symbol it exports must appear in the list parsed out
of the header, so a declaration the parser misses cannot quietly excuse itself; and
`scripts/check_parity.sh --self-test` runs three negative controls (a function nobody has
bound must fail all four wrappers, one binding hidden from one wrapper must fail that
wrapper alone, and the plain run must be green), because a check nobody has watched go red
proves nothing. CI runs the self-test before the check itself.

Two consequences are worth stating, because they are what the contract costs. First,
"every wrapper" includes the one whose tests cannot run on the build machine: the Android
wrapper is held to the same list, verified through the JNI glue's types and its
external-symbol check rather than through a JVM (`wrappers/android/README.md`). Second, a
wrapper is allowed to be shaped by its runtime — §16's Dart registration surface is the
standing example — but never to be smaller.

### 17.1 What the C ABI does not reach yet

Obligation 2 above is now enforced. Obligation 1 — *every core capability is exposed in
the C ABI* — is not, and the difference is a list rather than a feeling. A reverse audit of
the public C++ headers against `pd.h` found the following stranded, in rough order of what
it costs a wrapper. Nothing here is a wrapper's fault and no wrapper can fix it; each item
is an ABI addition, and adding one means binding it in four wrappers, which is why they are
written down instead of rushed.

- **The receipt DSL and the template layer** (`dsl/include/**`). `pd.h`'s document tier is a
  flat `pd_op` array with five kinds; `dsl::render`, `parseDocument`, `bind`,
  `applyFormatter`, the barcode encoder, the UTF-8 column-width helpers and `dsl::Json` have
  no `pd_*` entry point. The renderer is reachable from C only from inside `pd_self_test`,
  which lays out one fixed diagnostic ticket. **This has a consequence for §16**: a wrapper
  can register a formatter or a block handler, and the core stores it, but the only code
  that consults those records is the DSL render path — so from a wrapper today they are
  registrations with no reachable call site. The three that do fire end to end are the
  completion method, the probe step and the drawer kick. A `pd_render_document` /
  `pd_bind_template` pair is the missing piece, and until it exists that asymmetry belongs
  in this document rather than in a surprised integrator's afternoon.
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
