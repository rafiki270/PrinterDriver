# Changelog

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

Known limitation: `PrintJob.events` delivers the job's complete ordered history when the
job settles rather than transition by transition, because the C ABI hands its job-event
callback a pointer that does not outlive the call. See the README.
