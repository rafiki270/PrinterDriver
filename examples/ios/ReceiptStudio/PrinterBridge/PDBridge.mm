//
//  PDBridge.mm
//  ReceiptStudio
//
//  Objective-C++ over the PrinterDriver core. Thin by design: enum bridging, a
//  document-op encoder, and thread hops. No logic that decides anything about a
//  job's outcome lives here — that is the core's job, and duplicating it in a
//  wrapper is how wrappers start disagreeing with each other (docs/api.md §6).
//

#import "PDBridge.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "printerdriver/capability_probe.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/identity.hpp"
#include "printerdriver/transport.hpp"
#include "printerdriver/types.hpp"

#pragma mark - Enum bridging

// Written as switches rather than casts so that adding a core enum member breaks
// this file at compile time instead of silently arriving in Swift as a wrong case.

static PDJobState PDJobStateFrom(pd::JobState state) {
    switch (state) {
        case pd::JobState::Queued:              return PDJobStateQueued;
        case pd::JobState::PreflightOk:         return PDJobStatePreflightOk;
        case pd::JobState::SendStarted:         return PDJobStateSendStarted;
        case pd::JobState::BytesSent:           return PDJobStateBytesSent;
        case pd::JobState::PrintConfirmed:      return PDJobStatePrintConfirmed;
        case pd::JobState::CutCommandProcessed: return PDJobStateCutCommandProcessed;
        case pd::JobState::DoneSoftware:        return PDJobStateDoneSoftware;
        case pd::JobState::PhysicallyVerified:  return PDJobStatePhysicallyVerified;
        case pd::JobState::FailedKnown:         return PDJobStateFailedKnown;
        case pd::JobState::Unknown:             return PDJobStateUnknown;
        case pd::JobState::HeldOffline:         return PDJobStateHeldOffline;
    }
    return PDJobStateUnknown;
}

static PDConfidenceLevel PDConfidenceLevelFrom(pd::ConfidenceLevel level) {
    switch (level) {
        case pd::ConfidenceLevel::TransportAccepted:  return PDConfidenceLevelTransportAccepted;
        case pd::ConfidenceLevel::PrinterHealthy:     return PDConfidenceLevelPrinterHealthy;
        case pd::ConfidenceLevel::PrintConfirmed:     return PDConfidenceLevelPrintConfirmed;
        case pd::ConfidenceLevel::CutProcessed:       return PDConfidenceLevelCutProcessed;
        case pd::ConfidenceLevel::CutFaultFree:       return PDConfidenceLevelCutFaultFree;
        case pd::ConfidenceLevel::PhysicallyVerified: return PDConfidenceLevelPhysicallyVerified;
    }
    return PDConfidenceLevelTransportAccepted;
}

static PDFailureReason PDFailureReasonFrom(pd::FailureReason reason) {
    switch (reason) {
        case pd::FailureReason::None:                      return PDFailureReasonNone;
        case pd::FailureReason::TransportUnreachable:      return PDFailureReasonTransportUnreachable;
        case pd::FailureReason::PreflightCoverOpen:        return PDFailureReasonPreflightCoverOpen;
        case pd::FailureReason::PreflightPaperOut:         return PDFailureReasonPreflightPaperOut;
        case pd::FailureReason::PreflightHardwareError:    return PDFailureReasonPreflightHardwareError;
        case pd::FailureReason::TimeoutAwaitingCompletion: return PDFailureReasonTimeoutAwaitingCompletion;
        case pd::FailureReason::CutterFault:               return PDFailureReasonCutterFault;
        case pd::FailureReason::Unsupported:               return PDFailureReasonUnsupported;
        case pd::FailureReason::Unknown:                   return PDFailureReasonUnknown;
        case pd::FailureReason::Expired:                   return PDFailureReasonExpired;
        case pd::FailureReason::QueueOverflow:             return PDFailureReasonQueueOverflow;
    }
    return PDFailureReasonUnknown;
}

