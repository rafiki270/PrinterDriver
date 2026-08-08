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

  PrintJob(std::string id, std::string key, std::string printer_id, uint32_t attempt);

  void emit(JobState state, ConfidenceLevel confidence,
            std::optional<FailureReason> reason);
  void finish(const JobResult& outcome);

  std::string id_;
  std::string key_;
  std::string printer_id_;
  uint32_t attempt_ = 1;

  std::atomic<JobState> state_{JobState::Queued};
  std::atomic<ConfidenceLevel> confidence_{ConfidenceLevel::TransportAccepted};
  std::atomic<bool> terminal_{false};

  mutable std::mutex mutex_;
  mutable std::condition_variable done_;
  std::vector<JobEvent> history_;
  std::vector<EventCallback> subscribers_;
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
  // attempt counter (docs/sdk-spec.md §6). The no-payload overload reuses the
  // original submission's payload and returns nullptr when the original job was
  // loaded from the journal, whose records carry state but not bytes.
  std::shared_ptr<PrintJob> forceReprint(const std::string& key,
                                         const JobOptions& options = {});
  std::shared_ptr<PrintJob> forceReprint(const std::string& key, Payload payload,
                                         const JobOptions& options = {});

  // Last known state; never a live query, so it cannot block behind a print.
  DeviceStatus status() const;
  // Queues a DLE EOT 1-4 round trip behind any active job and waits for the answer.
  DeviceStatus refreshStatus(std::chrono::milliseconds timeout);

  void subscribe(DeviceEventCallback callback);
  void openCashDrawer();

  // Blocks until the queue is empty and the active job is terminal.
  void drain();

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
  std::shared_ptr<PrintJob> forceReprint(const std::string& key,
                                         const JobOptions& options = {});
  std::shared_ptr<PrintJob> forceReprint(const std::string& key, Payload payload,
                                         const JobOptions& options = {});

  void subscribeDevices(DriverDeviceEventCallback callback);

  JobStore& store() noexcept { return *store_; }
  const JobStore& store() const noexcept { return *store_; }

  // Probe results, keyed by device identity and shared by every printer on this
  // driver so a device that moves address keeps what was learned about it.
  FindingsStore& capabilities() noexcept { return *capabilities_; }
  const FindingsStore& capabilities() const noexcept { return *capabilities_; }

  void shutdown();

 private:
  friend class detail::PrinterRuntime;

  mutable std::mutex mutex_;
  std::shared_ptr<JobStore> store_;
  std::shared_ptr<FindingsStore> capabilities_;
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

}  // namespace pd
