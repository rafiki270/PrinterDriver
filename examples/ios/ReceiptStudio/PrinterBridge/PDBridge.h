//
//  PDBridge.h
//  ReceiptStudio
//
//  Objective-C face of the PrinterDriver C++ core. Deliberately pure Objective-C:
//  no C++ leaks into this header, so Swift can import it through the bridging
//  header without needing C++ interop.
//
//  Two rules this bridge exists to enforce:
//
//   1. Every closed core enum is re-exported verbatim (docs/api.md §1.3). Nothing
//      here adds a case, drops a case, or folds two cases together. In particular
//      there is no isSuccess: PDJobOutcome is tri-state and stays tri-state
//      (docs/api.md §1.4).
//   2. Core callbacks arrive on printer worker threads. Everything handed back to
//      Swift is re-dispatched onto the main queue first, so UI code never has to
//      think about which thread it is on.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#pragma mark - Closed enums (re-exported verbatim from core/include/printerdriver/types.hpp)

/// docs/sdk-spec.md §5, mirroring docs/techspec.md §5.1.
typedef NS_ENUM(NSInteger, PDJobState) {
    PDJobStateQueued = 0,
    PDJobStatePreflightOk,
    PDJobStateSendStarted,
    PDJobStateBytesSent,
    PDJobStatePrintConfirmed,
    PDJobStateCutCommandProcessed,
    PDJobStateDoneSoftware,
    PDJobStatePhysicallyVerified,
    PDJobStateFailedKnown,
    PDJobStateUnknown,
    PDJobStateHeldOffline,
};

/// What evidence backs the current claim. Never inflated by the SDK or by this bridge.
typedef NS_ENUM(NSInteger, PDConfidenceLevel) {
    PDConfidenceLevelTransportAccepted = 0,
    PDConfidenceLevelPrinterHealthy,
    PDConfidenceLevelPrintConfirmed,
    PDConfidenceLevelCutProcessed,
    PDConfidenceLevelCutFaultFree,
    PDConfidenceLevelPhysicallyVerified,
};

typedef NS_ENUM(NSInteger, PDFailureReason) {
    PDFailureReasonNone = 0,
    PDFailureReasonTransportUnreachable,
    PDFailureReasonPreflightCoverOpen,
    PDFailureReasonPreflightPaperOut,
    PDFailureReasonPreflightHardwareError,
    PDFailureReasonTimeoutAwaitingCompletion,
    PDFailureReasonCutterFault,
    PDFailureReasonUnsupported,
    PDFailureReasonUnknown,
    PDFailureReasonExpired,
    PDFailureReasonQueueOverflow,
};

/// docs/api.md §1.4: deliberately tri-state. Collapsing Unknown into either bucket
/// is exactly the bug that produces duplicate tickets.
typedef NS_ENUM(NSInteger, PDJobOutcome) {
    PDJobOutcomeDone = 0,
    PDJobOutcomeFailed,
    PDJobOutcomeUnknown,
};

/// docs/device-database.md "Confidence grades for every route".
typedef NS_ENUM(NSInteger, PDConfidenceGrade) {
    PDConfidenceGradeAJobLevelConfirmation = 0,
    PDConfidenceGradeBOrderedDeviceResponse,
    PDConfidenceGradeCDeviceStatusAround,
    PDConfidenceGradeDSpoolerCompleted,
    PDConfidenceGradeETransportOnly,
};

/// Who is actually making the claim carried by a result.
typedef NS_ENUM(NSInteger, PDCompletionAuthority) {
    PDCompletionAuthorityPhysicalPrinter = 0,
    PDCompletionAuthorityVendorSpooler,
    PDCompletionAuthorityPdAgent,
    PDCompletionAuthorityPrintServer,
    PDCompletionAuthorityTransportOnly,
};

typedef NS_ENUM(NSInteger, PDDeviceEvent) {
    PDDeviceEventOnline = 0,
    PDDeviceEventOffline,
    PDDeviceEventCoverOpen,
    PDDeviceEventCoverClosed,
    PDDeviceEventPaperOut,
    PDDeviceEventPaperNearEnd,
    PDDeviceEventPaperOk,
    PDDeviceEventCutterError,
    PDDeviceEventRecoverableError,
    PDDeviceEventUnrecoverableError,
    PDDeviceEventConnectionLost,
    PDDeviceEventConnectionRestored,
};

#pragma mark - Document ops (docs/api.md §3 tier 2)

typedef NS_ENUM(NSInteger, PDAlignment) {
    PDAlignmentLeft = 0,
    PDAlignmentCenter = 1,
    PDAlignmentRight = 2,
};

typedef NS_ENUM(NSInteger, PDOpKind) {
    /// One line of text plus the style it is printed with.
    PDOpKindText = 0,
    /// ESC d n.
    PDOpKindFeed,
    /// GS ( k, functions 165/167/169/180/181.
    PDOpKindQR,
};