static PDJobOutcome PDJobOutcomeFrom(pd::JobOutcome outcome) {
    switch (outcome) {
        case pd::JobOutcome::Done:    return PDJobOutcomeDone;
        case pd::JobOutcome::Failed:  return PDJobOutcomeFailed;
        case pd::JobOutcome::Unknown: return PDJobOutcomeUnknown;
    }
    return PDJobOutcomeUnknown;
}

static PDConfidenceGrade PDConfidenceGradeFrom(pd::ConfidenceGrade grade) {
    switch (grade) {
        case pd::ConfidenceGrade::A_JobLevelConfirmation:  return PDConfidenceGradeAJobLevelConfirmation;
        case pd::ConfidenceGrade::B_OrderedDeviceResponse: return PDConfidenceGradeBOrderedDeviceResponse;
        case pd::ConfidenceGrade::C_DeviceStatusAround:    return PDConfidenceGradeCDeviceStatusAround;
        case pd::ConfidenceGrade::D_SpoolerCompleted:      return PDConfidenceGradeDSpoolerCompleted;
        case pd::ConfidenceGrade::E_TransportOnly:         return PDConfidenceGradeETransportOnly;
    }
    return PDConfidenceGradeETransportOnly;
}

static PDCompletionAuthority PDCompletionAuthorityFrom(pd::CompletionAuthority authority) {
    switch (authority) {
        case pd::CompletionAuthority::PhysicalPrinter: return PDCompletionAuthorityPhysicalPrinter;
        case pd::CompletionAuthority::VendorSpooler:   return PDCompletionAuthorityVendorSpooler;
        case pd::CompletionAuthority::PdAgent:         return PDCompletionAuthorityPdAgent;
        case pd::CompletionAuthority::PrintServer:     return PDCompletionAuthorityPrintServer;
        case pd::CompletionAuthority::TransportOnly:   return PDCompletionAuthorityTransportOnly;
    }
    return PDCompletionAuthorityTransportOnly;
}

static PDDeviceEvent PDDeviceEventFrom(pd::DeviceEvent event) {
    switch (event) {
        case pd::DeviceEvent::Online:              return PDDeviceEventOnline;
        case pd::DeviceEvent::Offline:             return PDDeviceEventOffline;
        case pd::DeviceEvent::CoverOpen:           return PDDeviceEventCoverOpen;
        case pd::DeviceEvent::CoverClosed:         return PDDeviceEventCoverClosed;
        case pd::DeviceEvent::PaperOut:            return PDDeviceEventPaperOut;
        case pd::DeviceEvent::PaperNearEnd:        return PDDeviceEventPaperNearEnd;
        case pd::DeviceEvent::PaperOk:             return PDDeviceEventPaperOk;
        case pd::DeviceEvent::CutterError:         return PDDeviceEventCutterError;
        case pd::DeviceEvent::RecoverableError:    return PDDeviceEventRecoverableError;
        case pd::DeviceEvent::UnrecoverableError:  return PDDeviceEventUnrecoverableError;
        case pd::DeviceEvent::ConnectionLost:      return PDDeviceEventConnectionLost;
        case pd::DeviceEvent::ConnectionRestored:  return PDDeviceEventConnectionRestored;
        case pd::DeviceEvent::ForeignWriterDetected:
            return PDDeviceEventForeignWriterDetected;
    }
    return PDDeviceEventOffline;
}

static NSString *PDString(const std::string &value) {
    return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

static NSString *PDString(const char *value) {
    return value != nullptr ? ([NSString stringWithUTF8String:value] ?: @"") : @"";
}

static NSNumber *_Nullable PDBoolNumber(const std::optional<bool> &value) {
    return value.has_value() ? @(*value) : nil;
}

#pragma mark - PDOp

@implementation PDOp

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _kind = PDOpKindText;
        _text = @"";
        _alignment = PDAlignmentLeft;
        _bold = NO;
        _underline = 0;
        _inverse = NO;
        _widthScale = 1;
        _heightScale = 1;
        _feedLines = 1;
        _qrModuleSize = 4;
        _qrErrorCorrection = @"M";
    }
    return self;
}

