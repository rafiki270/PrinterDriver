#include "printerdriver/driver.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

#include "printerdriver/response_parser.hpp"

namespace pd {

const char kReprintBannerLine[] = "*** REPRINT / POSSIBLE DUPLICATE ***";
const char kReprintAttemptPrefix[] = "PRINT ATTEMPT: ";

namespace {

std::string randomHex(size_t digits) {
  static const char kDigits[] = "0123456789abcdef";
  static std::mutex mutex;
  static std::mt19937_64 engine(std::random_device{}());
  std::lock_guard<std::mutex> lock(mutex);
  std::string out;
  out.reserve(digits);
  for (size_t i = 0; i < digits; ++i) {
    out += kDigits[engine() & 0xFu];
  }
  return out;
}

std::string newJobId() {
  return randomHex(8) + "-" + randomHex(4) + "-" + randomHex(4) + "-" + randomHex(4) +
         "-" + randomHex(12);
}

// Confidence is an evidence ladder, so "the strongest thing we can prove" is a max
// over an ordinal, and a profile ceiling is a min against the same ordinal.
int rank(ConfidenceLevel level) noexcept { return static_cast<int>(level); }

ConfidenceLevel raise(ConfidenceLevel current, ConfidenceLevel candidate) noexcept {
  return rank(candidate) > rank(current) ? candidate : current;
}

ConfidenceLevel clampTo(ConfidenceLevel value, ConfidenceLevel ceiling) noexcept {
  return rank(value) > rank(ceiling) ? ceiling : value;
}

CutVariant effectiveCut(CutSetting setting, const CapabilityProfile& profile) noexcept {
  switch (setting) {
    case CutSetting::Profile: return profile.cut;
    case CutSetting::Partial: return CutVariant::Partial;
    case CutSetting::Full: return CutVariant::Full;
    case CutSetting::None: return CutVariant::None;
  }
  return CutVariant::None;
}

std::string threeDigits(uint32_t value) {
  std::string out = std::to_string(value % 1000u);
  while (out.size() < 3) {
    out.insert(out.begin(), '0');
  }
  return out;
}

}  // namespace

// --- Payload ---------------------------------------------------------------------

Payload Payload::raster(std::vector<uint8_t> gray, uint32_t width, uint32_t height) {
  RasterPayload raster_payload;
  raster_payload.gray = std::move(gray);
  raster_payload.width = width;
  raster_payload.height = height;
  return Payload{raster_payload};
}

Payload Payload::raster(RasterPayload payload) { return Payload{std::move(payload)}; }

Payload Payload::document(const escpos::Encoder& encoder) {
  return document(encoder.bytes(), encoder.codePage());
}

Payload Payload::document(escpos::Bytes bytes, escpos::CodePage code_page) {
  DocumentPayload document_payload;
  document_payload.bytes = std::move(bytes);
  document_payload.code_page = code_page;
  return Payload{std::move(document_payload)};
}

Payload Payload::raw(escpos::Bytes bytes) {
  RawPayload raw_payload;
  raw_payload.bytes = std::move(bytes);
  return Payload{std::move(raw_payload)};
}

PayloadKind Payload::kind() const noexcept {
  if (std::holds_alternative<RasterPayload>(content)) {
    return PayloadKind::Raster;
  }
  if (std::holds_alternative<DocumentPayload>(content)) {
    return PayloadKind::Document;
  }
  return PayloadKind::Raw;
}

// --- PrintJob --------------------------------------------------------------------

PrintJob::PrintJob(std::string id, std::string key, std::string printer_id,
                   uint32_t attempt)
    : id_(std::move(id)),
      key_(std::move(key)),
      printer_id_(std::move(printer_id)),
      attempt_(attempt) {}

std::vector<JobEvent> PrintJob::history() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return history_;
}

void PrintJob::subscribe(EventCallback callback) {
  if (!callback) {
    return;
  }
  std::vector<JobEvent> replay;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    replay = history_;
    subscribers_.push_back(callback);
  }
  for (const JobEvent& event : replay) {
    callback(event);
  }
}

void PrintJob::emit(JobState state, ConfidenceLevel confidence,
                    std::optional<FailureReason> reason) {
  const JobEvent event = JobEvent::make(state, confidence, reason);
  std::vector<EventCallback> subscribers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.store(state);
    confidence_.store(confidence);
    history_.push_back(event);
    subscribers = subscribers_;
  }
  for (const EventCallback& callback : subscribers) {
    callback(event);
  }
}

void PrintJob::finish(const JobResult& outcome) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = outcome;
  }
  terminal_.store(true);
  done_.notify_all();
}

JobResult PrintJob::result() const {
  std::unique_lock<std::mutex> lock(mutex_);
  done_.wait(lock, [this] { return terminal_.load(); });
  return result_;
}

std::optional<JobResult> PrintJob::result(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!done_.wait_for(lock, timeout, [this] { return terminal_.load(); })) {
    return std::nullopt;
  }
  return result_;
}

// --- Internals -------------------------------------------------------------------

