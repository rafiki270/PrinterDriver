#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "printerdriver/capability_probe.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/identity.hpp"
#include "printerdriver/job_store.hpp"
// M15 — the option and result types for selfTest()/autoDetect() (docs/api.md §15).
#include "printerdriver/self_test.hpp"
#include "printerdriver/transport.hpp"
#include "printerdriver/types.hpp"

// The public interface (docs/api.md §2): three nouns, one submit call, tri-state
// outcome. Everything below is naming from that document; the sequencing behind it is
// docs/techspec.md §5.2 (GS ( H) and §5.3 (GS r 1).

namespace pd {

namespace detail {
class PrinterRuntime;
class MarkerAllocator;
struct DriverEventHub;
struct JobIndex;
}  // namespace detail

// The print-queue addon (docs/sdk-spec.md §12). Declared here only so it can drive a
// PrintJob it reserved; it lives in its own library and the core never calls it.
class PrintQueue;

// --- Payload tiers (docs/api.md §3) ---------------------------------------------

// Tier 1. 8-bit grayscale, row-major, no padding; 0 = black. The core scales to the
// printer's dot width, binarizes and bands.
struct RasterPayload {
  std::vector<uint8_t> gray;
  uint32_t width = 0;
  uint32_t height = 0;
  escpos::Binarization binarization = escpos::Binarization::FloydSteinberg;
  uint8_t threshold = 128;
  uint32_t max_rows_per_band = 1024;
};

// Tier 2. Bytes produced by escpos::Encoder, carrying the code page they were built
// with so the core can prepend banner text in the same encoding.
struct DocumentPayload {
  escpos::Bytes bytes;
  escpos::CodePage code_page = escpos::CodePage::PC437;
};

// Tier 3. Passed through verbatim. Must not embed its own cuts or realtime status
// tricks: the core owns job termination (docs/api.md §3).
struct RawPayload {
  escpos::Bytes bytes;
};

struct Payload {
  std::variant<RasterPayload, DocumentPayload, RawPayload> content{RawPayload{}};

  static Payload raster(std::vector<uint8_t> gray, uint32_t width, uint32_t height);
  static Payload raster(RasterPayload payload);
  static Payload document(const escpos::Encoder& encoder);
  static Payload document(escpos::Bytes bytes, escpos::CodePage code_page);
  static Payload raw(escpos::Bytes bytes);

  PayloadKind kind() const noexcept;
};

// --- Job options (docs/api.md §3) -----------------------------------------------

// Profile means "whatever this printer's cutter natively does", which is how the
// profile's cut variant reaches a job that did not ask for anything specific. Both
// built-in profiles are partial, so the observable default matches docs/api.md.
enum class CutSetting { Profile, Partial, Full, None };

enum class PreflightMode { Strict, Skip };

struct JobOptions {
  std::string key;  // empty → generated; no cross-restart dedupe protection
  CutSetting cut = CutSetting::Profile;
  bool open_drawer = false;
  PreflightMode preflight = PreflightMode::Strict;
  uint32_t timeout_ms = 0;  // 0 → the profile's completion timeout

  // Whitespace around the print (docs/receipt-dsl.md "Margins"). Top feed is emitted
  // before the first content byte; the bottom figure is the *total* clearance the
  // engine leaves between the last content and the cut, and it can only ever add to
  // the profile's blade-clearance floor: the engine feeds
  // max(profile.media.head_to_cutter_feed_dots, bottom_feed_dots), so no margin
  // setting can reintroduce a clipped trailing QR.
  uint16_t top_feed_dots = 0;
  uint16_t bottom_feed_dots = 0;

  // Print the receipt verification identifier in the ticket trailer — the ORDER line
  // and the trailer QR (docs/api.md §14). On by default: the printed token is what
  // turns a piece of paper into evidence of a specific journaled job. Turning it off
  // suppresses both the line and the QR; the token is still journaled and still
  // resolvable through PrinterDriver::jobByToken.
  bool print_verification_id = true;
};

// forceReprint's own options (docs/api.md §3). Converts implicitly from JobOptions so
// that every existing call site keeps the banner it already had.
struct ReprintOptions {
  JobOptions job;

