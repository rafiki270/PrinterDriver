/**
 * The public API against a recording stand-in for the TurboModule: does each idiomatic
 * call reach the right ABI entry point, with the right marshalled arguments, and does it
 * interpret the answer honestly?
 *
 * This is the layer where a wrapper bug would be invisible to the C++ syntax check and to
 * the enum bridge alike -- calling pd_force_reprint when the caller asked for the banner to
 * be suppressed, say, or reading a `done` out of an `unknown`.
 */

import { test, beforeEach } from 'node:test';
import assert from 'node:assert/strict';

import {
  ConfidenceGrade,
  CompletionAuthority,
  ConfidenceLevel,
  JobOutcome,
  RenderPath,
  ReportKind,
} from '../src/enums.ts';
import { installSinkForTesting, resetBridgeForTesting, setNativeModule } from '../src/native.ts';
import { Payloads, Receipt } from '../src/payload.ts';
import { PrinterDriver } from '../src/PrinterDriver.ts';
import { PrinterDriverError } from '../src/Printer.ts';
import { createFakeNative } from './support/fakeNative.ts';
import type { FakeNative } from './support/fakeNative.ts';

let fake: FakeNative;

beforeEach(() => {
  resetBridgeForTesting();
  fake = createFakeNative();
  setNativeModule(fake.module);
  installSinkForTesting();
});

function driver(): PrinterDriver {
  return PrinterDriver.create({ storageDirectory: '/tmp/pd' });
}

// --- driver -------------------------------------------------------------------------------

test('create passes the storage directory through and keeps the handle', () => {
  const instance = driver();
  assert.deepEqual(fake.lastCall('create')?.args, [
    { storageDirectory: '/tmp/pd', fsyncDisabled: 0, hasLog: 0 },
  ]);
  assert.equal(instance.instanceNonce, 'K7');
  assert.equal(instance.localSubnet, '192.168.1.0/24');
  assert.deepEqual(instance.profileIds(), ['generic', 'xprinter_s_series']);
});

test('a driver that cannot be created is an error, not a silent null handle', () => {
  fake.answers.set('create', 0);
  assert.throws(() => driver(), PrinterDriverError);
});

test('an onLog callback is flagged to the native side and receives log messages', () => {
  const seen: string[] = [];
  PrinterDriver.create({ onLog: (message) => seen.push(message) });
  assert.equal((fake.lastCall('create')?.args[0] as { hasLog: number }).hasLog, 1);
  fake.sink?.('log', { message: 'transport reconnecting' });
  assert.deepEqual(seen, ['transport reconnecting']);
});

// --- printers and jobs ---------------------------------------------------------------------

test('addPrinter marshals the tcp config and print marshals payload and options', () => {
  const printer = driver().addPrinter({ host: '192.168.1.101', widthDots: 576, profileId: 'generic' });
  assert.equal(printer.id, 'tcp:192.168.1.101:9100');
  assert.equal(printer.widthDots, 576);
  assert.equal(printer.completion, 'gsParenH');
  assert.equal(printer.completionProvenance, 'probed');
  assert.equal(printer.language, 'escPos');

  const job = printer.print(Payloads.text(['Order 7F3A']), { key: 'order-7F3A#kitchen-1' });
  const call = fake.lastCall('print');
  assert.equal(call?.args[0], 1);
  assert.equal(call?.args[1], 10);
  assert.deepEqual(call?.args[3], {
    key: 'order-7F3A#kitchen-1',
    cut: 0,
    openDrawer: 0,
    preflight: 0,
    timeoutMs: 0,
    topFeedDots: 0,
    bottomFeedDots: 0,
    suppressVerificationId: 0,
  });
  assert.equal(job.key, 'order-1');
  assert.equal(job.printToken, 'K73F');
  assert.equal(job.cutToken, 'K740');
});

test('a refused submission raises the driver error rather than returning a dead job', () => {
  fake.answers.set('print', 0);
  fake.answers.set('lastError', 'payload was malformed');
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  assert.throws(() => printer.print(Payloads.raw(Uint8Array.from([1]))), /payload was malformed/);
});

test('forceReprint uses the plain ABI call until the banner is spoken about', () => {
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  printer.forceReprint('order-1');
  assert.equal(fake.callsTo('forceReprint').length, 1);
  assert.equal(fake.callsTo('forceReprintOpts').length, 0);

  printer.forceReprint('order-1', { banner: false });
  assert.equal(fake.callsTo('forceReprintOpts').length, 1);
  const options = fake.lastCall('forceReprintOpts')?.args[3] as { suppressBanner: number };
  assert.equal(options.suppressBanner, 1);
});