namespace detail {

// Four printable characters mapped to a job id (docs/techspec.md §5.2). The suffix is
// shared by a job's P and C tokens so a captured stream reads as one receipt, and it
// is held until the job ends so no outstanding marker can be answered by a later job.
class MarkerAllocator {
 public:
  std::string acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < 1000; ++i) {
      std::string suffix = threeDigits(next_++);
      if (in_use_.insert(suffix).second) {
        return suffix;
      }
    }
    throw std::runtime_error("no free completion marker tokens");
  }

  void release(const std::string& suffix) {
    std::lock_guard<std::mutex> lock(mutex_);
    in_use_.erase(suffix);
  }

  size_t outstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_use_.size();
  }

 private:
  mutable std::mutex mutex_;
  uint32_t next_ = 0;
  std::set<std::string> in_use_;
};

struct DriverEventHub {
  std::mutex mutex;
  std::vector<DriverDeviceEventCallback> subscribers;

  void emit(const std::string& printer_id, DeviceEvent event) {
    std::vector<DriverDeviceEventCallback> copy;
    {
      std::lock_guard<std::mutex> lock(mutex);
      copy = subscribers;
    }
    for (const auto& callback : copy) {
      callback(printer_id, event);
    }
  }
};

struct JobEntry {
  std::shared_ptr<PrintJob> job;
  std::string printer_id;
  std::shared_ptr<Payload> payload;  // null for jobs reconstructed from the journal
  JobOptions options;
  uint32_t attempt = 1;
};

struct JobIndex {
  mutable std::mutex mutex;
  std::unordered_map<std::string, JobEntry> by_key;
};

enum class WaitOutcome { Signalled, LinkDown, Timeout, Aborted };

struct Task {
  std::function<void()> run;
  std::function<void()> cancel;
};

class PrinterRuntime {
 public:
  PrinterRuntime(PrinterConfig config, std::shared_ptr<JobStore> store,
                 std::shared_ptr<MarkerAllocator> markers,
                 std::shared_ptr<DriverEventHub> hub, std::shared_ptr<JobIndex> index)
      : config_(std::move(config)),
        store_(std::move(store)),
        markers_(std::move(markers)),
        hub_(std::move(hub)),
        index_(std::move(index)) {}

  ~PrinterRuntime() { stop(); }

  void start() {
    worker_ = std::thread([this] { workerLoop(); });
  }

  void stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    stopping_.store(true);
    queue_cv_.notify_all();
    io_cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    std::deque<Task> leftovers;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      leftovers.swap(tasks_);
    }
    for (Task& task : leftovers) {
      if (task.cancel) {
        task.cancel();
      }
    }
    closeTransport();
    idle_cv_.notify_all();
  }

  const std::string& id() const noexcept { return config_.id; }
  const CapabilityProfile& profile() const noexcept { return config_.profile; }
  uint32_t widthDots() const noexcept { return config_.width_dots; }

  // A null payload is only valid for a reprint, where the original submission's
  // payload is reused.
  std::shared_ptr<PrintJob> submit(std::shared_ptr<Payload> payload, JobOptions options,
                                   bool reprint);

  DeviceStatus status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
  }

  DeviceStatus refreshStatus(std::chrono::milliseconds timeout);
  void openCashDrawer();
  void drain();

  void subscribeDevice(DeviceEventCallback callback) {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    device_subscribers_.push_back(std::move(callback));
  }

 private:
  void workerLoop();
  void push(Task task);

  bool ensureConnected(std::string* error);
  void closeTransport();

  void runJob(const std::shared_ptr<PrintJob>& job, const Payload& payload,
              const JobOptions& options, uint32_t attempt);

  void beginJobIo();
  WaitOutcome awaitToken(const std::string& token, std::chrono::milliseconds timeout);
  WaitOutcome awaitQueued(std::chrono::milliseconds timeout);
  WaitOutcome awaitRealtime(size_t count, std::chrono::milliseconds timeout,
                            std::vector<escpos::ParsedEvent>* out);
  void clearRealtime();

  void onBytes(const uint8_t* data, size_t size);
  void onDisconnected(TransportError error, const std::string& message);
  void mergeStatus(const escpos::StatusFlags& flags, std::vector<DeviceEvent>* out);
  void dispatchDeviceEvents(const std::vector<DeviceEvent>& events);

  void advance(const std::shared_ptr<PrintJob>& job, JobState state,
               ConfidenceLevel confidence, FailureReason reason);
  void terminate(const std::shared_ptr<PrintJob>& job, JobState state,
                 const JobResult& outcome);

  escpos::Bytes buildPayload(const Payload& payload, const JobOptions& options,
                             uint32_t attempt, const std::string& key) const;
  escpos::Bytes buildCut(CutVariant variant, const std::string& marker_token) const;
  TransportResult sendPaced(const escpos::Bytes& bytes);

  PrinterConfig config_;
  std::shared_ptr<JobStore> store_;
  std::shared_ptr<MarkerAllocator> markers_;
  std::shared_ptr<DriverEventHub> hub_;
  std::shared_ptr<JobIndex> index_;

  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::condition_variable idle_cv_;
  std::deque<Task> tasks_;
  bool active_ = false;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stopped_{false};

  std::mutex connection_mutex_;
  std::unique_ptr<Transport> transport_;
  bool had_connection_ = false;

  mutable std::mutex io_mutex_;
  std::condition_variable io_cv_;
  escpos::ResponseParser parser_;
  std::vector<std::string> gsh_tokens_;
  size_t queued_answers_ = 0;
  std::vector<escpos::ParsedEvent> realtime_answers_;
  bool link_down_ = false;

  mutable std::mutex status_mutex_;
  DeviceStatus status_;

  std::mutex subscriber_mutex_;
  std::vector<DeviceEventCallback> device_subscribers_;
};