  // *** REPRINT / POSSIBLE DUPLICATE *** and the attempt counter. Enabled by default;
  // disabling it is a per-call, deliberate act for a receipt where the banner is
  // inappropriate (a customer-facing copy). A kitchen ticket should never disable it —
  // the banner is what lets staff bin the duplicate instead of cooking it twice.
  bool banner = true;

  ReprintOptions() = default;
  ReprintOptions(JobOptions options) : job(std::move(options)) {}  // NOLINT: implicit
};

struct DeviceStatus {
  bool connected = false;
  std::optional<bool> online;
  std::optional<bool> cover_open;
  std::optional<bool> paper_out;
  std::optional<bool> paper_near_end;
  std::optional<bool> cutter_error;
  std::optional<bool> unrecoverable_error;
  std::optional<bool> recoverable_error;
  // False until a status query or ASB frame has actually been decoded. A snapshot
  // that has never heard from the device says so rather than reporting healthy.
  bool observed = false;
};

using DeviceEventCallback = std::function<void(DeviceEvent)>;
using DriverDeviceEventCallback =
    std::function<void(const std::string& printer_id, DeviceEvent)>;

// --- PrintJob (docs/api.md §2) ---------------------------------------------------

class PrintJob {
 public:
  using EventCallback = std::function<void(const JobEvent&)>;

  const std::string& id() const noexcept { return id_; }
  const std::string& key() const noexcept { return key_; }
  const std::string& printerId() const noexcept { return printer_id_; }
  uint32_t attempt() const noexcept { return attempt_; }

  // The receipt verification identifiers (docs/api.md §14): the GS ( H tokens this
  // attempt printed and cut under. Empty until the job has been handed to a worker,
  // and empty for good on a profile whose fence is not GS ( H — the RVI is the wire
  // token, so a printer that has no wire token has no RVI to print. The print token is
  // the one the ticket carries as `V:`.
  std::string printToken() const;
  std::string cutToken() const;

  JobState state() const noexcept { return state_.load(); }
  ConfidenceLevel confidence() const noexcept { return confidence_.load(); }
  bool isTerminal() const noexcept { return terminal_.load(); }

  std::vector<JobEvent> history() const;

  // Replays every event so far, then streams. Callbacks run on the printer's worker
  // thread (or on the calling thread for the replay) and must not block or call back
  // into the driver. The last event is always terminal.
  void subscribe(EventCallback callback);

  JobResult result() const;
  std::optional<JobResult> result(std::chrono::milliseconds timeout) const;

 private:
  friend class detail::PrinterRuntime;
  friend class PrinterDriver;
  friend class Printer;
  // The queue addon publishes HeldOffline, and terminates jobs that expire or overflow
  // before they ever reach a worker (docs/sdk-spec.md §12). Both are transitions on a
  // job the engine has not started, which is exactly why they cannot come from it.
  friend class PrintQueue;

  PrintJob(std::string id, std::string key, std::string printer_id, uint32_t attempt);

  // Registration and history replay are atomic from the subscriber's point of view:
  // emit() parks events for a subscriber whose replay is still running, and
  // subscribe() delivers the parked backlog before letting the subscriber go live,
  // so a late subscriber can never see a newer event before an older recorded one.
  // Parking, not replaying under mutex_, because no callback may ever run under
  // mutex_: a caller that blocks or takes its own lock in a callback must not be
  // able to deadlock the job.
  struct Subscriber {
    EventCallback callback;
    bool draining = true;
    std::vector<JobEvent> pending;
  };

  void emit(JobState state, ConfidenceLevel confidence,
            std::optional<FailureReason> reason);
  void finish(const JobResult& outcome);
  void setTokens(const std::string& print_token, const std::string& cut_token);

  std::string id_;
  std::string key_;
  std::string printer_id_;
  uint32_t attempt_ = 1;
  std::string print_token_;
  std::string cut_token_;

  std::atomic<JobState> state_{JobState::Queued};
  std::atomic<ConfidenceLevel> confidence_{ConfidenceLevel::TransportAccepted};
  std::atomic<bool> terminal_{false};