/// One designer block, flattened for the encoder. Columns and dividers never reach
/// the bridge as their own kind: the app renders them into padded text lines using
/// the printer's characters-per-line, because column geometry is a layout decision
/// that belongs where the preview is drawn, not in the byte encoder.
@interface PDOp : NSObject

@property (nonatomic, assign) PDOpKind kind;

/// Text content, or the QR payload.
@property (nonatomic, copy) NSString *text;

@property (nonatomic, assign) PDAlignment alignment;
@property (nonatomic, assign) BOOL bold;
/// 0 = off, 1 = 1-dot, 2 = 2-dot (ESC - n).
@property (nonatomic, assign) uint8_t underline;
/// GS B n. Not wrapped by the core encoder, emitted as a raw command by the bridge.
@property (nonatomic, assign) BOOL inverse;
/// GS ! multipliers, 1...8.
@property (nonatomic, assign) uint8_t widthScale;
@property (nonatomic, assign) uint8_t heightScale;

/// PDOpKindFeed only.
@property (nonatomic, assign) uint8_t feedLines;

/// PDOpKindQR only. Module size 1...16, error correction "L" | "M" | "Q" | "H".
@property (nonatomic, assign) uint8_t qrModuleSize;
@property (nonatomic, copy) NSString *qrErrorCorrection;

+ (instancetype)textOp:(NSString *)text;
+ (instancetype)feedOp:(uint8_t)lines;
+ (instancetype)qrOp:(NSString *)payload
          moduleSize:(uint8_t)moduleSize
     errorCorrection:(NSString *)errorCorrection
           alignment:(PDAlignment)alignment;

@end

#pragma mark - Value types handed back to Swift

/// A status snapshot. Every flag is an NSNumber-or-nil because "not observed" and
/// "observed false" are different answers and the UI has to be able to say so.
NS_SWIFT_SENDABLE
@interface PDDeviceStatus : NSObject
@property (nonatomic, readonly, assign) BOOL connected;
/// NO until a status query or ASB frame has actually been decoded. A snapshot that
/// has never heard from the device says so rather than reporting healthy.
@property (nonatomic, readonly, assign) BOOL observed;
@property (nonatomic, readonly, strong, nullable) NSNumber *online;
@property (nonatomic, readonly, strong, nullable) NSNumber *coverOpen;
@property (nonatomic, readonly, strong, nullable) NSNumber *paperOut;
@property (nonatomic, readonly, strong, nullable) NSNumber *paperNearEnd;
@property (nonatomic, readonly, strong, nullable) NSNumber *cutterError;
@property (nonatomic, readonly, strong, nullable) NSNumber *unrecoverableError;
@property (nonatomic, readonly, strong, nullable) NSNumber *recoverableError;
@end

NS_SWIFT_SENDABLE
@interface PDJobEvent : NSObject
@property (nonatomic, readonly, assign) PDJobState state;
@property (nonatomic, readonly, assign) PDConfidenceLevel confidence;
/// PDFailureReasonNone when the event carried no reason.
@property (nonatomic, readonly, assign) PDFailureReason reason;
@property (nonatomic, readonly, copy) NSString *stateName;
@property (nonatomic, readonly, copy) NSString *confidenceName;
@end

NS_SWIFT_SENDABLE
@interface PDJobResult : NSObject
@property (nonatomic, readonly, assign) PDJobOutcome outcome;
@property (nonatomic, readonly, assign) PDConfidenceLevel confidence;
@property (nonatomic, readonly, assign) PDFailureReason reason;
@property (nonatomic, readonly, assign) PDConfidenceGrade grade;
@property (nonatomic, readonly, assign) PDCompletionAuthority authority;
/// The actual command that produced the evidence, e.g. "GS(H) fn48".
@property (nonatomic, readonly, copy) NSString *method;
@property (nonatomic, readonly, copy) NSString *outcomeName;
@property (nonatomic, readonly, copy) NSString *confidenceName;
@property (nonatomic, readonly, copy) NSString *reasonName;
@property (nonatomic, readonly, copy) NSString *gradeLetter;
@property (nonatomic, readonly, copy) NSString *gradeName;
@property (nonatomic, readonly, copy) NSString *authorityName;
@property (nonatomic, readonly, copy) NSString *jobId;
@property (nonatomic, readonly, copy) NSString *jobKey;
@end