void PrinterRuntime::push(Task task) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    tasks_.push_back(std::move(task));
  }
  queue_cv_.notify_all();
}

void PrinterRuntime::workerLoop() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stopping_.load() || !tasks_.empty(); });
      if (stopping_.load()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
      active_ = true;
    }
    if (task.run) {
      task.run();
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      active_ = false;
    }
    idle_cv_.notify_all();
  }
}

void PrinterRuntime::drain() {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  idle_cv_.wait(lock, [this] {
    return stopped_.load() || (tasks_.empty() && !active_);
  });
}

bool PrinterRuntime::ensureConnected(std::string* error) {
  bool restored = false;
  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (transport_ && transport_->isConnected()) {
      return true;
    }
    if (transport_) {
      transport_->close();
      transport_.reset();
    }
    if (!config_.transport) {
      if (error) {
        *error = "printer " + config_.id + " has no transport configured";
      }
      return false;
    }
    std::unique_ptr<Transport> transport = config_.transport();
    if (!transport) {
      if (error) {
        *error = "transport factory returned nothing for printer " + config_.id;
      }
      return false;
    }
    transport->onBytes([this](const uint8_t* data, size_t size) { onBytes(data, size); });
    transport->onDisconnected([this](TransportError code, const std::string& message) {
      onDisconnected(code, message);
    });
    const TransportResult result = transport->connect();
    if (!result.ok) {
      if (error) {
        *error = result.message;
      }
      return false;
    }
    transport_ = std::move(transport);
    restored = had_connection_;
    had_connection_ = true;
  }
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.connected = true;
  }
  if (restored) {
    dispatchDeviceEvents({DeviceEvent::ConnectionRestored});
  }
  return true;
}

void PrinterRuntime::closeTransport() {
  std::unique_ptr<Transport> transport;
  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    transport = std::move(transport_);
  }
  if (transport) {
    transport->close();
  }
  std::lock_guard<std::mutex> status_lock(status_mutex_);
  status_.connected = false;
}

void PrinterRuntime::onBytes(const uint8_t* data, size_t size) {
  std::vector<DeviceEvent> events;
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    for (escpos::ParsedEvent& event : parser_.feed(data, size)) {
      switch (event.kind) {
        case escpos::ParsedEventKind::GsHAck:
          gsh_tokens_.push_back(event.token);
          break;
        case escpos::ParsedEventKind::QueuedStatus:
          ++queued_answers_;
          break;
        case escpos::ParsedEventKind::RealtimeStatus:
          mergeStatus(event.flags, &events);
          realtime_answers_.push_back(event);
          break;
        case escpos::ParsedEventKind::AsbStatus:
          mergeStatus(event.flags, &events);
          break;
        case escpos::ParsedEventKind::UnknownByte:
          break;
      }
    }
  }
  io_cv_.notify_all();
  dispatchDeviceEvents(events);
}

void PrinterRuntime::onDisconnected(TransportError, const std::string&) {
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    link_down_ = true;
  }
  io_cv_.notify_all();
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.connected = false;
  }
  dispatchDeviceEvents({DeviceEvent::ConnectionLost});
}

void PrinterRuntime::mergeStatus(const escpos::StatusFlags& flags,
                                 std::vector<DeviceEvent>* out) {
  const std::vector<DeviceEvent> events = escpos::toDeviceEvents(flags);
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.observed = true;
    if (flags.online) status_.online = flags.online;
    if (flags.cover_open) status_.cover_open = flags.cover_open;
    if (flags.paper_out) status_.paper_out = flags.paper_out;
    if (flags.paper_near_end) status_.paper_near_end = flags.paper_near_end;
    if (flags.cutter_error) status_.cutter_error = flags.cutter_error;
    if (flags.unrecoverable_error) status_.unrecoverable_error = flags.unrecoverable_error;
    if (flags.auto_recoverable_error) status_.recoverable_error = flags.auto_recoverable_error;
  }
  if (out) {
    out->insert(out->end(), events.begin(), events.end());
  }
}

void PrinterRuntime::dispatchDeviceEvents(const std::vector<DeviceEvent>& events) {
  if (events.empty()) {
    return;
  }
  std::vector<DeviceEventCallback> subscribers;
  {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    subscribers = device_subscribers_;
  }
  for (const DeviceEvent event : events) {
    for (const DeviceEventCallback& callback : subscribers) {
      callback(event);
    }
    if (hub_) {
      hub_->emit(config_.id, event);
    }
  }
}

void PrinterRuntime::beginJobIo() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  parser_.reset();
  gsh_tokens_.clear();
  queued_answers_ = 0;
  realtime_answers_.clear();
  link_down_ = false;
}

