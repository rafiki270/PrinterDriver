/**
 * printerdriver-react-native — the public entry point.
 *
 * This is the only module in the package that imports 'react-native'. Everything else is
 * plain TypeScript over a small injected interface, which is why the whole JavaScript
 * layer can be unit-tested on a bare Node host (see test/ and README "Verification
 * status").
 */

import { getNativePrinterDriver } from './NativePrinterDriver.ts';
import { setNativeModuleLoader } from './native.ts';

setNativeModuleLoader(getNativePrinterDriver);

export { PrinterDriver } from './PrinterDriver.ts';
export { Printer, PrinterDriverError } from './Printer.ts';
export type { SelfTestResult } from './Printer.ts';
export { PrintJob, isTerminalState } from './PrintJob.ts';
export { PrintQueue } from './PrintQueue.ts';
export { EventStream } from './events.ts';
export type { Subscription } from './events.ts';
export { Payloads, Receipt } from './payload.ts';
export { PayloadError } from './marshal.ts';
export {
  NativeModuleUnavailableError,
  setNativeModule,
  installSinkForTesting,
} from './native.ts';

export {
  Alignment,
  Binarization,
  CodePage,
  CommandLanguage,
  CompletionAuthority,
  CompletionMechanism,
  ConfidenceGrade,
  ConfidenceLevel,
  CutSetting,
  CutVariant,
  DetectionStatus,
  DeviceEvent,
  DrainOrder,
  DrawerKickMethod,
  DrawerPortStandard,
  DrawerState,
  DrawerStatusMethod,
  FailureReason,
  JobOutcome,
  JobState,
  MatchKind,
  OpKind,
  PayloadKind,
  PreflightMode,
  ProfileSelection,
  Provenance,
  UnknownEnumValueError,
  codePageOrder,
  confidenceGradeLetters,
  enumMirrors,
} from './enums.ts';
export type { Tristate } from './enums.ts';

export type {
  AutoDetectOptions,
  DetectedPrinter,
  DetectionSummary,
  DeviceEventMessage,
  DeviceStatus,
  DiscoverOptions,
  DiscoveredDevice,
  DocumentOp,
  DocumentPayload,
  DrawerCapabilities,
  DrawerReading,
  DrawerRequest,
  DrawerResult,
  DriverConfig,
  JobEvent,
  JobOptions,
  JobResult,
  Payload,
  PrinterCapabilities,
  QueueOptions,
  QueuePolicy,
  RasterPayload,
  RawPayload,
  ReprintOptions,
  SelfTestOptions,
  TcpPrinterConfig,
} from './types.ts';

export type { CustomTransport } from './transports.ts';
export {
  assertNonPrinting,
  pinLevelToNative,
} from './registrations.ts';
export type {
  BlockHandlerRegistration,
  BlockRenderResult,
  CompletionMatch,
  CompletionMethodRegistration,
  DrawerKickRegistration,
  FormatterRegistration,
  PinLevel,
  ProbeFinding,
  ProbeStepRegistration,
} from './registrations.ts';

export type { Spec as NativePrinterDriverSpec } from './NativePrinterDriver.ts';
export { nativeMethodNames, methodNameForAbiFunction } from './methodNames.ts';