  mutable std::mutex mutex_;
  mutable std::condition_variable done_;
  std::vector<JobEvent> history_;
  // shared_ptr because subscribe() keeps working with its entry after releasing
  // mutex_, across reallocations of this vector. There is no unsubscribe.
  std::vector<std::shared_ptr<Subscriber>> subscribers_;
  JobResult result_;
};

// --- Printer (docs/api.md §2) ----------------------------------------------------

// When the driver is allowed to interrogate the device (docs/capability-profiles.md:
// probe once, persist, never re-probe on every boot).
enum class ProbePolicy {
  UseStored,   // default: apply stored findings if any, never touch the device
  IfUnknown,   // interrogate once, when nothing is stored for this printer
  Always,      // interrogate on every addPrinter — for a bench, not for a venue
  Never,       // ignore stored findings too; the profile is the whole truth
};

struct PrinterConfig {
  std::string id;  // empty → derived from the transport description
  TransportFactory transport;
  uint32_t width_dots = escpos::kWidth80mm;
  CapabilityProfile profile = generic_escpos();
  ProbePolicy probe = ProbePolicy::UseStored;
  // Per-phase budgets for the interrogation. The endpoint and hints below are filled
  // in by the driver.
  ProbeOptions probe_options;
  // Anything the caller already knows about the device, folded into the fingerprint.
  IdentityHints identity_hints;
};

class Printer {
 public:
  ~Printer();

  Printer(const Printer&) = delete;
  Printer& operator=(const Printer&) = delete;

  const std::string& id() const noexcept;
  uint32_t widthDots() const noexcept;
  // By value: a probe may replace the effective profile on the worker thread once,
  // before the first job runs, so a reference could dangle across that swap.
  CapabilityProfile profile() const;
  // What the probe established first-hand, if it ran or if stored findings applied.
  std::optional<CapabilityFindings> findings() const;

  // Submitting an existing key returns the existing job in whatever state it is in
  // and prints nothing (docs/api.md §3).
  std::shared_ptr<PrintJob> print(Payload payload, const JobOptions& options = {});

  // Deliberate duplicate. Prepends *** REPRINT / POSSIBLE DUPLICATE *** and the
  // attempt counter (docs/sdk-spec.md §6) unless ReprintOptions::banner says
  // otherwise. The no-payload overload reuses the original submission's payload and
  // returns nullptr when the original job was loaded from the journal, whose records
  // carry state but not bytes.
  std::shared_ptr<PrintJob> forceReprint(const std::string& key,
                                         const ReprintOptions& options = {});
  std::shared_ptr<PrintJob> forceReprint(const std::string& key, Payload payload,
                                         const ReprintOptions& options = {});

  // Last known state; never a live query, so it cannot block behind a print.
  DeviceStatus status() const;
  // Queues a DLE EOT 1-4 round trip behind any active job and waits for the answer.
  DeviceStatus refreshStatus(std::chrono::milliseconds timeout);

  void subscribe(DeviceEventCallback callback);
  void openCashDrawer();

  // --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------
  //
  // openCashDrawer() above is the fire-and-forget call this SDK shipped with: it
  // queues one ESC p and tells the caller nothing, which is the `{sent: true}` the
  // drawer document rejects. It stays because it is ABI, and because a caller that
  // genuinely does not want to wait is entitled to say so. openDrawer() is the
  // verified sequence and is what everything new should use.
  //
  // The sequence, from docs/cash-drawer.md "Opening sequence":
  //   1. read the sensor first — a drawer that is already open is never pulsed again;
  //   2. run on the printer's own worker, so a pulse can never interleave with the
  //      bytes of a fenced job;
  //   3. one queued pulse at the profile's duration on the requested channel;
  //   4. watch the switch for the profile's verification window;
  //   5. it changed  -> OpenVerified;
  //   6. it did not  -> FailedToOpen;
  //   7. nothing can answer -> KickSentUnverified, which is a different claim;
  //   8. hold the manufacturer cooldown before the next pulse.
  //
  // Blocks until the sequence reaches a verdict. Refused — zero bytes written, state
  // Unknown — when the profile has no drawer, when its kick method is one this engine
  // cannot drive, or when the electrical standard is Unknown.
  DrawerOpenResult openDrawer(const DrawerRequest& request = {});

  // One non-destructive read of the drawer switch. Never pulses, so it is safe on
  // hardware openDrawer() refuses. Reports the raw pin level and, only when the
  // polarity has been calibrated, an interpretation of it.
  DrawerReading readDrawerSensor(std::chrono::milliseconds timeout);