void PrinterRuntime::clearRealtime() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  realtime_answers_.clear();
  if (parser_.outstandingRealtime() > 0) {
    // Unanswered realtime expectations would steal the next queued answer, so they
    // are dropped once the phase that asked for them is over.
    parser_.reset();
  }
}

WaitOutcome PrinterRuntime::awaitToken(const std::string& token,
                                       std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(io_mutex_);
  const bool signalled = io_cv_.wait_for(lock, timeout, [this, &token] {
    return stopping_.load() || link_down_ ||
           std::find(gsh_tokens_.begin(), gsh_tokens_.end(), token) != gsh_tokens_.end();
  });
  (void)signalled;
  if (std::find(gsh_tokens_.begin(), gsh_tokens_.end(), token) != gsh_tokens_.end()) {
    return WaitOutcome::Signalled;
  }
  if (link_down_) {
    return WaitOutcome::LinkDown;
  }
  if (stopping_.load()) {
    return WaitOutcome::Aborted;
  }
  return WaitOutcome::Timeout;
}

WaitOutcome PrinterRuntime::awaitQueued(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(io_mutex_);
  io_cv_.wait_for(lock, timeout, [this] {
    return stopping_.load() || link_down_ || queued_answers_ > 0;
  });
  if (queued_answers_ > 0) {
    --queued_answers_;
    return WaitOutcome::Signalled;
  }
  if (link_down_) {
    return WaitOutcome::LinkDown;
  }
  if (stopping_.load()) {
    return WaitOutcome::Aborted;
  }
  return WaitOutcome::Timeout;
}

WaitOutcome PrinterRuntime::awaitRealtime(size_t count, std::chrono::milliseconds timeout,
                                          std::vector<escpos::ParsedEvent>* out) {
  std::unique_lock<std::mutex> lock(io_mutex_);
  io_cv_.wait_for(lock, timeout, [this, count] {
    return stopping_.load() || link_down_ || realtime_answers_.size() >= count;
  });
  if (out) {
    *out = realtime_answers_;
  }
  if (realtime_answers_.size() >= count) {
    return WaitOutcome::Signalled;
  }
  if (link_down_) {
    return WaitOutcome::LinkDown;
  }
  if (stopping_.load()) {
    return WaitOutcome::Aborted;
  }
  return WaitOutcome::Timeout;
}

void PrinterRuntime::advance(const std::shared_ptr<PrintJob>& job, JobState state,
                             ConfidenceLevel confidence, FailureReason reason) {
  // Persist first, then publish: an observer must never learn of a transition the
  // journal has not committed (docs/techspec.md §5.1).
  store_->recordState(job->id(), state, confidence, reason);
  job->emit(state, confidence,
            reason == FailureReason::None ? std::optional<FailureReason>()
                                          : std::optional<FailureReason>(reason));
}

void PrinterRuntime::terminate(const std::shared_ptr<PrintJob>& job, JobState state,
                               const JobResult& outcome) {
  advance(job, state, outcome.confidence, outcome.reason);
  job->finish(outcome);
}

escpos::Bytes PrinterRuntime::buildPayload(const Payload& payload,
                                           const JobOptions& options, uint32_t attempt,
                                           const std::string& key) const {
  const escpos::CodePage code_page =
      std::holds_alternative<DocumentPayload>(payload.content)
          ? std::get<DocumentPayload>(payload.content).code_page
          : config_.profile.code_page;

  escpos::Encoder encoder;
  // The core owns job framing: every job starts from a known printer state, which is
  // also what makes a raw payload's trailing fence attributable (docs/api.md §3).
  encoder.initialize();
  encoder.selectCodePage(code_page);

  if (attempt > 1) {
    encoder.align(escpos::Alignment::Center)
        .bold(true)
        .line(kReprintBannerLine)
        .bold(false);
    if (!key.empty()) {
      encoder.line("ORDER: " + key);
    }
    encoder.line(std::string(kReprintAttemptPrefix) + std::to_string(attempt))
        .align(escpos::Alignment::Left)
        .feed();
  }

  if (std::holds_alternative<RasterPayload>(payload.content)) {
    const RasterPayload& raster = std::get<RasterPayload>(payload.content);
    if (raster.width > 0 && raster.height > 0 && !raster.gray.empty()) {
      encoder.rasterGrayscale(raster.gray.data(), raster.width, raster.height,
                              config_.width_dots, raster.binarization, raster.threshold,
                              escpos::RasterScale::Normal, raster.max_rows_per_band);
    }
  } else if (std::holds_alternative<DocumentPayload>(payload.content)) {
    encoder.raw(std::get<DocumentPayload>(payload.content).bytes);
  } else {
    encoder.raw(std::get<RawPayload>(payload.content).bytes);
  }

  if (options.open_drawer) {
    encoder.kickCashDrawer();
  }
  // The fence attaches to the last print operation, so the job has to end in one.
  encoder.feedLines(config_.profile.final_feed_lines);
  return encoder.take();
}