+ (instancetype)textOp:(NSString *)text {
    PDOp *op = [[PDOp alloc] init];
    op.kind = PDOpKindText;
    op.text = text ?: @"";
    return op;
}

+ (instancetype)feedOp:(uint8_t)lines {
    PDOp *op = [[PDOp alloc] init];
    op.kind = PDOpKindFeed;
    op.feedLines = lines;
    return op;
}

+ (instancetype)qrOp:(NSString *)payload
          moduleSize:(uint8_t)moduleSize
     errorCorrection:(NSString *)errorCorrection
           alignment:(PDAlignment)alignment {
    PDOp *op = [[PDOp alloc] init];
    op.kind = PDOpKindQR;
    op.text = payload ?: @"";
    op.qrModuleSize = moduleSize;
    op.qrErrorCorrection = errorCorrection ?: @"M";
    op.alignment = alignment;
    return op;
}

@end

#pragma mark - Value types
//
// Immutable to everyone outside this file: the public header declares them
// readonly and NS_SWIFT_SENDABLE, which is only honest because the only code that
// ever fills one in is the constructor below, on the thread that created it, before
// it is handed to the main queue. These extensions are how that stays true.

@interface PDDeviceStatus ()
@property (nonatomic, assign) BOOL connected;
@property (nonatomic, assign) BOOL observed;
@property (nonatomic, strong, nullable) NSNumber *online;
@property (nonatomic, strong, nullable) NSNumber *coverOpen;
@property (nonatomic, strong, nullable) NSNumber *paperOut;
@property (nonatomic, strong, nullable) NSNumber *paperNearEnd;
@property (nonatomic, strong, nullable) NSNumber *cutterError;
@property (nonatomic, strong, nullable) NSNumber *unrecoverableError;
@property (nonatomic, strong, nullable) NSNumber *recoverableError;
@end

@interface PDJobEvent ()
@property (nonatomic, assign) PDJobState state;
@property (nonatomic, assign) PDConfidenceLevel confidence;
@property (nonatomic, assign) PDFailureReason reason;
@property (nonatomic, copy) NSString *stateName;
@property (nonatomic, copy) NSString *confidenceName;
@end

@interface PDJobResult ()
@property (nonatomic, assign) PDJobOutcome outcome;
@property (nonatomic, assign) PDConfidenceLevel confidence;
@property (nonatomic, assign) PDFailureReason reason;
@property (nonatomic, assign) PDConfidenceGrade grade;
@property (nonatomic, assign) PDCompletionAuthority authority;
@property (nonatomic, copy) NSString *method;
@property (nonatomic, copy) NSString *outcomeName;
@property (nonatomic, copy) NSString *confidenceName;
@property (nonatomic, copy) NSString *reasonName;
@property (nonatomic, copy) NSString *gradeLetter;
@property (nonatomic, copy) NSString *gradeName;
@property (nonatomic, copy) NSString *authorityName;
@property (nonatomic, copy) NSString *jobId;
@property (nonatomic, copy) NSString *jobKey;
@end

@interface PDIdentity ()
@property (nonatomic, copy) NSString *vendorGuess;
@property (nonatomic, copy) NSString *profileGuess;
@property (nonatomic, assign) NSInteger confidencePercent;
@property (nonatomic, assign) BOOL identityTrusted;
@property (nonatomic, assign) BOOL impersonationSuspected;
@property (nonatomic, copy) NSString *reportedManufacturer;
@property (nonatomic, copy) NSString *reportedModel;
@property (nonatomic, copy) NSString *firmware;
@property (nonatomic, copy) NSString *serial;
@property (nonatomic, copy) NSString *ouiVendor;
@property (nonatomic, copy) NSArray<NSString *> *signals;
@property (nonatomic, copy) NSString *completionMechanism;
@property (nonatomic, copy) NSString *profileName;
@property (nonatomic, strong, nullable) NSNumber *supportsProcessIdMarker;
@property (nonatomic, strong, nullable) NSNumber *supportsQueuedPaperStatus;
@property (nonatomic, strong, nullable) NSNumber *supportsRealtimeStatus;
@end