  // Records which pin level means "open" for the drawer physically attached to this
  // printer, and persists it (docs/cash-drawer.md §4 — Star warns that the meaning of
  // the signal depends on the attached drawer, so this SDK measures instead of
  // assuming). Returns false when there is nowhere to persist it, in which case the
  // calibration still applies to this process.
  bool calibrateDrawerPolarity(bool high_means_open);

  DrawerCapabilities drawerCapabilities() const;
  DrawerPolarity drawerPolarity() const;
  // --- end M14 ---------------------------------------------------------------------

  // --- M15: self-test (docs/api.md §15) ---------------------------------------------
  //
  // Prints ONE diagnostic ticket through the full fenced engine and returns the ordinary
  // tri-state result plus what the ticket says. The paper is the detection report:
  // identity, profile and how it was selected, media, completion mechanism with its
  // grade ceiling and provenance, the drawer classification, a Czech/Hungarian/Polish
  // charset line, a Code 128 sample, and the job's own GS ( H verification token in the
  // trailer QR. Anything the profile cannot do is printed as a declared degradation
  // rather than silently dropped.
  //
  // Composition only: identify + probe + the DSL renderer + print(), with an
  // idempotency key of `selftest-<unix ms>` and printVerificationId on. There is no
  // second engine and no bypass, which is the point — a Done at grade A here is the
  // statement that the ordinary path works end to end on this unit.
  //
  // Blocks until the job is terminal. DEFINED IN THE DSL LIBRARY: see the note at the
  // top of printerdriver/self_test.hpp.
  SelfTestResult selfTest(const SelfTestOptions& options = {});

  // Runs the capability probe against this device on this printer's own worker thread,
  // behind whatever is queued, and returns what it established — the same probe
  // addPrinter schedules under ProbePolicy::IfUnknown and Always, never a second one.
  // Exposed because a self-test, and an operator, want to be able to ask *now*.
  // `print_test_lines` false keeps it printless at the cost of asking the ordered fences
  // out of an empty buffer (see AutoDetectOptions for why that is a weaker answer).
  // nullopt when the device could not be reached or answered nothing at all.
  std::optional<CapabilityFindings> probeNow(bool print_test_lines = true);
  // --- end M15 -----------------------------------------------------------------------

  // Blocks until the queue is empty and the active job is terminal.
  void drain();

  // --- Print-queue addon hooks (docs/sdk-spec.md §12) ------------------------------
  //
  // An addon that holds jobs has to hand its caller an ordinary PrintJob while the
  // bytes are still parked, then send that same job later — one handle, one id, one
  // journal record, one event stream. These two calls are the whole mechanism.
  //
  // reserveJob() mints the job and claims the idempotency key in the driver's index,
  // so a key waiting in a queue already dedupes against a direct print() and shows up
  // in PrinterDriver::findJob. It returns the existing job (with *created false) when
  // the key is already known, which is how docs/sdk-spec.md §12 rule 2 falls out.
  //
  // submitReserved() runs a reserved job down the identical path print() uses: same
  // worker thread, same preflight, same fences, same confidence grading. There is no
  // second engine and no bypass (§12 rule 3) — the only difference from print() is
  // that the handle already exists and the caller already published states on it.
  std::shared_ptr<PrintJob> reserveJob(const std::string& key, PayloadKind kind,
                                       uint64_t payload_bytes, bool* created);
  void submitReserved(const std::shared_ptr<PrintJob>& job, Payload payload,
                      const JobOptions& options);

 private:
  friend class PrinterDriver;
  explicit Printer(std::shared_ptr<detail::PrinterRuntime> runtime);

  std::shared_ptr<detail::PrinterRuntime> rt_;
};

// --- PrinterDriver (docs/api.md §2) ----------------------------------------------

class PrinterDriver {
 public:
  explicit PrinterDriver(StorageConfig storage);
  ~PrinterDriver();

  PrinterDriver(const PrinterDriver&) = delete;
  PrinterDriver& operator=(const PrinterDriver&) = delete;

  std::shared_ptr<Printer> addPrinter(PrinterConfig config);
  std::shared_ptr<Printer> printer(const std::string& printer_id) const;
  std::vector<std::shared_ptr<Printer>> printers() const;