escpos::Bytes PrinterRuntime::buildCut(CutVariant variant,
                                       const std::string& marker_token) const {
  escpos::Encoder encoder;
  encoder.useCutWithFeed(config_.profile.cut_with_feed, config_.profile.cut_feed_units);
  encoder.cut(variant == CutVariant::Full ? escpos::CutMode::Full
                                          : escpos::CutMode::Partial);
  switch (config_.profile.completion) {
    case CompletionMechanism::GsParenH:
      encoder.processId(marker_token);
      break;
    case CompletionMechanism::GsR1:
      encoder.queuedPaperStatus();
      break;
    case CompletionMechanism::None:
      break;
  }
  return encoder.take();
}

TransportResult PrinterRuntime::sendPaced(const escpos::Bytes& bytes) {
  Transport* transport = nullptr;
  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    transport = transport_.get();
  }
  if (transport == nullptr) {
    return TransportResult::failure(TransportError::NotConnected, "no transport");
  }
  const size_t chunk = config_.profile.chunk_bytes;
  if (chunk == 0) {
    return transport->write(bytes.data(), bytes.size());
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    const size_t take = std::min(chunk, bytes.size() - offset);
    const TransportResult result = transport->write(bytes.data() + offset, take);
    if (!result.ok) {
      return TransportResult::failure(result.error, result.message,
                                      offset + result.bytes_written);
    }
    offset += take;
    if (offset < bytes.size() && config_.profile.inter_chunk_delay_ms > 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.profile.inter_chunk_delay_ms));
    }
  }
  return TransportResult::success(offset);
}