@implementation PDDeviceStatus
@end

@implementation PDJobEvent

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _stateName = @"";
        _confidenceName = @"";
    }
    return self;
}

@end

@implementation PDJobResult

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _method = @"none";
        _outcomeName = @"";
        _confidenceName = @"";
        _reasonName = @"";
        _gradeLetter = @"E";
        _gradeName = @"";
        _authorityName = @"";
        _jobId = @"";
        _jobKey = @"";
    }
    return self;
}

@end

@implementation PDIdentity

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _vendorGuess = @"Unknown";
        _profileGuess = @"generic_80";
        _reportedManufacturer = @"";
        _reportedModel = @"";
        _firmware = @"";
        _serial = @"";
        _ouiVendor = @"";
        _signals = @[];
        _completionMechanism = @"";
        _profileName = @"";
    }
    return self;
}

@end

#pragma mark - Encoding designer ops into a document payload

static pd::escpos::Alignment PDCoreAlignment(PDAlignment alignment) {
    switch (alignment) {
        case PDAlignmentLeft:   return pd::escpos::Alignment::Left;
        case PDAlignmentCenter: return pd::escpos::Alignment::Center;
        case PDAlignmentRight:  return pd::escpos::Alignment::Right;
    }
    return pd::escpos::Alignment::Left;
}

static pd::escpos::QrErrorCorrection PDCoreErrorCorrection(NSString *level) {
    NSString *normalized = [(level ?: @"M") uppercaseString];
    if ([normalized isEqualToString:@"L"]) return pd::escpos::QrErrorCorrection::L;
    if ([normalized isEqualToString:@"Q"]) return pd::escpos::QrErrorCorrection::Q;
    if ([normalized isEqualToString:@"H"]) return pd::escpos::QrErrorCorrection::H;
    return pd::escpos::QrErrorCorrection::M;
}

static uint8_t PDClampScale(uint8_t value) {
    if (value < 1) return 1;
    if (value > 8) return 8;
    return value;
}

/// GS B n — white-on-black. The core encoder does not wrap it (it tracks only the
/// style state it owns), so it goes out as a raw command and is always switched off
/// again on the same line. Emitting it here rather than adding it to the encoder
/// keeps the wrapper from growing a private dialect of ESC/POS.
static void PDSetInverse(pd::escpos::Encoder &encoder, bool enabled) {
    const uint8_t bytes[3] = {0x1D, 0x42, static_cast<uint8_t>(enabled ? 1 : 0)};
    encoder.raw(bytes, sizeof(bytes));
}

