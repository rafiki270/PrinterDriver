# Changelog

## Unreleased

- Custom transports (docs/compatibility-brief.md §25): `CustomTransport`,
  `PrinterDriver.addCustomPrinter`, `Printer.feedBytes` and
  `Printer.reportLinkDropped` bind `pd_add_printer_custom`,
  `pd_transport_feed_bytes` and `pd_transport_link_dropped`, so a Bluetooth, MFi,
  vendor-SDK or USB link the application owns is driven with the same fence, journal and
  grading as a TCP printer. `connect` and `write` are native function pointers because no
  `dart:ffi` callback can answer the core's worker thread with a value; `close` may be a
  Dart function, through `CustomTransport.withDartClose`.
- `ConfidenceGrade` gained `aPlusDurableQueryableJob` at value 0 and A..E shifted up by
  one, mirroring the A+ tier of docs/compatibility-brief.md §24. Nothing produces A+:
  the ePOS transport does not exist in the core. **A mirror of an older wrapper read
  against a newer library would report every grade one rung too strong.**
- New mirrored enums `Provenance` and `CommandLanguage` (§28, §1), plus
  `Printer.completionProvenance` and `Printer.language`: whether a printer's fence is
  documented, probed or merely assumed, and which language its profile is driven in.
  Anything but `escPos` is refused with `unsupported` before a byte is written.
- `grade`, `authority` and `method` moved from `JobDone` up to `JobResult`. The ABI
  carries them on all three outcomes, and a refusal that grades `eTransportOnly` with
  method `none` is information a failed job previously dropped.
- `PrintJob.events` is live, transition by transition, and has been since the C ABI
  started handing its job events over by value. The README and the note below described
  a limitation that no longer exists.

## 0.1.0

First release: hand-written `dart:ffi` bindings for the PrinterDriver C ABI
(`capi/include/printerdriver/pd.h`).

- `PrinterDriver`, `Printer` and `PrintJob` over the whole ABI: TCP printers, the three
  payload tiers, job options (key, cut, drawer, preflight, timeout), `findJob`,
  `forceReprint`, device status and the device event stream.
- Tri-state `JobResult` as a sealed class — `JobDone` / `JobFailed` / `JobUnknown`, no
  success boolean — plus `Stream<JobEvent> events`, `Future<JobResult> result` and the
  `send(..., onProgress:)` closure form.
- Every enum of pd.h mirrored member for member, with an explicit `unrecognized` landing
  spot so a newer native library can never be read as a wrong member.
- Native library resolution through `PRINTERDRIVER_LIB_PATH`, an explicit path, or the
  platform loader; `addScriptedPrinterForTesting` for driving the scripted devices of a
  library built from the `printerdriver_capi_testing` target.

Known limitation at the time: `PrintJob.events` delivered the job's complete ordered
history when the job settled rather than transition by transition, because the C ABI
then handed its job-event callback a pointer that did not outlive the call. Lifted
above.