  // Any job this driver knows about, including ones reconstructed from the journal
  // after a restart (docs/api.md §4).
  std::shared_ptr<PrintJob> findJob(const std::string& key) const;

  // Paper → job (docs/api.md §14). Resolves either of a job's verification
  // identifiers, print or cut, most-recent-first: a sequence wraps after 4 418 jobs
  // per instance nonce and the newest holder of a token is the one an operator holding
  // a receipt is asking about. Includes jobs reconstructed from the journal, so a
  // receipt printed before the last restart still resolves.
  std::shared_ptr<PrintJob> jobByToken(const std::string& token) const;

  std::shared_ptr<PrintJob> forceReprint(const std::string& key,
                                         const ReprintOptions& options = {});
  std::shared_ptr<PrintJob> forceReprint(const std::string& key, Payload payload,
                                         const ReprintOptions& options = {});

  void subscribeDevices(DriverDeviceEventCallback callback);

  JobStore& store() noexcept { return *store_; }
  const JobStore& store() const noexcept { return *store_; }

  // Probe results, keyed by device identity and shared by every printer on this
  // driver so a device that moves address keeps what was learned about it.
  FindingsStore& capabilities() noexcept { return *capabilities_; }
  const FindingsStore& capabilities() const noexcept { return *capabilities_; }

  // --- M14 --- Calibrated drawer polarities, keyed by printer id and persisted next
  // to the journal for the same reason the instance nonce is: it describes the
  // installation, not this run (docs/cash-drawer.md §4).
  DrawerPolarityStore& drawerPolarities() noexcept { return *drawer_polarities_; }
  const DrawerPolarityStore& drawerPolarities() const noexcept {
    return *drawer_polarities_;
  }
  // --- end M14 ---

  // The two-character random prefix every verification identifier this driver issues
  // carries (docs/api.md §14). Persisted in the storage directory at first start, so
  // the tokens on yesterday's paper still name this instance; regenerated per process
  // for an in-memory driver, which has nowhere to keep it.
  const std::string& instanceNonce() const noexcept;

  // --- M15: auto-detection (docs/api.md §15) -----------------------------------------
  //
  // The one call from "I know nothing" to a list of configured-and-classified printers:
  // LAN discovery (the non-printing DLE EOT 1 sweep of discovery.hpp) → multi-signal
  // identification per candidate → the non-destructive capability probe, respecting the
  // stored-findings cache → one DetectedPrinter per address, classified honestly.
  //
  // NOTHING PRINTS AND NOTHING FIRES. Only the printless probe paths are used: DLE EOT,
  // GS I, and the two fences asked out of an empty buffer. The full probe's GS ( H test
  // line DOES print, so it is not used here, and a completion established this way is
  // reported without promoting its provenance — read the comment on
  // AutoDetectOptions::allow_printing_probe before trusting a fence this call found.
  //
  // Blocks until every candidate is finished. `on_candidate` fires as each one is,
  // from a worker thread.
  std::vector<DetectedPrinter> autoDetect(const AutoDetectOptions& options = {},
                                          const AutoDetectProgressCallback& on_candidate = {});
  // --- end M15 -----------------------------------------------------------------------

  void shutdown();

 private:
  friend class detail::PrinterRuntime;

  mutable std::mutex mutex_;
  std::shared_ptr<JobStore> store_;
  std::shared_ptr<FindingsStore> capabilities_;
  std::shared_ptr<DrawerPolarityStore> drawer_polarities_;  // M14
  std::shared_ptr<detail::MarkerAllocator> markers_;
  std::shared_ptr<detail::DriverEventHub> hub_;
  std::shared_ptr<detail::JobIndex> index_;
  std::vector<std::shared_ptr<Printer>> printers_;
  bool stopped_ = false;
};

// Text the reprint banner is built from (docs/techspec.md §7.1). Exposed so tests
// and wrappers assert on the same literal the wire carries.
extern const char kReprintBannerLine[];
extern const char kReprintAttemptPrefix[];
// The ticket trailer: `ORDER: <key>  V:<print-token>`, plus the same string as the
// trailer QR payload (docs/api.md §14).
extern const char kOrderPrefix[];
extern const char kVerificationPrefix[];

}  // namespace pd