static pd::escpos::Encoder PDEncodeOps(NSArray<PDOp *> *ops, pd::escpos::CodePage codePage) {
    pd::escpos::Encoder encoder;
    encoder.selectCodePage(codePage);

    bool inverseActive = false;
    for (PDOp *op in ops) {
        switch (op.kind) {
            case PDOpKindText: {
                encoder.align(PDCoreAlignment(op.alignment));
                encoder.bold(op.bold);
                encoder.underline(op.underline);
                encoder.textSize(PDClampScale(op.widthScale), PDClampScale(op.heightScale));
                if (op.inverse != inverseActive) {
                    PDSetInverse(encoder, op.inverse);
                    inverseActive = op.inverse;
                }
                encoder.line(op.text.UTF8String ?: "");
                break;
            }
            case PDOpKindFeed: {
                if (inverseActive) {
                    PDSetInverse(encoder, false);
                    inverseActive = false;
                }
                const uint8_t lines = op.feedLines == 0 ? 1 : op.feedLines;
                encoder.feedLines(lines);
                break;
            }
            case PDOpKindQR: {
                if (inverseActive) {
                    PDSetInverse(encoder, false);
                    inverseActive = false;
                }
                encoder.align(PDCoreAlignment(op.alignment));
                uint8_t moduleSize = op.qrModuleSize == 0 ? 4 : op.qrModuleSize;
                if (moduleSize > 16) {
                    moduleSize = 16;
                }
                encoder.qr(op.text.UTF8String ?: "", moduleSize,
                           PDCoreErrorCorrection(op.qrErrorCorrection));
                break;
            }
        }
    }

    if (inverseActive) {
        PDSetInverse(encoder, false);
    }
    // Leave the printer in the documented default style: the core prepends its own
    // ESC @ per job, but a document that ends double-height would still surprise
    // anything reading the captured stream.
    encoder.bold(false).underline(0).textSize(1, 1).align(pd::escpos::Alignment::Left);
    return encoder;
}

#pragma mark - Value-type construction
//
// Free functions rather than methods so the C++ lambdas below never have to capture
// an Objective-C object: a subscriber lambda outlives the call that installed it, and
// a captured `self` in it is a lifetime question nobody should have to answer.

static PDJobEvent *PDMakeJobEvent(const pd::JobEvent &event) {
    PDJobEvent *bridged = [[PDJobEvent alloc] init];
    bridged.state = PDJobStateFrom(event.state);
    bridged.confidence = PDConfidenceLevelFrom(event.confidence);
    bridged.reason = event.reason.has_value() ? PDFailureReasonFrom(*event.reason)
                                              : PDFailureReasonNone;
    bridged.stateName = PDString(pd::to_string(event.state));
    bridged.confidenceName = PDString(pd::to_string(event.confidence));
    return bridged;
}

static PDJobResult *PDMakeJobResult(const pd::JobResult &result, const pd::PrintJob &job) {
    PDJobResult *bridged = [[PDJobResult alloc] init];
    bridged.outcome = PDJobOutcomeFrom(result.outcome);
    bridged.confidence = PDConfidenceLevelFrom(result.confidence);
    bridged.reason = PDFailureReasonFrom(result.reason);
    bridged.grade = PDConfidenceGradeFrom(result.grade);
    bridged.authority = PDCompletionAuthorityFrom(result.authority);
    bridged.method = PDString(result.method);
    bridged.outcomeName = PDString(pd::to_string(result.outcome));
    bridged.confidenceName = PDString(pd::to_string(result.confidence));
    bridged.reasonName = PDString(pd::to_string(result.reason));
    bridged.gradeLetter = PDString(pd::gradeLetter(result.grade));
    bridged.gradeName = PDString(pd::to_string(result.grade));
    bridged.authorityName = PDString(pd::to_string(result.authority));
    bridged.jobId = PDString(job.id());
    bridged.jobKey = PDString(job.key());
    return bridged;
}

static PDDeviceStatus *PDMakeDeviceStatus(const pd::DeviceStatus &status) {
    PDDeviceStatus *bridged = [[PDDeviceStatus alloc] init];
    bridged.connected = status.connected;
    bridged.observed = status.observed;
    bridged.online = PDBoolNumber(status.online);
    bridged.coverOpen = PDBoolNumber(status.cover_open);
    bridged.paperOut = PDBoolNumber(status.paper_out);
    bridged.paperNearEnd = PDBoolNumber(status.paper_near_end);
    bridged.cutterError = PDBoolNumber(status.cutter_error);
    bridged.unrecoverableError = PDBoolNumber(status.unrecoverable_error);
    bridged.recoverableError = PDBoolNumber(status.recoverable_error);
    return bridged;
}

#pragma mark - PDBridge

@interface PDBridge () {
    std::shared_ptr<pd::PrinterDriver> _driver;
}