test('job events are subscribed once, streamed, and closed by a terminal state', async () => {
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const job = printer.print(new Receipt().line('hi').build());
  const seen: string[] = [];
  job.events.on((event) => seen.push(event.state));
  job.events.on(() => {});
  assert.equal(fake.callsTo('subscribeJob').length, 1, 'subscription must be idempotent');

  const emit = (state: number) =>
    fake.sink?.('job', {
      job: 100,
      state,
      confidence: 0,
      hasReason: 0,
      reason: 0,
      monotonicMs: 1,
    });
  emit(0);
  emit(2);
  emit(6);
  assert.deepEqual(seen, ['queued', 'sendStarted', 'doneSoftware']);
  assert.equal(job.events.isClosed, true);
});

test('the terminal result comes back as the tri-state union', async () => {
  fake.answers.set('jobAwait', {
    outcome: JobOutcome.unknown,
    confidence: ConfidenceLevel.transportAccepted,
    reason: 5,
    grade: ConfidenceGrade.eTransportOnly,
    authority: CompletionAuthority.transportOnly,
    method: 'none',
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const job = printer.print(Payloads.text(['x']));
  const result = await job.result;
  assert.equal(result.outcome, 'unknown');
  if (result.outcome !== 'unknown') throw new Error('unreachable');
  assert.equal(result.reason, 'timeoutAwaitingCompletion');
  assert.equal(result.grade, 'eTransportOnly');
  assert.equal(result.authority, 'transportOnly');
});

test('awaitResult with a timeout returns null rather than pretending the job failed', async () => {
  fake.answers.set('jobAwait', null);
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const job = printer.print(Payloads.text(['x']));
  assert.equal(await job.awaitResult(500), null);
});

test('send() reports progress and resolves once with the terminal result', async () => {
  fake.answers.set('jobAwait', {
    outcome: JobOutcome.done,
    confidence: ConfidenceLevel.cutFaultFree,
    reason: 0,
    grade: ConfidenceGrade.aJobLevelConfirmation,
    authority: 0,
    method: 'GS(H) fn48',
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const progress: string[] = [];
  const pending = printer.send(Payloads.text(['x']), {
    key: 'order-2',
    onProgress: (event) => progress.push(event.state),
  });
  fake.sink?.('job', { job: 100, state: 3, confidence: 0, hasReason: 0, reason: 0, monotonicMs: 1 });
  const result = await pending;
  assert.deepEqual(progress, ['bytesSent']);
  assert.equal(result.outcome, 'done');
});

// --- device status and events ------------------------------------------------------------------

test('status and refreshStatus both come back as tri-state fields', async () => {
  const raw = {
    connected: 1,
    observed: 1,
    online: 1,
    coverOpen: 0,
    paperOut: 0,
    paperNearEnd: -1,
    cutterError: 0,
    unrecoverableError: 0,
    recoverableError: 0,
  };
  fake.answers.set('printerStatus', raw);
  fake.answers.set('printerRefreshStatus', raw);
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  assert.equal(printer.status().paperNearEnd, 'unknown');
  assert.equal((await printer.refreshStatus(700)).online, 'yes');
});

test('device events arrive as closed enum members', () => {
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const seen: string[] = [];
  printer.events.on((event) => seen.push(event));
  fake.sink?.('device', { printer: 10, event: 12 });
  assert.deepEqual(seen, ['foreignWriterDetected']);
});

// --- custom transports ---------------------------------------------------------------------------

test('a JS transport is wired to the sink and answers connect and write honestly', async () => {
  const written: number[] = [];
  const instance = driver();
  const printer = instance.addCustomPrinter(
    {
      description: 'bt-spp:00:11:22:33:44:55',
      connect: () => true,
      write: (data) => {
        written.push(data.byteLength);
        // A short write, reported as a short write.
        return 1;
      },
      close: () => {},
    },
    { profileId: 'generic', widthDots: 384 }
  );
  const call = fake.lastCall('addPrinterCustom');
  assert.equal(call?.args[2], 'bt-spp:00:11:22:33:44:55');
  assert.equal(call?.args[3], 'generic');
  assert.equal(call?.args[4], 384);

  const transportId = call?.args[1] as number;
  fake.sink?.('transport.connect', { requestId: 1, transport: transportId });
  fake.sink?.('transport.write', {
    requestId: 2,
    transport: transportId,
    data: new ArrayBuffer(9),
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(fake.responses, [
    { requestId: 1, value: { ok: true } },
    { requestId: 2, value: { written: 1 } },
  ]);
  assert.deepEqual(written, [9]);

  assert.equal(printer.feedBytes(Uint8Array.from([0x10, 0x04])), true);
  assert.equal(printer.reportLinkDropped('out of range'), true);
});

// --- custom registrations ----------------------------------------------------------------------------

test('a completion method registers with its claimed grade and answers matches', async () => {
  const instance = driver();
  instance.registerCompletionMethod({
    id: 'acme.x-idle',
    grade: 'bOrderedDeviceResponse',
    authority: 'physicalPrinter',
    methodName: 'ESC x idle',
    fenceBytes: (token) => new TextEncoder().encode(`\x1bx${token}`).buffer as ArrayBuffer,
    match: () => ({ kind: 'matched', token: 'K73F' }),
  });
  assert.deepEqual(fake.lastCall('registerCompletionMethod')?.args[1], {
    id: 'acme.x-idle',
    grade: ConfidenceGrade.bOrderedDeviceResponse,
    authority: CompletionAuthority.physicalPrinter,
    methodName: 'ESC x idle',
  });

  fake.sink?.('completion.match', {
    requestId: 1,
    id: 'acme.x-idle',
    data: new ArrayBuffer(4),
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(fake.responses, [{ requestId: 1, value: { kind: 0, token: 'K73F' } }]);
});

test('a refused registration is reported and its handlers are unwired again', () => {
  fake.answers.set('registerFormatter', false);
  fake.answers.set('lastError', 'duplicate id');
  const instance = driver();
  assert.throws(
    () => instance.registerFormatter({ name: 'money', format: () => 'x' }),
    /duplicate id/
  );
  fake.sink?.('formatter.format', { requestId: 1, name: 'money', value: '', args: '', locale: '' });
  assert.deepEqual(fake.responses, [{ requestId: 1, value: { handled: false, text: '' } }]);
});

test('a probe step with printable request bytes is refused before it reaches the ABI', () => {
  const instance = driver();
  assert.throws(
    () =>
      instance.registerProbeStep({
        id: 'acme.hello',
        requestBytes: new TextEncoder().encode('HELLO').buffer as ArrayBuffer,
        classify: () => ({ answered: true, label: 'x' }),
      }),
    /printable/
  );
  assert.equal(fake.callsTo('registerProbeStep').length, 0);
  // The non-printing form is accepted: DLE EOT 1 is every byte below 0x20.
  instance.registerProbeStep({
    id: 'acme.status',
    requestBytes: Uint8Array.from([0x10, 0x04, 0x01]).buffer,
    classify: () => ({ answered: true, label: 'answered' }),
  });
  assert.equal(fake.callsTo('registerProbeStep').length, 1);
});

test('a drawer kick without both status callbacks registers as having no readable switch', () => {
  const instance = driver();
  instance.registerDrawerKick({
    id: 'acme.kick',
    kickBytes: () => new ArrayBuffer(5),
  });
  assert.deepEqual(fake.lastCall('registerDrawerKick')?.args[1], {
    id: 'acme.kick',
    hasStatus: false,
  });
});

// --- cash drawer ---------------------------------------------------------------------------------------

test('openDrawer returns a state, never a boolean', async () => {
  fake.answers.set('drawerOpen', {
    state: 3,
    previousState: 0,
    channel: 1,
    pulseMs: 100,
    elapsedMs: 143,
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const result = await printer.openDrawer({ channel: 1 });
  assert.equal(result.state, 'kickSentUnverified');
  assert.equal(result.elapsedMs, 143);
  assert.deepEqual(fake.lastCall('drawerOpen')?.args[2], { channel: 1, pulseMs: 0 });
});

test('drawer capabilities decode the port standard and provenances separately', () => {
  fake.answers.set('printerDrawerCapabilities', {
    present: 1,
    standard: 1,
    voltage: 24,
    maxCurrentMa: 1000,
    channelCount: 2,
    sensorPin: 6,
    method: 2,
    defaultPulseMs: 100,
    maxPulseMs: 200,
    cooldownMs: 500,
    canKickDuringPrint: 0,
    statusAvailable: 1,
    statusMethod: 2,
    sharedBetweenDrawers: 1,
    sharedWithBuzzer: 0,
    electricalProvenance: 0,
    commandsProvenance: 2,
    kickable: 0,
  });
  const capabilities = driver().addPrinter({ host: '10.0.0.1' }).drawerCapabilities();
  assert.equal(capabilities.standard, 'star24v6p6c');
  assert.equal(capabilities.sensorPin, 6);
  assert.equal(capabilities.electricalProvenance, 'documented');
  assert.equal(capabilities.commandsProvenance, 'unverified');
  assert.equal(capabilities.kickable, false);
});

// --- print queue ----------------------------------------------------------------------------------------

test('the queue binds hold, block and unblock rather than only enqueue', () => {
  const instance = driver();
  const printer = instance.addPrinter({ host: '10.0.0.1' });
  const queue = instance.createQueue({ holdWhileOffline: true, maxDepth: 64, defaultTtlMs: 300000 });
  assert.deepEqual(fake.lastCall('queueCreate')?.args[1], {
    holdWhileOffline: 1,
    defaultTtlMs: 300000,
    maxDepth: 64,
    drainOrder: 0,
  });
  queue.enqueue(printer, Payloads.text(['ticket']), { key: 'order-3', priority: 2 });
  assert.equal((fake.lastCall('queueEnqueue')?.args[3] as { priority: number }).priority, 2);
  queue.pause('p1');
  queue.resume('p1');
  assert.equal(queue.isBlocked('p1'), false);
  queue.unblock('p1');
  queue.tick();
  assert.equal(queue.pending(), 0);
  assert.equal(queue.drainedCount, 0);
});

// --- detection ----------------------------------------------------------------------------------------------

test('autoDetect streams candidates and resolves with all of them', async () => {
  const summary = {
    endpoint: '192.168.1.101:9100',
    vendor: 'Xprinter',
    model: 'XP-S260M',
    firmware: '',
    serial: '',
    identityTrusted: 0,
    confidencePercent: 35,
    impersonationSuspected: 1,
    identityFresh: 1,
    profileId: 'xprinter_s_series',
    selection: 1,
    nominalPaperMm: 80,
    printableWidthDots: 576,
    charsPerLine: 48,
    dpi: 203,
    completion: 0,
    gradeCeiling: 1,
    authority: 0,
    method: 'GS(H) fn48',
    completionProvenance: 1,
    drawerPresent: 1,
    drawerKickable: 1,
    drawerStandard: 0,
    drawerVoltage: 24,
    drawerElectricalProvenance: 0,
    drawerCommandsProvenance: 2,
    provenanceSummary: 'GS(H) fn48 Probed',
    degradations: ['BARCODE not supported on this path'],
  };
  fake.answers.set('autoDetect', 1);
  const instance = driver();
  const sweep = instance.autoDetect({ subnetCidr: '192.168.1.0/24' });
  const requestId = fake.lastCall('autoDetect')?.args[2] as number;
  fake.sink?.('detected', {
    requestId,
    completed: 1,
    total: 254,
    printer: {
      endpoint: '192.168.1.101:9100',
      host: '192.168.1.101',
      port: 9100,
      status: 0,
      portOpen: 1,
      fromCache: 0,
      dleEotHex: '16',
      summary,
    },
  });
  const found = await sweep.done;
  assert.equal(found.length, 1);
  assert.equal(found[0]?.status, 'answered');
  assert.equal(found[0]?.summary.identityTrusted, false);
  assert.equal(found[0]?.summary.impersonationSuspected, true);
  assert.deepEqual(found[0]?.summary.degradations, ['BARCODE not supported on this path']);
  assert.equal(sweep.candidates.isClosed, true);
});

test('a sweep that the core refuses surfaces the driver error', async () => {
  fake.answers.set('discover', -1);
  fake.answers.set('lastError', 'CIDR wider than /16');
  const sweep = driver().discover({ subnetCidr: '10.0.0.0/8' });
  await assert.rejects(sweep.done, /CIDR wider than \/16/);
});

test('selfTest returns the ticket text alongside the ordinary tri-state result', async () => {
  fake.answers.set('selfTest', {
    result: {
      outcome: JobOutcome.done,
      confidence: ConfidenceLevel.cutFaultFree,
      reason: 0,
      grade: ConfidenceGrade.aJobLevelConfirmation,
      authority: 0,
      method: 'GS(H) fn48',
    },
    detection: {
      endpoint: '192.168.1.101:9100',
      vendor: 'Xprinter',
      model: 'XP-S260M',
      firmware: '',
      serial: '',
      identityTrusted: 1,
      confidencePercent: 90,
      impersonationSuspected: 0,
      identityFresh: 1,
      profileId: 'xprinter_s_series',
      selection: 1,
      nominalPaperMm: 80,
      printableWidthDots: 576,
      charsPerLine: 48,
      dpi: 203,
      completion: 0,
      gradeCeiling: 1,
      authority: 0,
      method: 'GS(H) fn48',
      completionProvenance: 1,
      drawerPresent: 1,
      drawerKickable: 1,
      drawerStandard: 0,
      drawerVoltage: 24,
      drawerElectricalProvenance: 0,
      drawerCommandsProvenance: 2,
      provenanceSummary: 'GS(H) fn48 Probed',
      degradations: [],
    },
    key: 'selftest-1',
    printToken: 'K73F',
    ticketText: 'PRINTERDRIVER SELF-TEST\n',
    job: 100,
  });
  const printer = driver().addPrinter({ host: '192.168.1.101' });
  const outcome = await printer.selfTest({ noBarcode: true });
  assert.equal(outcome.result.outcome, 'done');
  assert.equal(outcome.printToken, 'K73F');
  assert.match(outcome.ticketText, /SELF-TEST/);
  assert.equal(outcome.detection.gradeCeiling, 'aJobLevelConfirmation');
});

// --- the receipt DSL ------------------------------------------------------------------------------------------

const documentJson = JSON.stringify({
  meta: { width: 576 },
  blocks: [{ type: 'text', text: 'Order {{order.id}}' }],
});

/** One degradation and one hard refusal, in the wire shape the C++ module writes. */
const missingPathEntry = {
  kind: 0,
  block: 'blocks[0]',
  requested: '{{order.note}}',
  delivered: 'omitted',
  path: 2,
  note: 'no such path in the model',
};

test('renderDocument previews without printing and types every report entry', async () => {
  fake.answers.set('renderDocument', {
    ok: 1,
    bytes: new ArrayBuffer(12),
    codePage: 16,
    hasCut: 1,
    cut: 1,
    topFeedDots: 0,
    bottomFeedDots: 90,
    report: [missingPathEntry],
    error: '',
  });
  const printer = driver().addPrinter({ host: '192.168.1.101' });
  const rendered = await printer.renderDocument(
    { blocks: [{ type: 'text', text: 'hi' }] },
    { order: { id: '7F3A' } },
    { widthDots: 384, timeZone: '+02:00' }
  );

  const call = fake.lastCall('renderDocument');
  assert.equal(call?.args[0], 1);
  assert.equal(call?.args[1], 10);
  // A plain object is stringified here; the core's own strict reader parses it, so an
  // object and a stored .json template reach the identical parser.
  assert.equal(call?.args[2], '{"blocks":[{"type":"text","text":"hi"}]}');
  assert.equal(call?.args[3], '{"order":{"id":"7F3A"}}');
  assert.deepEqual(call?.args[4], {
    widthDots: 384,
    cutClearanceDots: 0,
    maxRowsPerBand: 0,
    locale: '',
    currency: '',
    timeZone: '+02:00',
  });

  assert.equal(rendered.bytes.byteLength, 12);
  assert.equal(rendered.codePage, 'wpc1252');
  assert.equal(rendered.meta.cut, 'partial');
  assert.equal(rendered.meta.bottomFeedDots, 90);
  assert.equal(rendered.report.length, 1);
  const entry = rendered.report[0];
  assert.equal(entry?.kind, 'missingPath');
  assert.equal(entry?.path, 'notRendered');
  assert.equal(entry?.block, 'blocks[0]');
  assert.equal(entry?.note, 'no such path in the model');
  // Nothing printed and no job exists.
  assert.equal(fake.callsTo('print').length, 0);
  assert.equal(fake.callsTo('printDocumentJson').length, 0);
});

test('a document that asked for no cut reports no cut, not the profile default', async () => {
  fake.answers.set('renderDocument', {
    ok: 1,
    bytes: new ArrayBuffer(0),
    codePage: 0,
    hasCut: 0,
    cut: 0,
    topFeedDots: 0,
    bottomFeedDots: 0,
    report: [],
    error: '',
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const rendered = await printer.renderDocument(documentJson);
  assert.equal(rendered.meta.cut, null);
  assert.deepEqual(rendered.report, []);
  // A JSON string is passed through untouched, and an absent model stays absent.
  assert.equal(fake.lastCall('renderDocument')?.args[2], documentJson);
  assert.equal(fake.lastCall('renderDocument')?.args[3], '');
});

test('a refused render carries both the reason and the report that explains it', async () => {
  fake.answers.set('renderDocument', {
    ok: 0,
    bytes: new ArrayBuffer(0),
    codePage: 0,
    hasCut: 0,
    cut: 0,
    topFeedDots: 0,
    bottomFeedDots: 0,
    report: [
      {
        kind: 3,
        block: 'document',
        requested: 'template',
        delivered: 'nothing',
        path: 2,
        note: 'a template was submitted with no model',
      },
    ],
    error: 'the document is a template and no model was supplied',
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  await assert.rejects(printer.renderDocument(documentJson), (error: Error) => {
    assert.equal(error.name, 'PrinterDriverError');
    assert.match(error.message, /no model was supplied/);
    assert.match(error.message, /document {2}malformedTemplate/);
    return true;
  });
});

test('printDocument submits an ordinary job and attaches the render report to it', async () => {
  fake.answers.set('printDocumentJson', {
    job: 100,
    report: [missingPathEntry],
    error: '',
  });
  fake.answers.set('jobAwait', {
    outcome: JobOutcome.done,
    confidence: ConfidenceLevel.cutFaultFree,
    reason: 0,
    grade: ConfidenceGrade.aJobLevelConfirmation,
    authority: 0,
    method: 'GS(H) fn48',
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  const job = await printer.printDocument(documentJson, { order: { id: '7F3A' } }, {
    key: 'order-7F3A#kitchen-1',
    bottomFeedDots: 120,
  });

  // The document's meta reaches the job through the ordinary pd_job_options.
  assert.deepEqual(fake.lastCall('printDocumentJson')?.args[4], {
    key: 'order-7F3A#kitchen-1',
    cut: 0,
    openDrawer: 0,
    preflight: 0,
    timeoutMs: 0,
    topFeedDots: 0,
    bottomFeedDots: 120,
    suppressVerificationId: 0,
  });
  // It IS a print job: same handle table, same events, same tri-state result.
  assert.equal(job.key, 'order-1');
  assert.equal(job.renderReport.length, 1);
  assert.equal(job.renderReport[0]?.kind, 'missingPath');
  const result = await job.result;
  assert.equal(result.outcome, 'done');
  // A receipt that printed with a dropped placeholder is a receipt that printed.
  assert.equal(job.renderReport[0]?.delivered, 'omitted');
});

test('a document that produces no bytes submits nothing at all', async () => {
  fake.answers.set('printDocumentJson', {
    job: 0,
    report: [{ ...missingPathEntry, kind: 3, block: 'document' }],
    error: 'the document JSON did not parse',
  });
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  await assert.rejects(printer.printDocument('{ not json'), /did not parse/);
});

test('a document that cannot be stringified never reaches the ABI', async () => {
  const cyclic: Record<string, unknown> = {};
  cyclic.self = cyclic;
  const printer = driver().addPrinter({ host: '10.0.0.1' });
  await assert.rejects(printer.renderDocument(cyclic), /not representable as JSON/);
  assert.equal(fake.callsTo('renderDocument').length, 0);
});

test('the driver reads the last report back through the index reader', () => {
  fake.answers.set('renderReportCount', 2);
  fake.answers.set('renderReportAt', (_driver: unknown, index: unknown) =>
    index === 0 ? missingPathEntry : { ...missingPathEntry, kind: 9, path: 0 }
  );
  const instance = driver();
  const report = instance.renderReport();
  assert.equal(report.length, 2);
  assert.equal(report[0]?.kind, 'missingPath');
  assert.equal(report[1]?.kind, 'truncated');
  assert.equal(report[1]?.path, 'hardware');
  assert.equal(instance.reportKindName('missingPath'), 'MissingPath');
  assert.equal(fake.lastCall('reportKindName')?.args[0], ReportKind.missingPath);
  assert.equal(instance.renderPathName('notRendered'), 'Hardware');
  assert.equal(fake.lastCall('renderPathName')?.args[0], RenderPath.notRendered);
});

// --- lifetime -------------------------------------------------------------------------------------------------

test('destroy is awaited and is idempotent', async () => {
  const instance = driver();
  await instance.destroy();
  await instance.destroy();
  assert.equal(fake.callsTo('destroy').length, 1);
});
