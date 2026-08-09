/**
 * The TurboModule method names, and the one rule that produces them.
 *
 * Split out of src/NativePrinterDriver.ts on purpose: that file imports 'react-native' for
 * `TurboModuleRegistry`, and React Native's entry point is Flow-typed source that a plain
 * Node process cannot parse. This module imports nothing, so test/parity.test.ts can hold
 * it against pd.h on any host.
 */

/**
 * Every method name above that corresponds to a public `pd_*` function, in one runtime
 * array so the parity test can compare it against pd.h without a type-level trick.
 * `setEventSink` and `respond` are deliberately absent: they are module plumbing, not ABI.
 */
export const nativeMethodNames: readonly string[] = [
  'create',
  'destroy',
  'lastError',
  'profileIds',
  'instanceNonce',
  'localSubnet',
  'addPrinterTcp',
  'addPrinterCustom',
  'transportFeedBytes',
  'transportLinkDropped',
  'printerId',
  'printerWidthDots',
  'printerCompletion',
  'printerCompletionProvenance',
  'printerLanguage',
  'printerStatus',
  'printerRefreshStatus',
  'openCashDrawer',
  'printerDrain',
  'subscribeDevice',
  'print',
  'forceReprint',
  'forceReprintOpts',
  'findJob',
  'jobByToken',
  'jobId',
  'jobKey',
  'jobPrintToken',
  'jobCutToken',
  'jobAttempt',
  'jobCurrentState',
  'jobConfidence',
  'jobIsTerminal',
  'subscribeJob',
  'jobAwait',
  'jobStateName',
  'confidenceLevelName',
  'deviceEventName',
  'failureReasonName',
  'jobOutcomeName',
  'confidenceGradeName',
  'confidenceGradeLetter',
  'completionAuthorityName',
  'provenanceName',
  'commandLanguageName',
  'payloadKindName',
  'completionMechanismName',
  'cutVariantName',
  'drainOrderName',
  'drawerStateName',
  'drawerPortStandardName',
  'drawerKickMethodName',
  'drawerStatusMethodName',
  'profileSelectionName',
  'detectionStatusName',
  'matchKindName',
  'codePageAt',
  'queueCreate',
  'queueDestroy',
  'queueEnqueue',
  'queuePause',
  'queueResume',
  'queueIsPaused',
  'queueIsBlocked',
  'queueUnblock',
  'queuePending',
  'queueExpiredCount',
  'queueOverflowCount',
  'queueDrainedCount',
  'queueTick',
  'printerDrawerCapabilities',
  'drawerOpen',
  'drawerReadSensor',
  'drawerCalibratePolarity',
  'drawerPolarityCalibrated',
  'drawerHighMeansOpen',
  'selfTest',
  'autoDetect',
  'discover',
  'detectedAt',
  'discoveredAt',
  'registerCompletionMethod',
  'registerProbeStep',
  'registerBlockHandler',
  'registerFormatter',
  'registerDrawerKick',
  'renderDocument',
  'printDocumentJson',
  'renderReportAt',
  'renderReportCount',
  'reportKindName',
  'renderPathName',
];

/**
 * `pd_add_printer_tcp` -> `addPrinterTcp`. The one naming rule, exported because the
 * parity test applies it to pd.h and the native module applies it in C++ (the method
 * table in cpp/PrinterDriverModule.cpp is written out with the same spellings).
 */
export function methodNameForAbiFunction(abiFunction: string): string {
  const bare = abiFunction.startsWith('pd_') ? abiFunction.slice(3) : abiFunction;
  const parts = bare.split('_').filter((part) => part.length > 0);
  return parts
    .map((part, index) => (index === 0 ? part : part.charAt(0).toUpperCase() + part.slice(1)))
    .join('');
}