@property (nonatomic, strong) dispatch_queue_t workQueue;
@property (nonatomic, copy) NSString *storeDirectory;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSNumber *> *widthByPrinterId;

@end

@implementation PDBridge

+ (instancetype)shared {
    static PDBridge *shared = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        shared = [[PDBridge alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        NSString *documents = NSSearchPathForDirectoriesInDomains(
            NSDocumentDirectory, NSUserDomainMask, YES).firstObject ?: NSTemporaryDirectory();
        NSString *store = [documents stringByAppendingPathComponent:@"PrinterDriver"];
        [[NSFileManager defaultManager] createDirectoryAtPath:store
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        _storeDirectory = store;
        _widthByPrinterId = [NSMutableDictionary dictionary];
        // Concurrent: a status refresh or a probe must not sit behind a print that
        // is waiting on a completion fence.
        _workQueue = dispatch_queue_create("com.printerdriver.receiptstudio.bridge",
                                           DISPATCH_QUEUE_CONCURRENT);
        _driver = std::make_shared<pd::PrinterDriver>(
            pd::StorageConfig::at(std::string(store.UTF8String)));
    }
    return self;
}

+ (NSArray<NSString *> *)profileNames {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (const std::string &name : pd::devices::names()) {
        [names addObject:PDString(name)];
    }
    return names;
}

- (pd::CapabilityProfile)profileForId:(NSString *)profileId widthDots:(uint32_t)widthDots {
    if (profileId.length > 0) {
        const std::string name(profileId.UTF8String);
        if (pd::devices::exists(name)) {
            return pd::devices::byName(name);
        }
        if (name == "xp_s260m" || name == "xp-s260m") {
            return pd::xp_s260m();
        }
    }
    // docs/capability-profiles.md §8: generic is not a profile, it is UNKNOWN DEVICE.
    return widthDots <= pd::escpos::kWidth58mm ? pd::devices::generic_58()
                                               : pd::devices::generic_80();
}

- (NSString *)addPrinterWithHost:(NSString *)host
                            port:(uint16_t)port
                       widthDots:(uint32_t)widthDots
                       profileId:(NSString *)profileId {
    const std::string hostValue(host.UTF8String ?: "");
    const uint16_t resolvedPort = port == 0 ? 9100 : port;
    NSString *printerId = [NSString stringWithFormat:@"%@:%u", host, (unsigned)resolvedPort];

    if (auto existing = _driver->printer(std::string(printerId.UTF8String))) {
        return printerId;
    }

    pd::PrinterConfig config;
    config.id = std::string(printerId.UTF8String);
    config.transport = pd::tcp(hostValue, resolvedPort, 3000);
    config.width_dots = widthDots;
    config.profile = [self profileForId:profileId widthDots:widthDots];
    // Apply anything a previous probe established about this device, but never
    // interrogate it as a side effect of adding it: the Identify button is where an
    // operator asks for that (docs/capability-profiles.md).
    config.probe = pd::ProbePolicy::UseStored;
    config.probe_options.endpoint = hostValue + ":" + std::to_string(resolvedPort);

    _driver->addPrinter(config);
    @synchronized (self.widthByPrinterId) {
        self.widthByPrinterId[printerId] = @(widthDots);
    }
    return printerId;
}

- (PDJobResult *)transportFailureResultWithKey:(NSString *)key {
    PDJobResult *result = [[PDJobResult alloc] init];
    result.outcome = PDJobOutcomeFailed;
    result.reason = PDFailureReasonTransportUnreachable;
    result.outcomeName = @"Failed";
    result.reasonName = @"TransportUnreachable";
    result.confidenceName = @"TransportAccepted";
    result.gradeName = @"E_TransportOnly";
    result.authorityName = @"TransportOnly";
    result.jobKey = key ?: @"";
    return result;
}

/// Subscribes to the job's event stream and, on a separate waiter, to its terminal
/// result. They are separate on purpose: the core calls subscribers while it still
/// holds the job's terminal transition open, so asking for result() from inside a
/// subscriber callback would wait on a thread that is waiting for it to return.
- (void)observeJob:(std::shared_ptr<pd::PrintJob>)job
          progress:(PDJobProgressBlock)progress
        completion:(PDJobCompletionBlock)completion {
    if (progress != nil) {
        job->subscribe([progress](const pd::JobEvent &event) {
            // Core worker thread → main queue. Every UI callback in this bridge
            // crosses here and nowhere else.
            PDJobEvent *bridged = PDMakeJobEvent(event);
            dispatch_async(dispatch_get_main_queue(), ^{
                progress(bridged);
            });
        });
    }

    dispatch_async(self.workQueue, ^{
        const pd::JobResult result = job->result();
        PDJobResult *bridged = PDMakeJobResult(result, *job);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(bridged);
        });
    });
}