void PrinterRuntime::runJob(const std::shared_ptr<PrintJob>& job, const Payload& payload,
                            const JobOptions& options, uint32_t attempt) {
  const CapabilityProfile& profile = config_.profile;
  const ConfidenceLevel ceiling = profile.maxConfidence();
  const auto timeout = std::chrono::milliseconds(
      options.timeout_ms != 0 ? options.timeout_ms : profile.completion_timeout_ms);
  const auto realtime_timeout = std::chrono::milliseconds(profile.preflight_timeout_ms);
  ConfidenceLevel confidence = ConfidenceLevel::TransportAccepted;
  const auto reached = [&] { return clampTo(confidence, ceiling); };

  std::string error;
  if (!ensureConnected(&error)) {
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::TransportUnreachable, reached()));
    return;
  }
  beginJobIo();

  // --- Preflight (docs/techspec.md §5.2 steps 3-4) --------------------------------
  if (options.preflight == PreflightMode::Strict && profile.supports_realtime_status) {
    escpos::Bytes probe;
    {
      std::lock_guard<std::mutex> lock(io_mutex_);
      for (const escpos::DleEotKind kind :
           {escpos::DleEotKind::PrinterStatus, escpos::DleEotKind::OfflineCause,
            escpos::DleEotKind::ErrorCause, escpos::DleEotKind::PaperSensor}) {
        parser_.expectRealtime(kind);
        const escpos::Bytes command = escpos::dleEot(kind);
        probe.insert(probe.end(), command.begin(), command.end());
      }
    }
    const TransportResult sent = sendPaced(probe);
    if (!sent.ok) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached()));
      return;
    }
    std::vector<escpos::ParsedEvent> answers;
    const WaitOutcome outcome = awaitRealtime(4, realtime_timeout, &answers);
    if (outcome == WaitOutcome::LinkDown) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached()));
      return;
    }
    escpos::StatusFlags merged;
    for (const escpos::ParsedEvent& answer : answers) {
      if (answer.flags.online) merged.online = answer.flags.online;
      if (answer.flags.cover_open) merged.cover_open = answer.flags.cover_open;
      if (answer.flags.paper_out) merged.paper_out = answer.flags.paper_out;
      if (answer.flags.paper_near_end) merged.paper_near_end = answer.flags.paper_near_end;
      if (answer.flags.cutter_error) merged.cutter_error = answer.flags.cutter_error;
      if (answer.flags.unrecoverable_error) {
        merged.unrecoverable_error = answer.flags.unrecoverable_error;
      }
    }
    clearRealtime();

    std::optional<FailureReason> refusal;
    if (merged.cover_open.value_or(false)) {
      refusal = FailureReason::PreflightCoverOpen;
    } else if (merged.paper_out.value_or(false)) {
      refusal = FailureReason::PreflightPaperOut;
    } else if (merged.unrecoverable_error.value_or(false) ||
               merged.cutter_error.value_or(false) ||
               (merged.online.has_value() && !*merged.online)) {
      refusal = FailureReason::PreflightHardwareError;
    }
    if (refusal) {
      // Zero payload bytes have been sent, which is what makes this FailedKnown and
      // safe to resubmit rather than Unknown (docs/techspec.md §6).
      terminate(job, JobState::FailedKnown, JobResult::failed(*refusal, reached()));
      return;
    }
    if (!answers.empty()) {
      confidence = raise(confidence, ConfidenceLevel::PrinterHealthy);
      advance(job, JobState::PreflightOk, reached(), FailureReason::None);
    }
    // No answers at all is not evidence of a fault: this printer's LAN module may
    // simply not forward realtime status. The job proceeds without claiming health,
    // and the completion fence below is where the truth comes out.
  }

  const escpos::Bytes wire = buildPayload(payload, options, attempt, job->key());
  const std::string marker_suffix =
      profile.completion == CompletionMechanism::GsParenH ? markers_->acquire()
                                                          : std::string();
  const std::string print_token = marker_suffix.empty() ? "" : "P" + marker_suffix;
  const std::string cut_token = marker_suffix.empty() ? "" : "C" + marker_suffix;
  struct MarkerGuard {
    MarkerAllocator* allocator;
    std::string suffix;
    ~MarkerGuard() {
      if (allocator && !suffix.empty()) {
        allocator->release(suffix);
      }
    }
  } marker_guard{markers_.get(), marker_suffix};

  // --- The ordering rule (docs/techspec.md §5.1) ----------------------------------
  // SendStarted is durable before the socket sees byte one. Everything after this
  // point may be ambiguous; nothing before it can be.
  advance(job, JobState::SendStarted, reached(), FailureReason::None);

  const TransportResult sent = sendPaced(wire);
  if (!sent.ok) {
    if (sent.bytes_written == 0) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached()));
    } else {
      terminate(job, JobState::Unknown,
                JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown});
    }
    return;
  }

  const CutVariant cut = effectiveCut(options.cut, profile);

  escpos::Bytes fence;
  switch (profile.completion) {
    case CompletionMechanism::GsParenH:
      fence = escpos::processIdMarker(print_token);
      break;
    case CompletionMechanism::GsR1: {
      std::lock_guard<std::mutex> lock(io_mutex_);
      parser_.expectQueued();
      fence = escpos::gsPaperStatus();
      break;
    }
    case CompletionMechanism::None:
      // Nothing will ever be waited for, so the cut goes out with the payload rather
      // than after an acknowledgement that is never coming.
      if (cut != CutVariant::None) {
        fence = buildCut(cut, print_token);
      }
      break;
  }
  if (!fence.empty()) {
    const TransportResult fence_sent = sendPaced(fence);
    if (!fence_sent.ok) {
      terminate(job, JobState::Unknown,
                JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown});
      return;
    }
  }
  advance(job, JobState::BytesSent, reached(), FailureReason::None);

  if (profile.completion == CompletionMechanism::None) {
    // The write-only truth (docs/sdk-spec.md §5): bytes reached a buffer somewhere,
    // and this printer can never say more than that.
    terminate(job, JobState::DoneSoftware,
              JobResult::done(clampTo(ConfidenceLevel::TransportAccepted, ceiling)));
    return;
  }

  const WaitOutcome print_ack =
      profile.completion == CompletionMechanism::GsParenH ? awaitToken(print_token, timeout)
                                                          : awaitQueued(timeout);
  if (print_ack != WaitOutcome::Signalled) {
    // Bytes were sent. A timeout here is exactly the case the legacy 5 s DLE EOT
    // check gets wrong (docs/api.md §4): the receipt may well be printing.
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(),
                        print_ack == WaitOutcome::Timeout
                            ? FailureReason::TimeoutAwaitingCompletion
                            : FailureReason::Unknown});
    return;
  }
  confidence = raise(confidence, ConfidenceLevel::PrintConfirmed);
  advance(job, JobState::PrintConfirmed, reached(), FailureReason::None);

  if (cut == CutVariant::None) {
    terminate(job, JobState::DoneSoftware, JobResult::done(reached()));
    return;
  }

  if (profile.completion == CompletionMechanism::GsR1) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    parser_.expectQueued();
  }
  const TransportResult cut_sent = sendPaced(buildCut(cut, cut_token));
  if (!cut_sent.ok) {
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown});
    return;
  }
  const WaitOutcome cut_ack =
      profile.completion == CompletionMechanism::GsParenH ? awaitToken(cut_token, timeout)
                                                          : awaitQueued(timeout);
  if (cut_ack != WaitOutcome::Signalled) {
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(),
                        cut_ack == WaitOutcome::Timeout
                            ? FailureReason::TimeoutAwaitingCompletion
                            : FailureReason::Unknown});
    return;
  }
  confidence = raise(confidence, ConfidenceLevel::CutProcessed);
  advance(job, JobState::CutCommandProcessed, reached(), FailureReason::None);

  if (profile.completion != CompletionMechanism::GsParenH) {
    // A second GS r 1 fences the cut command but is not a documented cutter
    // guarantee (docs/techspec.md §3.2), so the claim stops here: print completion
    // confirmed, cut command processed, no cutter status read.
    terminate(job, JobState::DoneSoftware, JobResult::done(reached()));
    return;
  }

  // Ordered fence first, real-time cutter status second — the only order in which
  // DLE EOT 3 cannot overtake the cut (docs/techspec.md §3.3).
  clearRealtime();
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    parser_.expectRealtime(escpos::DleEotKind::ErrorCause);
  }
  const TransportResult status_sent = sendPaced(escpos::dleEot(escpos::DleEotKind::ErrorCause));
  std::vector<escpos::ParsedEvent> answers;
  if (status_sent.ok) {
    awaitRealtime(1, realtime_timeout, &answers);
  }
  for (const escpos::ParsedEvent& answer : answers) {
    if (answer.flags.cutter_error.value_or(false)) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::CutterFault, reached()));
      return;
    }
  }
  if (!answers.empty()) {
    confidence = raise(confidence, ConfidenceLevel::CutFaultFree);
  }
  // Answers empty means the bit could not be read, so the claim stays at
  // CutProcessed instead of being upgraded on silence.
  terminate(job, JobState::DoneSoftware, JobResult::done(reached()));
}