/// The result of an identity probe. Every field is what the device *reported*,
/// never what it is: GS I can lie (docs/capability-profiles.md §5).
NS_SWIFT_SENDABLE
@interface PDIdentity : NSObject
@property (nonatomic, readonly, copy) NSString *vendorGuess;
@property (nonatomic, readonly, copy) NSString *profileGuess;
@property (nonatomic, readonly, assign) NSInteger confidencePercent;
/// Only ever YES when a signal independent of GS I agrees with GS I.
@property (nonatomic, readonly, assign) BOOL identityTrusted;
@property (nonatomic, readonly, assign) BOOL impersonationSuspected;
@property (nonatomic, readonly, copy) NSString *reportedManufacturer;
@property (nonatomic, readonly, copy) NSString *reportedModel;
@property (nonatomic, readonly, copy) NSString *firmware;
@property (nonatomic, readonly, copy) NSString *serial;
@property (nonatomic, readonly, copy) NSString *ouiVendor;
/// Human-readable reasons behind the guess, in report order.
@property (nonatomic, readonly, copy) NSArray<NSString *> *signals;
/// What the probe established first-hand about the completion fence.
@property (nonatomic, readonly, copy) NSString *completionMechanism;
@property (nonatomic, readonly, copy) NSString *profileName;
@property (nonatomic, readonly, strong, nullable) NSNumber *supportsProcessIdMarker;
@property (nonatomic, readonly, strong, nullable) NSNumber *supportsQueuedPaperStatus;
@property (nonatomic, readonly, strong, nullable) NSNumber *supportsRealtimeStatus;
@end

#pragma mark - Bridge

typedef void (^PDJobProgressBlock)(PDJobEvent *event);
typedef void (^PDJobCompletionBlock)(PDJobResult *result);
typedef void (^PDStatusBlock)(PDDeviceStatus *status);
typedef void (^PDIdentityBlock)(PDIdentity *_Nullable identity, NSString *_Nullable error);
typedef void (^PDDeviceEventBlock)(NSString *printerId, PDDeviceEvent event);

/// One driver for the whole app. The store directory is Documents/PrinterDriver,
/// so the job journal survives app restarts and crash recovery can resurface a job
/// that was in flight as Unknown (docs/api.md §4).
@interface PDBridge : NSObject

+ (instancetype)shared;

/// Storage directory actually in use, for display.
@property (nonatomic, readonly, copy) NSString *storeDirectory;

/// Names in the shipped device database (docs/device-database.md), e.g. "generic_80".
+ (NSArray<NSString *> *)profileNames;

/// Adds (or returns the existing) printer. `profileId` is a device-database name;
/// pass nil or an unknown name for the conservative generic profile matching the
/// width. Returns the stable printer id used by every other call.
- (NSString *)addPrinterWithHost:(NSString *)host
                            port:(uint16_t)port
                       widthDots:(uint32_t)widthDots
                       profileId:(nullable NSString *)profileId
    NS_SWIFT_NAME(addPrinter(host:port:widthDots:profileId:));

/// Submits a document. `key` is the idempotency key: re-submitting a key that this
/// driver already knows prints nothing and re-reports the existing job's outcome.
/// `progress` receives every JobEvent in order; `completion` fires exactly once,
/// with a terminal result. Both are invoked on the main queue.
- (void)printOps:(NSArray<PDOp *> *)ops
       printerId:(NSString *)printerId
  idempotencyKey:(NSString *)key
        progress:(nullable PDJobProgressBlock)progress
      completion:(PDJobCompletionBlock)completion
    NS_SWIFT_NAME(submit(ops:printerId:key:progress:completion:));

/// Deliberate duplicate of a job whose outcome was Unknown. Prepends
/// *** REPRINT / POSSIBLE DUPLICATE *** and the attempt counter.
- (void)forceReprintOps:(NSArray<PDOp *> *)ops
              printerId:(NSString *)printerId
         idempotencyKey:(NSString *)key
               progress:(nullable PDJobProgressBlock)progress
             completion:(PDJobCompletionBlock)completion
    NS_SWIFT_NAME(forceReprint(ops:printerId:key:progress:completion:));

/// DLE EOT 1-4 round trip, queued behind any active job. Answers on the main queue.
- (void)statusForPrinterId:(NSString *)printerId
                completion:(PDStatusBlock)completion
    NS_SWIFT_NAME(refreshStatus(printerId:completion:));

/// Last known snapshot; never a live query, so it cannot block behind a print.
- (PDDeviceStatus *)lastKnownStatusForPrinterId:(NSString *)printerId
    NS_SWIFT_NAME(lastKnownStatus(printerId:));

/// Runs the non-destructive capability probe against an address that need not be a
/// configured printer. Never sends a cut, DLE ENQ or DLE DC4.
- (void)identifyHost:(NSString *)host
                port:(uint16_t)port
                 mac:(nullable NSString *)mac
          completion:(PDIdentityBlock)completion
    NS_SWIFT_NAME(identify(host:port:mac:completion:));

/// Per-printer device events (online/offline, paper, cover, cutter). Main queue.
- (void)subscribeDeviceEvents:(PDDeviceEventBlock)block
    NS_SWIFT_NAME(subscribeDeviceEvents(_:));

@end

NS_ASSUME_NONNULL_END