- (void)submitOps:(NSArray<PDOp *> *)ops
        printerId:(NSString *)printerId
   idempotencyKey:(NSString *)key
     forceReprint:(BOOL)forceReprint
         progress:(PDJobProgressBlock)progress
       completion:(PDJobCompletionBlock)completion {
    auto printer = _driver->printer(std::string(printerId.UTF8String ?: ""));
    if (!printer) {
        PDJobResult *result = [self transportFailureResultWithKey:key];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(result);
        });
        return;
    }

    const pd::CapabilityProfile profile = printer->profile();
    pd::escpos::Encoder encoder = PDEncodeOps(ops, profile.code_page);

    pd::JobOptions options;
    options.key = std::string(key.UTF8String ?: "");
    options.cut = pd::CutSetting::Profile;
    options.preflight = pd::PreflightMode::Strict;

    auto payload = pd::Payload::document(encoder);
    std::shared_ptr<pd::PrintJob> job =
        forceReprint ? printer->forceReprint(options.key, payload, options)
                     : printer->print(payload, options);
    if (!job) {
        PDJobResult *result = [self transportFailureResultWithKey:key];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(result);
        });
        return;
    }
    [self observeJob:job progress:progress completion:completion];
}

- (void)printOps:(NSArray<PDOp *> *)ops
       printerId:(NSString *)printerId
  idempotencyKey:(NSString *)key
        progress:(PDJobProgressBlock)progress
      completion:(PDJobCompletionBlock)completion {
    [self submitOps:ops
          printerId:printerId
     idempotencyKey:key
       forceReprint:NO
           progress:progress
         completion:completion];
}

- (void)forceReprintOps:(NSArray<PDOp *> *)ops
              printerId:(NSString *)printerId
         idempotencyKey:(NSString *)key
               progress:(PDJobProgressBlock)progress
             completion:(PDJobCompletionBlock)completion {
    [self submitOps:ops
          printerId:printerId
     idempotencyKey:key
       forceReprint:YES
           progress:progress
         completion:completion];
}

- (void)statusForPrinterId:(NSString *)printerId completion:(PDStatusBlock)completion {
    auto printer = _driver->printer(std::string(printerId.UTF8String ?: ""));
    if (!printer) {
        PDDeviceStatus *empty = [[PDDeviceStatus alloc] init];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(empty);
        });
        return;
    }
    dispatch_async(self.workQueue, ^{
        // Queued behind any active job, which is why it cannot run on the caller's
        // thread: a status poll must never be able to stall a print.
        const pd::DeviceStatus status = printer->refreshStatus(std::chrono::milliseconds(2500));
        PDDeviceStatus *bridged = PDMakeDeviceStatus(status);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(bridged);
        });
    });
}

- (PDDeviceStatus *)lastKnownStatusForPrinterId:(NSString *)printerId {
    auto printer = _driver->printer(std::string(printerId.UTF8String ?: ""));
    if (!printer) {
        return [[PDDeviceStatus alloc] init];
    }
    return PDMakeDeviceStatus(printer->status());
}