size_t payloadInputBytes(const Payload& payload) {
  if (std::holds_alternative<RasterPayload>(payload.content)) {
    return std::get<RasterPayload>(payload.content).gray.size();
  }
  if (std::holds_alternative<DocumentPayload>(payload.content)) {
    return std::get<DocumentPayload>(payload.content).bytes.size();
  }
  return std::get<RawPayload>(payload.content).bytes.size();
}

std::shared_ptr<PrintJob> PrinterRuntime::submit(std::shared_ptr<Payload> payload,
                                                 JobOptions options, bool reprint) {
  std::string key = options.key;
  uint32_t attempt = 1;

  std::lock_guard<std::mutex> index_lock(index_->mutex);
  if (!key.empty()) {
    const auto existing = index_->by_key.find(key);
    if (existing != index_->by_key.end()) {
      if (!reprint) {
        // docs/api.md §3: an existing key never prints again, whatever state it is in.
        return existing->second.job;
      }
      attempt = existing->second.attempt + 1;
      if (!payload) {
        payload = existing->second.payload;
      }
    }
  }
  if (!payload) {
    return nullptr;
  }
  if (key.empty()) {
    key = "auto-" + newJobId();
  }

  const std::string job_id = newJobId();
  auto job = std::shared_ptr<PrintJob>(new PrintJob(job_id, key, config_.id, attempt));

  JobRecord record;
  record.id = job_id;
  record.key = key;
  record.printer_id = config_.id;
  record.attempt = attempt;
  record.payload_kind = payload->kind();
  record.payload_bytes = payloadInputBytes(*payload);
  store_->createJob(record);
  job->emit(JobState::Queued, ConfidenceLevel::TransportAccepted, std::nullopt);

  JobEntry entry;
  entry.job = job;
  entry.printer_id = config_.id;
  entry.payload = payload;
  entry.options = options;
  entry.attempt = attempt;
  index_->by_key[key] = entry;

  if (stopping_.load()) {
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::TransportUnreachable,
                                ConfidenceLevel::TransportAccepted));
    return job;
  }

  auto shared_payload = entry.payload;
  Task task;
  task.run = [this, job, shared_payload, options, attempt] {
    runJob(job, *shared_payload, options, attempt);
  };
  task.cancel = [this, job] {
    // Never dequeued, so provably zero bytes on the wire.
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::Unknown, ConfidenceLevel::TransportAccepted));
  };
  push(std::move(task));
  return job;
}

DeviceStatus PrinterRuntime::refreshStatus(std::chrono::milliseconds timeout) {
  if (stopping_.load()) {
    return status();
  }
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  Task task;
  task.run = [this, timeout, done] {
    std::string error;
    if (ensureConnected(&error)) {
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        realtime_answers_.clear();
      }
      escpos::Bytes probe;
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        for (const escpos::DleEotKind kind :
             {escpos::DleEotKind::PrinterStatus, escpos::DleEotKind::OfflineCause,
              escpos::DleEotKind::ErrorCause, escpos::DleEotKind::PaperSensor}) {
          parser_.expectRealtime(kind);
          const escpos::Bytes command = escpos::dleEot(kind);
          probe.insert(probe.end(), command.begin(), command.end());
        }
      }
      if (sendPaced(probe).ok) {
        awaitRealtime(4, timeout, nullptr);
      }
      clearRealtime();
    }
    done->set_value();
  };
  task.cancel = [done] { done->set_value(); };
  push(std::move(task));
  future.wait();
  return status();
}

void PrinterRuntime::openCashDrawer() {
  if (stopping_.load()) {
    return;
  }
  Task task;
  task.run = [this] {
    std::string error;
    if (!ensureConnected(&error)) {
      return;
    }
    escpos::Encoder encoder;
    encoder.kickCashDrawer();
    sendPaced(encoder.bytes());
  };
  push(std::move(task));
}

}  // namespace detail

// --- Printer ---------------------------------------------------------------------

Printer::Printer(std::shared_ptr<detail::PrinterRuntime> runtime)
    : rt_(std::move(runtime)) {}

Printer::~Printer() = default;

const std::string& Printer::id() const noexcept { return rt_->id(); }
uint32_t Printer::widthDots() const noexcept { return rt_->widthDots(); }
const CapabilityProfile& Printer::profile() const noexcept { return rt_->profile(); }

std::shared_ptr<PrintJob> Printer::print(Payload payload, const JobOptions& options) {
  return rt_->submit(std::make_shared<Payload>(std::move(payload)), options, false);
}

std::shared_ptr<PrintJob> Printer::forceReprint(const std::string& key,
                                                const JobOptions& options) {
  JobOptions effective = options;
  effective.key = key;
  return rt_->submit(nullptr, effective, true);
}

std::shared_ptr<PrintJob> Printer::forceReprint(const std::string& key, Payload payload,
                                                const JobOptions& options) {
  JobOptions effective = options;
  effective.key = key;
  return rt_->submit(std::make_shared<Payload>(std::move(payload)), effective, true);
}

DeviceStatus Printer::status() const { return rt_->status(); }

DeviceStatus Printer::refreshStatus(std::chrono::milliseconds timeout) {
  return rt_->refreshStatus(timeout);
}

void Printer::subscribe(DeviceEventCallback callback) {
  rt_->subscribeDevice(std::move(callback));
}

void Printer::openCashDrawer() { rt_->openCashDrawer(); }

void Printer::drain() { rt_->drain(); }

// --- PrinterDriver ---------------------------------------------------------------

PrinterDriver::PrinterDriver(StorageConfig storage)
    : store_(std::make_shared<JobStore>(std::move(storage))),
      markers_(std::make_shared<detail::MarkerAllocator>()) {
  hub_ = std::make_shared<detail::DriverEventHub>();
  index_ = std::make_shared<detail::JobIndex>();
  // Jobs from a previous run come back as terminal handles so findJob(key) works
  // across a restart and a re-submitted key still cannot print twice.
  for (const JobRecord& record : store_->all()) {
    auto job = std::shared_ptr<PrintJob>(
        new PrintJob(record.id, record.key, record.printer_id, record.attempt));
    JobResult outcome;
    switch (record.state) {
      case JobState::DoneSoftware:
      case JobState::PhysicallyVerified:
        outcome = JobResult::done(record.confidence);
        break;
      case JobState::FailedKnown:
        outcome = JobResult::failed(record.reason, record.confidence);
        break;
      default:
        outcome = JobResult{JobOutcome::Unknown, record.confidence, record.reason};
        break;
    }
    job->emit(record.state, record.confidence,
              record.reason == FailureReason::None
                  ? std::optional<FailureReason>()
                  : std::optional<FailureReason>(record.reason));
    job->finish(outcome);
    detail::JobEntry entry;
    entry.job = job;
    entry.printer_id = record.printer_id;
    entry.attempt = record.attempt;
    index_->by_key[record.key] = entry;
  }
}

PrinterDriver::~PrinterDriver() { shutdown(); }

void PrinterDriver::shutdown() {
  std::vector<std::shared_ptr<Printer>> printers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    printers = printers_;
  }
  for (const auto& printer : printers) {
    printer->rt_->stop();
  }
}

std::shared_ptr<Printer> PrinterDriver::addPrinter(PrinterConfig config) {
  if (config.id.empty()) {
    std::unique_ptr<Transport> probe = config.transport ? config.transport() : nullptr;
    config.id = probe ? probe->describe() : ("printer-" + newJobId());
  }
  auto runtime = std::make_shared<detail::PrinterRuntime>(std::move(config), store_,
                                                          markers_, hub_, index_);
  runtime->start();
  auto printer = std::shared_ptr<Printer>(new Printer(runtime));
  std::lock_guard<std::mutex> lock(mutex_);
  printers_.push_back(printer);
  return printer;
}

std::shared_ptr<Printer> PrinterDriver::printer(const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& candidate : printers_) {
    if (candidate->id() == printer_id) {
      return candidate;
    }
  }
  return nullptr;
}

std::vector<std::shared_ptr<Printer>> PrinterDriver::printers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return printers_;
}

std::shared_ptr<PrintJob> PrinterDriver::findJob(const std::string& key) const {
  std::lock_guard<std::mutex> lock(index_->mutex);
  const auto it = index_->by_key.find(key);
  return it == index_->by_key.end() ? nullptr : it->second.job;
}

std::shared_ptr<PrintJob> PrinterDriver::forceReprint(const std::string& key,
                                                      const JobOptions& options) {
  std::string printer_id;
  {
    std::lock_guard<std::mutex> lock(index_->mutex);
    const auto it = index_->by_key.find(key);
    if (it == index_->by_key.end()) {
      return nullptr;
    }
    printer_id = it->second.printer_id;
  }
  auto target = printer(printer_id);
  if (!target) {
    return nullptr;
  }
  // Returns nullptr when the original job came back from the journal: those records
  // carry what happened to a job, never what it contained, so a byte-identical
  // reprint needs the caller to supply the payload again.
  return target->forceReprint(key, options);
}

std::shared_ptr<PrintJob> PrinterDriver::forceReprint(const std::string& key,
                                                      Payload payload,
                                                      const JobOptions& options) {
  std::string printer_id;
  {
    std::lock_guard<std::mutex> lock(index_->mutex);
    const auto it = index_->by_key.find(key);
    if (it != index_->by_key.end()) {
      printer_id = it->second.printer_id;
    }
  }
  auto target = printer_id.empty() ? nullptr : printer(printer_id);
  if (!target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (printers_.empty()) {
      return nullptr;
    }
    target = printers_.front();
  }
  return target->forceReprint(key, std::move(payload), options);
}

void PrinterDriver::subscribeDevices(DriverDeviceEventCallback callback) {
  std::lock_guard<std::mutex> lock(hub_->mutex);
  hub_->subscribers.push_back(std::move(callback));
}

}  // namespace pd