- (void)identifyHost:(NSString *)host
                port:(uint16_t)port
                 mac:(NSString *)mac
          completion:(PDIdentityBlock)completion {
    const std::string hostValue(host.UTF8String ?: "");
    const uint16_t resolvedPort = port == 0 ? 9100 : port;
    const std::string macValue(mac.UTF8String ?: "");

    dispatch_async(self.workQueue, ^{
        pd::TcpConfig config;
        config.host = hostValue;
        config.port = resolvedPort;
        pd::TcpTransport transport(config);

        pd::ProbeOptions options;
        options.endpoint = transport.describe();
        // Two short lines: the ordered fences only mean anything when there is print
        // data ahead of them. The probe never sends a cut.
        options.print_test_lines = true;
        options.hints.mac = macValue;
        pd::CapabilityProbe probe(options);
        transport.onBytes([&probe](const uint8_t *data, size_t size) {
            probe.onBytes(data, size);
        });
        transport.onDisconnected([](pd::TransportError, const std::string &) {});

        const pd::TransportResult connected = transport.connect();
        if (!connected.ok) {
            NSString *message = PDString(connected.message);
            dispatch_async(dispatch_get_main_queue(), ^{
                completion(nil, message.length > 0 ? message : @"connect failed");
            });
            return;
        }

        const pd::CapabilityFindings findings =
            probe.run([&transport](const pd::escpos::Bytes &bytes) {
                return transport.write(bytes).ok;
            });
        transport.close();

        const pd::IdentityAssessment assessment =
            pd::identify(findings.reported, pd::IdentityHints{macValue, ""},
                         findings.behaviour());
        // A guess the database does not carry is still a guess; fall back to the
        // conservative generic entry rather than letting the lookup throw.
        const pd::CapabilityProfile defaults =
            pd::devices::exists(assessment.profile_guess)
                ? pd::devices::byName(assessment.profile_guess)
                : pd::devices::generic_80();
        const pd::CapabilityProfile profile = pd::promote(defaults, findings);

        PDIdentity *identity = [[PDIdentity alloc] init];
        identity.vendorGuess = PDString(assessment.vendor_guess);
        identity.profileGuess = PDString(assessment.profile_guess);
        identity.confidencePercent = assessment.confidence_percent;
        identity.identityTrusted = assessment.identity_trusted;
        identity.impersonationSuspected = assessment.impersonation_suspected;
        identity.reportedManufacturer = PDString(assessment.reported_manufacturer);
        identity.reportedModel = PDString(assessment.reported_model);
        identity.firmware = PDString(assessment.firmware);
        identity.serial = PDString(assessment.serial);
        identity.ouiVendor = PDString(assessment.oui_vendor);
        identity.completionMechanism = PDString(pd::to_string(profile.completion));
        identity.profileName = PDString(profile.name);
        identity.supportsProcessIdMarker = PDBoolNumber(findings.gs_h_process_id);
        identity.supportsQueuedPaperStatus = PDBoolNumber(findings.gs_r1);
        identity.supportsRealtimeStatus = PDBoolNumber(findings.dle_eot);

        NSMutableArray<NSString *> *signals = [NSMutableArray array];
        for (const std::string &signal : assessment.signals) {
            [signals addObject:PDString(signal)];
        }
        identity.signals = signals;

        dispatch_async(dispatch_get_main_queue(), ^{
            completion(identity, nil);
        });
    });
}

- (void)subscribeDeviceEvents:(PDDeviceEventBlock)block {
    if (block == nil) {
        return;
    }
    _driver->subscribeDevices([block](const std::string &printerId, pd::DeviceEvent event) {
        NSString *identifier = PDString(printerId);
        PDDeviceEvent bridged = PDDeviceEventFrom(event);
        dispatch_async(dispatch_get_main_queue(), ^{
            block(identifier, bridged);
        });
    });
}

@end
