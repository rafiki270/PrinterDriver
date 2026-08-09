#include "printerdriver/driver.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

#include "printerdriver/response_parser.hpp"
// M13b: the Star engine (docs/wire-protocols.md §2). A second command language driven by
// the same runtime, the same journal and the same confidence grading — the point of
// keeping the fence logic here rather than in a transport is that neither engine can
// weaken the other's guarantees, and both end up in one evidence document.
#include "printerdriver/star.hpp"

namespace pd {

const char kReprintBannerLine[] = "*** REPRINT / POSSIBLE DUPLICATE ***";
const char kReprintAttemptPrefix[] = "PRINT ATTEMPT: ";
const char kOrderPrefix[] = "ORDER: ";
const char kVerificationPrefix[] = "V:";

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

// The verification identifier alphabet (docs/api.md §14): the 94 printable ASCII
// characters excluding the space, so a token is one unambiguous word on paper, survives
// a journal field and satisfies escpos::isValidProcessIdToken.
constexpr char kTokenAlphabetFirst = '!';  // 0x21
constexpr uint32_t kTokenAlphabetSize = 94u;
constexpr uint32_t kTokenSequenceSpace = kTokenAlphabetSize * kTokenAlphabetSize;  // 8836

std::string encodeBase94(uint32_t value) {
  const uint32_t wrapped = value % kTokenSequenceSpace;
  std::string out(2, kTokenAlphabetFirst);
  out[0] = static_cast<char>(kTokenAlphabetFirst + (wrapped / kTokenAlphabetSize));
  out[1] = static_cast<char>(kTokenAlphabetFirst + (wrapped % kTokenAlphabetSize));
  return out;
}

bool isTokenAlphabet(char c) noexcept {
  return c >= kTokenAlphabetFirst &&
         c < static_cast<char>(kTokenAlphabetFirst + kTokenAlphabetSize);
}

std::string randomNonce() {
  static std::mutex mutex;
  static std::mt19937_64 engine(std::random_device{}());
  std::lock_guard<std::mutex> lock(mutex);
  std::string out(2, kTokenAlphabetFirst);
  for (char& c : out) {
    c = static_cast<char>(kTokenAlphabetFirst + (engine() % kTokenAlphabetSize));
  }
  return out;
}

// The nonce is what makes a token name *this* driver instance rather than this run, so
// it outlives the process: yesterday's receipt has to keep resolving after a restart.
std::string loadOrCreateNonce(const std::string& directory) {
  if (directory.empty()) {
    return randomNonce();  // in-memory driver: nowhere to keep it
  }
  std::string path = directory;
  if (path.back() != '/') {
    path += '/';
  }
  path += "instance.nonce";
  {
    std::ifstream input(path);
    std::string stored;
    if (input && std::getline(input, stored) && stored.size() == 2 &&
        isTokenAlphabet(stored[0]) && isTokenAlphabet(stored[1])) {
      return stored;
    }
  }
  const std::string nonce = randomNonce();
  // Temp-file + rename, like the journal's compact(): a crash mid-write can no longer
  // leave a partial nonce that silently rotates the instance identity on next boot,
  // orphaning every V: token already on paper.
  const std::string temp = path + ".tmp";
  {
    std::ofstream output(temp, std::ios::trunc);
    if (!output) {
      // A directory that cannot be written still gets a working driver: the tokens
      // simply stop being resolvable across a restart — a diagnostic loss, not a
      // printing one.
      return nonce;
    }
    output << nonce << "\n";
  }
  std::rename(temp.c_str(), path.c_str());
  // Two constructors racing on one store directory (out of contract — the journal has
  // a single owner, docs/sdk-spec.md §14 — but cheap to soften): re-read after the
  // rename so both racers converge on whichever write landed, instead of each keeping
  // a private nonce and flagging the other's echoes as a foreign writer.
  {
    std::ifstream reread(path);
    std::string stored;
    if (reread && std::getline(reread, stored) && stored.size() == 2 &&
        isTokenAlphabet(stored[0]) && isTokenAlphabet(stored[1])) {
      return stored;
    }
  }
  return nonce;
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

std::string PrintJob::printToken() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return print_token_;
}

std::string PrintJob::cutToken() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cut_token_;
}

void PrintJob::setTokens(const std::string& print_token, const std::string& cut_token) {
  std::lock_guard<std::mutex> lock(mutex_);
  print_token_ = print_token;
  cut_token_ = cut_token;
}

void PrintJob::subscribe(EventCallback callback) {
  if (!callback) {
    return;
  }
  auto subscriber = std::make_shared<Subscriber>();
  subscriber->callback = std::move(callback);
  std::vector<JobEvent> replay;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    replay = history_;
    subscribers_.push_back(subscriber);
  }
  // Registered but still draining, so a concurrent emit() parks its events instead
  // of invoking the callback: nothing can overtake this replay.
  for (const JobEvent& event : replay) {
    subscriber->callback(event);
  }
  // Deliver whatever was parked mid-replay, then go live. The flag only flips in
  // the critical section that finds the backlog empty, so no event can slip
  // between the last drained batch and the first live delivery.
  std::vector<JobEvent> parked;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (subscriber->pending.empty()) {
        subscriber->draining = false;
        break;
      }
      parked.swap(subscriber->pending);
    }
    for (const JobEvent& event : parked) {
      subscriber->callback(event);
    }
    parked.clear();
  }
}

void PrintJob::emit(JobState state, ConfidenceLevel confidence,
                    std::optional<FailureReason> reason) {
  const JobEvent event = JobEvent::make(state, confidence, reason);
  std::vector<std::shared_ptr<Subscriber>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.store(state);
    confidence_.store(confidence);
    history_.push_back(event);
    live.reserve(subscribers_.size());
    for (const std::shared_ptr<Subscriber>& subscriber : subscribers_) {
      if (subscriber->draining) {
        // Parked in the same critical section that recorded the event, so the
        // backlog is in history order; subscribe() delivers it before going live.
        subscriber->pending.push_back(event);
      } else {
        live.push_back(subscriber);
      }
    }
  }
  for (const std::shared_ptr<Subscriber>& subscriber : live) {
    subscriber->callback(event);
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

// Verification identifiers (docs/api.md §14, docs/techspec.md §5.2). Four printable
// characters: `[2-char per-instance nonce][2-char job sequence]`. The nonce says which
// driver instance owns the echo — that is what makes a foreign writer identifiable on
// the paper and on the wire — and the sequence says which job.
//
// A job takes two adjacent sequence values, an even one for its print fence and the odd
// successor for its cut fence, so P and C stay distinguishable inside the fixed
// four-character layout without spending a character on a discriminator. Both are held
// from the moment the job record is minted until the job is terminal, so no outstanding
// marker can ever be answered by a later job. 8 836 sequences ⇒ 4 418 jobs per wrap.
class MarkerAllocator {
 public:
  struct Pair {
    std::string print_token;
    std::string cut_token;
  };

  explicit MarkerAllocator(std::string nonce) : nonce_(std::move(nonce)) {}

  const std::string& nonce() const noexcept { return nonce_; }

  Pair acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < kTokenSequenceSpace / 2u; ++i) {
      const uint32_t slot = next_++;
      Pair pair{nonce_ + encodeBase94(slot * 2u), nonce_ + encodeBase94(slot * 2u + 1u)};
      if (in_use_.count(pair.print_token) != 0 || in_use_.count(pair.cut_token) != 0) {
        continue;
      }
      in_use_.insert(pair.print_token);
      in_use_.insert(pair.cut_token);
      return pair;
    }
    throw std::runtime_error("no free completion marker tokens");
  }

  void release(const Pair& pair) {
    std::lock_guard<std::mutex> lock(mutex_);
    in_use_.erase(pair.print_token);
    in_use_.erase(pair.cut_token);
  }

  bool isOutstanding(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_use_.count(token) != 0;
  }

  // Whether an echoed token could have come from this driver at all. Deliberately the
  // nonce rather than the outstanding set: a printer that answers a marker after the
  // job it belonged to has already timed out is late, not foreign, and reporting a
  // multi-writer violation there would cry wolf on the one case Unknown exists for.
  bool isOurs(const std::string& token) const noexcept {
    return token.size() == 4 && token.compare(0, 2, nonce_) == 0;
  }

  size_t outstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_use_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::string nonce_;
  uint32_t next_ = 0;
  std::set<std::string> in_use_;
};

// Holds a job's token pair for exactly as long as the work that will print it exists.
// Both the run and the cancel closure capture one, so a job that is dequeued and a job
// that is thrown away at shutdown release identically, with no explicit call anywhere.
struct MarkerLease {
  std::shared_ptr<MarkerAllocator> allocator;
  MarkerAllocator::Pair pair;

  MarkerLease(std::shared_ptr<MarkerAllocator> owner, MarkerAllocator::Pair tokens)
      : allocator(std::move(owner)), pair(std::move(tokens)) {}
  MarkerLease(const MarkerLease&) = delete;
  MarkerLease& operator=(const MarkerLease&) = delete;
  ~MarkerLease() {
    if (allocator) {
      allocator->release(pair);
    }
  }
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
  // Paper → job (docs/api.md §14). Both of a job's tokens land here, oldest first, and
  // a lookup answers with the newest holder: after a sequence wrap the receipt in an
  // operator's hand is far more likely to be the recent one, and the journal timestamp
  // settles the rest. Strong references, because an older attempt's job is no longer
  // reachable through by_key but its printed token must still resolve.
  std::unordered_map<std::string, std::vector<std::shared_ptr<PrintJob>>> by_token;

  void registerToken(const std::string& token, const std::shared_ptr<PrintJob>& job) {
    if (token.empty() || !job) {
      return;
    }
    by_token[token].push_back(job);
  }
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
                 std::shared_ptr<DriverEventHub> hub, std::shared_ptr<JobIndex> index,
                 std::shared_ptr<FindingsStore> capabilities,
                 std::shared_ptr<DrawerPolarityStore> drawer_polarities)
      : config_(std::move(config)),
        store_(std::move(store)),
        markers_(std::move(markers)),
        hub_(std::move(hub)),
        index_(std::move(index)),
        capabilities_(std::move(capabilities)),
        drawer_polarities_(std::move(drawer_polarities)) {}

  ~PrinterRuntime() { stop(); }

  void start() {
    applyStoredFindings();
    applyStoredDrawerPolarity();  // M14
    worker_ = std::thread([this] { workerLoop(); });
    // Queued before any job can be, so a promoted profile is in force by the time the
    // first receipt is built rather than one receipt too late.
    scheduleProbe();
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
  CapabilityProfile profile() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    return config_.profile;
  }
  std::optional<CapabilityFindings> findings() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    return findings_;
  }
  uint32_t widthDots() const noexcept { return config_.width_dots; }

  // A null payload is only valid for a reprint, where the original submission's
  // payload is reused. `banner` is only ever true on a reprint that asked for one.
  std::shared_ptr<PrintJob> submit(std::shared_ptr<Payload> payload, JobOptions options,
                                   bool reprint, bool banner);

  // Addon hooks (docs/sdk-spec.md §12); see the comments on Printer::reserveJob.
  std::shared_ptr<PrintJob> reserve(const std::string& key, PayloadKind kind,
                                    uint64_t payload_bytes, bool* created);
  void submitReserved(const std::shared_ptr<PrintJob>& job,
                      std::shared_ptr<Payload> payload, JobOptions options);

  DeviceStatus status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
  }

  DeviceStatus refreshStatus(std::chrono::milliseconds timeout);
  void openCashDrawer();
  void drain();

  // --- M14: cash drawer -------------------------------------------------------------
  DrawerOpenResult openDrawer(const DrawerRequest& request);
  DrawerReading readDrawerSensor(std::chrono::milliseconds timeout);
  bool calibrateDrawerPolarity(bool high_means_open);
  DrawerPolarity drawerPolarity() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    return config_.profile.drawer.status.polarity;
  }
  // --- end M14 ----------------------------------------------------------------------

  void subscribeDevice(DeviceEventCallback callback) {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    device_subscribers_.push_back(std::move(callback));
  }

 private:
  void workerLoop();
  void push(Task task);
  // M14. The peripheral lane: the same worker, so a pulse can never interleave with a
  // fenced job's bytes, but with a say in where it lands relative to jobs that are
  // merely queued. See openDrawer() for why canKickDuringPrint decides that.
  void pushPeripheral(Task task, bool ahead_of_queued_jobs);

  bool ensureConnected(std::string* error);
  void closeTransport();

  void applyStoredFindings();
  void scheduleProbe();
  void runProbe();

  // --- M14 ---
  void applyStoredDrawerPolarity();
  // Records a drawer sense level observed anywhere: a GS r 2 answer, an ASB frame, or
  // a DLE EOT 1 answer taken during preflight. The verification window watches this.
  std::optional<bool> drawerPin() const;
  // Sends GS r 2 and waits for the queued answer. Runs on the worker thread only.
  std::optional<bool> pollDrawerPin(const CapabilityProfile& profile,
                                    std::chrono::milliseconds timeout);
  // The whole opening sequence, on the worker thread.
  DrawerOpenResult runDrawerSequence(const CapabilityProfile& profile,
                                     const DrawerRequest& request);
  // --- end M14 ---

  void runJob(const std::shared_ptr<PrintJob>& job, const Payload& payload,
              const JobOptions& options, uint32_t attempt, bool banner);

  // --- M13b: Star (docs/wire-protocols.md §2) --------------------------------------
  // A separate job path, not a branch inside runJob(). The two languages disagree about
  // what ESC d means, so every step from the first byte to the fence differs; sharing one
  // function would be a switch statement in every paragraph of it.
  void runStarJob(const std::shared_ptr<PrintJob>& job, const Payload& payload,
                  const JobOptions& options, uint32_t attempt, bool banner);
  escpos::Bytes buildStarPayload(const CapabilityProfile& profile, const Payload& payload,
                                 const JobOptions& options, uint32_t attempt,
                                 const std::string& key, bool banner,
                                 std::vector<std::string>* dropped) const;
  // Arms the next fence and returns the bytes that carry it. Arming before sending is not
  // an optimisation: the answer can arrive inside the very write that asks for it.
  escpos::Bytes armStarFence(const CapabilityProfile& profile);
  WaitOutcome awaitStarFence(std::chrono::milliseconds timeout);

  // Mints this attempt's verification identifiers and makes them durable and
  // resolvable before a byte carrying them can leave. Returns an empty lease when the
  // profile has no GS ( H fence, and therefore no wire token to promote.
  std::unique_ptr<MarkerLease> leaseTokens(const std::shared_ptr<PrintJob>& job,
                                           const CapabilityProfile& profile);

  void beginJobIo();
  WaitOutcome awaitToken(const std::string& token, std::chrono::milliseconds timeout);
  // `out_byte`, when non-null, receives the raw answer byte the expectation consumed.
  // The fence path ignores it — a GS r 1 answer is a completion signal and nothing
  // else — while the drawer path (M14) reads bit 0 of a GS r 2 answer out of it.
  WaitOutcome awaitQueued(std::chrono::milliseconds timeout, uint8_t* out_byte = nullptr);
  WaitOutcome awaitRealtime(size_t count, std::chrono::milliseconds timeout,
                            std::vector<escpos::ParsedEvent>* out);
  void clearRealtime();

  void onBytes(const uint8_t* data, size_t size);
  void onDisconnected(TransportError error, const std::string& message);
  void mergeStatus(const escpos::StatusFlags& flags, std::vector<DeviceEvent>* out);
  void dispatchDeviceEvents(const std::vector<DeviceEvent>& events);

  // `evidence` defaults to none: most transitions are mid-flight and have not earned
  // any yet. terminate() is the one caller that has something to pass.
  void advance(const std::shared_ptr<PrintJob>& job, JobState state,
               ConfidenceLevel confidence, FailureReason reason,
               const JobEvidence& evidence = {});
  void terminate(const std::shared_ptr<PrintJob>& job, JobState state,
                 const JobResult& outcome);

  escpos::Bytes buildPayload(const CapabilityProfile& profile, const Payload& payload,
                             const JobOptions& options, uint32_t attempt,
                             const std::string& key, bool banner,
                             const std::string& print_token) const;
  escpos::Bytes buildCut(const CapabilityProfile& profile, const JobOptions& options,
                         CutVariant variant, const std::string& marker_token) const;
  TransportResult sendPaced(const CapabilityProfile& profile,
                            const escpos::Bytes& bytes);

  PrinterConfig config_;
  mutable std::mutex profile_mutex_;
  std::optional<CapabilityFindings> findings_;
  std::shared_ptr<JobStore> store_;
  std::shared_ptr<MarkerAllocator> markers_;
  std::shared_ptr<DriverEventHub> hub_;
  std::shared_ptr<JobIndex> index_;
  std::shared_ptr<FindingsStore> capabilities_;
  std::shared_ptr<CapabilityProbe> probe_;
  mutable std::mutex probe_mutex_;
  // M14. Shared with the driver; the polarity is per printer id.
  std::shared_ptr<DrawerPolarityStore> drawer_polarities_;
  // M14. Guarded by status_mutex_ alongside status_, because both are fed from the
  // same decoded frames.
  std::optional<bool> drawer_pin_;
  // M14. When the last pulse left, so the manufacturer cooldown can be enforced
  // before the next one. Guarded by queue_mutex_; only the worker writes it.
  MonotonicTime last_drawer_pulse_{};
  bool drawer_pulsed_ = false;

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
  std::deque<uint8_t> queued_bytes_;  // M14; see awaitQueued's out_byte
  std::vector<escpos::ParsedEvent> realtime_answers_;
  bool link_down_ = false;

  // --- M13b: Star fence state, all under io_mutex_ -----------------------------------
  star::ResponseParser star_parser_;
  // Which parser owns the backchannel. A Star printer never answers an ESC/POS frame and
  // vice versa, so routing by profile is exact rather than a guess.
  bool star_mode_ = false;
  bool star_fence_outstanding_ = false;
  bool star_fence_signalled_ = false;
  bool star_fence_is_etb_ = false;
  uint8_t star_expect_n1_ = 0;
  uint8_t star_expect_n2_ = 0;
  uint8_t star_expect_counter_ = 0;
  // The last ETB counter this driver has seen. Unset until the counter is cleared at the
  // start of a session, because "we have never looked" and "it is zero" are different.
  std::optional<uint8_t> star_counter_;
  uint16_t star_sequence_ = 0;

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

// M14. There is one worker per printer and it runs a task to completion, so a drawer
// pulse can never land between two bytes of a receipt whatever this does — the "no
// receipt/cutter/drawer interleaving" rule of docs/cash-drawer.md is a property of the
// lane, not of a flag. What the flag decides is the *ordering against jobs that are
// only queued*: a printer whose drawer output may fire while the mechanism prints has
// no reason to make a cash tender wait behind three kitchen tickets, so its pulse goes
// in front of them. A printer whose drawer output cannot fire while printing (the
// Citizen quirk, §3) is serialised strictly behind everything already queued, which is
// the daemon behaviour that document asks for: finish the receipt, then pulse.
void PrinterRuntime::pushPeripheral(Task task, bool ahead_of_queued_jobs) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (ahead_of_queued_jobs) {
      tasks_.push_front(std::move(task));
    } else {
      tasks_.push_back(std::move(task));
    }
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

void PrinterRuntime::applyStoredFindings() {
  if (config_.probe == ProbePolicy::Never || !capabilities_ ||
      !capabilities_->persistent()) {
    return;
  }
  // Nothing is known about the identity until something has interrogated the device,
  // so a stored record is found by endpoint on this path and re-keyed by identity
  // once a probe has run.
  std::optional<CapabilityFindings> stored = capabilities_->findByEndpoint(config_.id);
  if (!stored) {
    return;
  }
  std::lock_guard<std::mutex> lock(profile_mutex_);
  config_.profile = promote(config_.profile, *stored);
  findings_ = std::move(stored);
}

void PrinterRuntime::scheduleProbe() {
  if (config_.probe == ProbePolicy::Never || config_.probe == ProbePolicy::UseStored) {
    return;
  }
  if (config_.probe == ProbePolicy::IfUnknown) {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    if (findings_.has_value()) {
      return;
    }
  }
  Task task;
  task.run = [this] { runProbe(); };
  push(std::move(task));
}

void PrinterRuntime::runProbe() {
  std::string error;
  if (!ensureConnected(&error)) {
    return;
  }
  ProbeOptions options = config_.probe_options;
  options.endpoint = config_.id;
  options.hints = config_.identity_hints;
  auto probe = std::make_shared<CapabilityProbe>(options);
  {
    std::lock_guard<std::mutex> lock(probe_mutex_);
    probe_ = probe;
  }
  CapabilityProfile snapshot = profile();
  CapabilityFindings findings = probe->run([this, &snapshot](const escpos::Bytes& bytes) {
    return sendPaced(snapshot, bytes).ok;
  });
  {
    std::lock_guard<std::mutex> lock(probe_mutex_);
    probe_.reset();
  }
  if (findings.empty()) {
    return;
  }
  findings.endpoint = config_.id;
  {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    config_.profile = promote(config_.profile, findings);
    findings_ = findings;
  }
  if (capabilities_ && capabilities_->persistent()) {
    capabilities_->save(findings);
  }
}

void PrinterRuntime::onBytes(const uint8_t* data, size_t size) {
  {
    // While the probe owns the conversation it owns the whole backchannel: its
    // questions are the only ones outstanding, and its parser has the expectations.
    std::shared_ptr<CapabilityProbe> probe;
    {
      std::lock_guard<std::mutex> lock(probe_mutex_);
      probe = probe_;
    }
    if (probe) {
      probe->onBytes(data, size);
      return;
    }
  }
  std::vector<DeviceEvent> events;
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    // --- M13b: the Star backchannel (docs/wire-protocols.md §2) ---------------------
    if (star_mode_) {
      for (const star::Event& event : star_parser_.feed(data, size)) {
        switch (event.kind) {
          case star::EventKind::EtxAck:
            // ESC GS ETX replies only to the issuing session and echoes the correlation
            // bytes it was handed, so an answer that does not carry ours is not ours.
            if (star_fence_outstanding_ && !star_fence_is_etb_ &&
                event.n1 == star_expect_n1_ && event.n2 == star_expect_n2_) {
              star_fence_signalled_ = true;
            } else {
              events.push_back(DeviceEvent::ForeignWriterDetected);
            }
            break;
          case star::EventKind::AsbStatus: {
            // THE MISATTRIBUTION GUARD. On TCP 9100 the ASB frame carrying the ETB
            // counter is broadcast to *every* connected host, so a counter that moved is
            // not evidence that our data finished — it is evidence that somebody's data
            // finished. A change is only ever accepted as our completion when we have a
            // fence outstanding AND the counter landed on exactly the value that fence
            // was expecting. Anything else is reported as a foreign writer and confirms
            // nothing, which fails our job Unknown: the correct answer for a receipt
            // whose fate we cannot establish, and the whole reason ETB is gated behind
            // an exclusive session in the first place.
            const bool changed =
                !star_counter_.has_value() || event.counter != *star_counter_;
            if (!changed) {
              break;
            }
            const bool ours = star_fence_outstanding_ && star_fence_is_etb_ &&
                              event.counter == star_expect_counter_;
            star_counter_ = event.counter;
            if (ours) {
              star_fence_signalled_ = true;
            } else {
              events.push_back(DeviceEvent::ForeignWriterDetected);
            }
            break;
          }
          case star::EventKind::UnknownByte:
            break;
        }
      }
    } else {
      for (escpos::ParsedEvent& event : parser_.feed(data, size)) {
        switch (event.kind) {
          case escpos::ParsedEventKind::GsHAck:
            // A structurally valid frame whose token carries somebody else's instance
            // nonce is somebody else's receipt (docs/api.md §14, docs/sdk-spec.md §14).
            // It is reported and dropped: attributing it would let a second writer's
            // printer finish one of our jobs, which is the failure the one-owner rule
            // exists to prevent. Nothing waiting is consumed either way — this branch
            // never touches a queued or realtime expectation.
            if (markers_ && !markers_->isOurs(event.token)) {
              events.push_back(DeviceEvent::ForeignWriterDetected);
              break;
            }
            gsh_tokens_.push_back(event.token);
            break;
          case escpos::ParsedEventKind::QueuedStatus:
            ++queued_answers_;
            // M14. The byte itself, kept alongside the count: a GS r 1 answer is a
            // completion signal whose value nothing reads, and a GS r 2 answer is a
            // drawer sense level that is nothing but its value.
            queued_bytes_.push_back(event.byte);
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
    // M14. ASB reports drawer-connector changes without being asked, and DLE EOT 1
    // carries the same bit, so the drawer's verification window gets its answer from
    // whichever arrives first rather than only from its own polls.
    if (flags.drawer_pin_high) drawer_pin_ = flags.drawer_pin_high;
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
  queued_bytes_.clear();  // M14
  realtime_answers_.clear();
  link_down_ = false;
  // M13b. star_counter_ is deliberately not cleared here: it is session state, not job
  // state, and forgetting the last counter between jobs would make every second fence
  // expect a baseline it has no reason to believe.
  star_parser_.reset();
  star_mode_ = false;
  star_fence_outstanding_ = false;
  star_fence_signalled_ = false;
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

WaitOutcome PrinterRuntime::awaitQueued(std::chrono::milliseconds timeout,
                                        uint8_t* out_byte) {
  std::unique_lock<std::mutex> lock(io_mutex_);
  io_cv_.wait_for(lock, timeout, [this] {
    return stopping_.load() || link_down_ || queued_answers_ > 0;
  });
  if (queued_answers_ > 0) {
    --queued_answers_;
    if (!queued_bytes_.empty()) {
      if (out_byte != nullptr) {
        *out_byte = queued_bytes_.front();
      }
      queued_bytes_.pop_front();
    }
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

// --- M13b: Star fences (docs/wire-protocols.md §2) ----------------------------------

escpos::Bytes PrinterRuntime::armStarFence(const CapabilityProfile& profile) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  star_fence_signalled_ = false;
  star_fence_outstanding_ = true;
  if (profile.completion == CompletionMechanism::StarEtb) {
    star_fence_is_etb_ = true;
    // The value this fence will produce. Modelled explicitly because the counter is five
    // bits and wraps 31 -> 0: an implementation that just compares "bigger than before"
    // stops confirming once per 32 receipts, at 3 a.m., on the busiest printer.
    star_expect_counter_ = star::nextEtbCounter(star_counter_.value_or(0));
    return star::etbFence();
  }
  star_fence_is_etb_ = false;
  // Correlation bytes. They come back verbatim, which is what turns "a printer finished
  // something" into "the data this call sent finished". Sequenced from 1 so that a zeroed
  // buffer can never look like a valid answer.
  ++star_sequence_;
  star_expect_n1_ = static_cast<uint8_t>((star_sequence_ >> 8) & 0xFFu);
  star_expect_n2_ = static_cast<uint8_t>(star_sequence_ & 0xFFu);
  return star::escGsEtxFence(star_expect_n1_, star_expect_n2_);
}

WaitOutcome PrinterRuntime::awaitStarFence(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(io_mutex_);
  io_cv_.wait_for(lock, timeout, [this] {
    return stopping_.load() || link_down_ || star_fence_signalled_;
  });
  const bool signalled = star_fence_signalled_;
  // Disarmed either way. A late answer to a fence whose job has already given up must not
  // be handed to the next job, which is the same rule the GS ( H marker lease enforces.
  star_fence_outstanding_ = false;
  star_fence_signalled_ = false;
  if (signalled) {
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
                             ConfidenceLevel confidence, FailureReason reason,
                             const JobEvidence& evidence) {
  // Persist first, then publish: an observer must never learn of a transition the
  // journal has not committed (docs/techspec.md §5.1).
  store_->recordState(job->id(), state, confidence, reason, evidence);
  job->emit(state, confidence,
            reason == FailureReason::None ? std::optional<FailureReason>()
                                          : std::optional<FailureReason>(reason));
}

void PrinterRuntime::terminate(const std::shared_ptr<PrintJob>& job, JobState state,
                               const JobResult& outcome) {
  // The evidence rides with the same transition that carries the outcome, so a
  // reload never has to guess what a completed job actually earned (docs/techspec.md
  // §5.1, docs/device-database.md "Confidence grades for every route").
  advance(job, state, outcome.confidence, outcome.reason,
         JobEvidence{outcome.grade, outcome.authority, outcome.method.c_str()});
  job->finish(outcome);
}

escpos::Bytes PrinterRuntime::buildPayload(const CapabilityProfile& profile,
                                           const Payload& payload,
                                           const JobOptions& options, uint32_t attempt,
                                           const std::string& key, bool banner,
                                           const std::string& print_token) const {
  const escpos::CodePage code_page =
      std::holds_alternative<DocumentPayload>(payload.content)
          ? std::get<DocumentPayload>(payload.content).code_page
          : profile.code_page;

  escpos::Encoder encoder;
  // The core owns job framing: every job starts from a known printer state, which is
  // also what makes a raw payload's trailing fence attributable (docs/api.md §3).
  encoder.initialize();
  encoder.selectCodePage(code_page);

  // Top margin (docs/receipt-dsl.md "Margins"): blank paper before the first content
  // line, ahead of even the reprint banner, because it is tear-off clearance for the
  // whole ticket rather than spacing for part of it.
  if (options.top_feed_dots > 0) {
    encoder.feedDots(options.top_feed_dots);
  }

  // The banner marks a deliberate duplicate, so it belongs to forceReprint and not to
  // the attempt counter: a fresh attempt after a FailedKnown printed nothing, and
  // warning about a duplicate that does not exist trains operators to ignore it.
  if (banner) {
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

  // The verification trailer (docs/api.md §14): the printed half of the receipt
  // verification identifier. It is the last content on the ticket, so an operator
  // holding the paper reads the code next to the order it belongs to, and the same
  // string goes into the QR so a scanner produces exactly what the eye can check.
  // Nothing is printed when the profile has no wire token to promote.
  if (options.print_verification_id && !print_token.empty()) {
    std::string line;
    if (!key.empty()) {
      line = std::string(kOrderPrefix) + key + "  ";
    }
    line += std::string(kVerificationPrefix) + print_token;
    encoder.feed()
        .align(escpos::Alignment::Center)
        .line(line)
        .qr(line)
        .align(escpos::Alignment::Left);
  }

  if (options.open_drawer) {
    encoder.kickCashDrawer();
  }
  // The fence attaches to the last print operation, so the job has to end in one.
  encoder.feedLines(profile.final_feed_lines);
  return encoder.take();
}

escpos::Bytes PrinterRuntime::buildCut(const CapabilityProfile& profile,
                                       const JobOptions& options, CutVariant variant,
                                       const std::string& marker_token) const {
  escpos::Encoder encoder;
  // The print head sits ahead of the blade, so the guarantee is: at cut time, at
  // least head_to_cutter_feed_dots of feed has occurred since the last printed
  // content. This rides on top of final_feed_lines rather than replacing it, right
  // before the cut command and the fence that follows it (docs/testing-plan.md).
  //
  // A caller's bottom margin (docs/receipt-dsl.md "Margins") can only widen that gap:
  // max, never min, so asking for whitespace is always granted and asking for less
  // than the blade clearance is silently refused rather than clipping the ticket.
  encoder.feedDots(std::max(profile.media.head_to_cutter_feed_dots,
                            options.bottom_feed_dots));
  encoder.useCutWithFeed(profile.quirks.extra_feed_before_cut > 0,
                         profile.quirks.extra_feed_before_cut);
  encoder.cut(variant == CutVariant::Full ? escpos::CutMode::Full
                                          : escpos::CutMode::Partial);
  switch (profile.completion) {
    case CompletionMechanism::GsParenH:
      encoder.processId(marker_token);
      break;
    case CompletionMechanism::GsR1:
      encoder.queuedPaperStatus();
      break;
    case CompletionMechanism::VendorIdle:
    case CompletionMechanism::EposJobId:
    case CompletionMechanism::StarCheckedBlock:
    // M13b: the Star fences are not ESC/POS bytes and never ride on an ESC/POS cut. A
    // Star job is built by buildStarPayload and fenced by armStarFence; this function is
    // only ever reached on the ESC/POS path.
    case CompletionMechanism::StarEtb:
    case CompletionMechanism::StarEscGsEtx:
    case CompletionMechanism::None:
      break;
  }
  return encoder.take();
}

TransportResult PrinterRuntime::sendPaced(const CapabilityProfile& profile,
                                          const escpos::Bytes& bytes) {
  Transport* transport = nullptr;
  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    transport = transport_.get();
  }
  if (transport == nullptr) {
    return TransportResult::failure(TransportError::NotConnected, "no transport");
  }
  const size_t chunk = profile.chunk_bytes;
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
    if (offset < bytes.size() && profile.inter_chunk_delay_ms > 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(profile.inter_chunk_delay_ms));
    }
  }
  return TransportResult::success(offset);
}

std::unique_ptr<MarkerLease> PrinterRuntime::leaseTokens(
    const std::shared_ptr<PrintJob>& job, const CapabilityProfile& profile) {
  if (!markers_ || profile.completion != CompletionMechanism::GsParenH) {
    return nullptr;
  }
  auto lease = std::unique_ptr<MarkerLease>(new MarkerLease(markers_, markers_->acquire()));
  job->setTokens(lease->pair.print_token, lease->pair.cut_token);
  // Durable before the wire, for the same reason SendStarted is: a receipt whose `V:`
  // code is not in the journal is a piece of paper nobody can resolve (docs/api.md
  // §14). Journaled here rather than at submission because the effective profile — and
  // with it whether there is a wire token at all — is only settled once a worker has
  // taken the job and a promoting probe has finished.
  store_->recordTokens(job->id(), lease->pair.print_token, lease->pair.cut_token);
  {
    std::lock_guard<std::mutex> index_lock(index_->mutex);
    index_->registerToken(lease->pair.print_token, job);
    index_->registerToken(lease->pair.cut_token, job);
  }
  return lease;
}

void PrinterRuntime::runJob(const std::shared_ptr<PrintJob>& job, const Payload& payload,
                            const JobOptions& options, uint32_t attempt, bool banner) {
  // One snapshot for the whole job: a probe may have replaced the effective profile,
  // and a job must not be built under one set of capabilities and fenced under another.
  const CapabilityProfile profile = this->profile();
  const ConfidenceLevel ceiling = profile.maxConfidence();
  const auto timeout = std::chrono::milliseconds(
      options.timeout_ms != 0 ? options.timeout_ms : profile.completion_timeout_ms);
  const auto realtime_timeout = std::chrono::milliseconds(profile.preflight_timeout_ms);
  ConfidenceLevel confidence = ConfidenceLevel::TransportAccepted;
  const auto reached = [&] { return clampTo(confidence, ceiling); };
  // Grades a failure or an Unknown honestly: nothing was confirmed, so the claim is
  // transport-only whatever fence the profile would have used. The two differ in
  // whether the link ever accepted a byte, which is exactly what an operator needs
  // to know before deciding about a reprint.
  const JobEvidence transport_evidence = evidenceFor(CompletionMechanism::None);
  const JobEvidence no_evidence{ConfidenceGrade::E_TransportOnly,
                                CompletionAuthority::TransportOnly, "none"};
  // A refusal or a fault read out of DLE EOT is grade C: device status taken around
  // the transmission, reported by the printer itself.
  const JobEvidence status_evidence{ConfidenceGrade::C_DeviceStatusAround,
                                    CompletionAuthority::PhysicalPrinter, "DLE EOT"};

  if (!profile.drivableByEscposEngine()) {
    // M13b: Star Line Mode / StarPRNT fenced by ETB or ESC GS ETX is now a real path, so
    // it is dispatched rather than refused (docs/wire-protocols.md §2). Everything else —
    // the StarPRNT SDK's checked block, the ePOS JobID, ZPL, CPCL, Brother raster — is
    // still a mechanism this engine does not speak, and printing anyway would produce a
    // receipt with no fence behind it and a result nobody should believe
    // (docs/device-database.md "Vendor stacks").
    if (profile.drivableByStarEngine()) {
      runStarJob(job, payload, options, attempt, banner);
      return;
    }
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::Unsupported, reached()).with(no_evidence));
    return;
  }

  std::string error;
  if (!ensureConnected(&error)) {
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::TransportUnreachable, reached())
                  .with(no_evidence));
    return;
  }
  beginJobIo();

  // --- Preflight (docs/techspec.md §5.2 steps 3-4) --------------------------------
  if (options.preflight == PreflightMode::Strict && profile.status.dle_eot) {
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
    const TransportResult sent = sendPaced(profile, probe);
    if (!sent.ok) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached())
                    .with(no_evidence));
      return;
    }
    std::vector<escpos::ParsedEvent> answers;
    const WaitOutcome outcome = awaitRealtime(4, realtime_timeout, &answers);
    if (outcome == WaitOutcome::LinkDown) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached())
                    .with(no_evidence));
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
      terminate(job, JobState::FailedKnown,
                JobResult::failed(*refusal, reached()).with(status_evidence));
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

  // Held for the rest of the job: a token stays outstanding until the receipt it
  // fences is finished, so no later job can be handed an echo meant for this one.
  const std::unique_ptr<MarkerLease> lease = leaseTokens(job, profile);
  const std::string print_token = lease ? lease->pair.print_token : std::string();
  const std::string cut_token = lease ? lease->pair.cut_token : std::string();

  const escpos::Bytes wire =
      buildPayload(profile, payload, options, attempt, job->key(), banner, print_token);

  // --- The ordering rule (docs/techspec.md §5.1) ----------------------------------
  // SendStarted is durable before the socket sees byte one. Everything after this
  // point may be ambiguous; nothing before it can be.
  advance(job, JobState::SendStarted, reached(), FailureReason::None);

  const TransportResult sent = sendPaced(profile, wire);
  if (!sent.ok) {
    if (sent.bytes_written == 0) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached())
                    .with(no_evidence));
    } else {
      terminate(job, JobState::Unknown,
                JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown}
                    .with(transport_evidence));
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
    case CompletionMechanism::VendorIdle:
    case CompletionMechanism::EposJobId:
    case CompletionMechanism::StarCheckedBlock:
    // M13b: unreachable on this path — a Star profile is dispatched to runStarJob above.
    case CompletionMechanism::StarEtb:
    case CompletionMechanism::StarEscGsEtx:
    case CompletionMechanism::None:
      // Nothing will ever be waited for, so the cut goes out with the payload rather
      // than after an acknowledgement that is never coming. The non-ESC/POS
      // mechanisms never reach here: the job was refused as Unsupported above.
      if (cut != CutVariant::None) {
        fence = buildCut(profile, options, cut, print_token);
      }
      break;
  }
  if (!fence.empty()) {
    const TransportResult fence_sent = sendPaced(profile, fence);
    if (!fence_sent.ok) {
      terminate(job, JobState::Unknown,
                JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown}
                    .with(transport_evidence));
      return;
    }
  }
  advance(job, JobState::BytesSent, reached(), FailureReason::None);

  if (profile.completion == CompletionMechanism::None) {
    // The write-only truth (docs/sdk-spec.md §5): bytes reached a buffer somewhere,
    // and this printer can never say more than that.
    terminate(job, JobState::DoneSoftware,
              JobResult::done(clampTo(ConfidenceLevel::TransportAccepted, ceiling))
                  .with(profile.evidence()));
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
                            : FailureReason::Unknown}
                  .with(transport_evidence));
    return;
  }
  confidence = raise(confidence, ConfidenceLevel::PrintConfirmed);
  advance(job, JobState::PrintConfirmed, reached(), FailureReason::None);

  if (cut == CutVariant::None) {
    terminate(job, JobState::DoneSoftware,
              JobResult::done(reached()).with(profile.evidence()));
    return;
  }

  if (profile.completion == CompletionMechanism::GsR1) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    parser_.expectQueued();
  }
  const TransportResult cut_sent =
      sendPaced(profile, buildCut(profile, options, cut, cut_token));
  if (!cut_sent.ok) {
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown}
                  .with(transport_evidence));
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
                            : FailureReason::Unknown}
                  .with(transport_evidence));
    return;
  }
  confidence = raise(confidence, ConfidenceLevel::CutProcessed);
  advance(job, JobState::CutCommandProcessed, reached(), FailureReason::None);

  if (profile.completion != CompletionMechanism::GsParenH) {
    // A second GS r 1 fences the cut command but is not a documented cutter
    // guarantee (docs/techspec.md §3.2), so the claim stops here: print completion
    // confirmed, cut command processed, no cutter status read.
    terminate(job, JobState::DoneSoftware,
              JobResult::done(reached()).with(profile.evidence()));
    return;
  }

  // Ordered fence first, real-time cutter status second — the only order in which
  // DLE EOT 3 cannot overtake the cut (docs/techspec.md §3.3).
  clearRealtime();
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    parser_.expectRealtime(escpos::DleEotKind::ErrorCause);
  }
  const TransportResult status_sent =
      sendPaced(profile, escpos::dleEot(escpos::DleEotKind::ErrorCause));
  std::vector<escpos::ParsedEvent> answers;
  if (status_sent.ok) {
    awaitRealtime(1, realtime_timeout, &answers);
  }
  for (const escpos::ParsedEvent& answer : answers) {
    if (answer.flags.cutter_error.value_or(false)) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::CutterFault, reached())
                    .with(status_evidence));
      return;
    }
  }
  if (!answers.empty()) {
    confidence = raise(confidence, ConfidenceLevel::CutFaultFree);
  }
  // Answers empty means the bit could not be read, so the claim stays at
  // CutProcessed instead of being upgraded on silence.
  terminate(job, JobState::DoneSoftware,
            JobResult::done(reached()).with(profile.evidence()));
}

// --- M13b: the Star job path (docs/wire-protocols.md §2) -----------------------------

escpos::Bytes PrinterRuntime::buildStarPayload(const CapabilityProfile& profile,
                                               const Payload& payload,
                                               const JobOptions& options, uint32_t attempt,
                                               const std::string& key, bool banner,
                                               std::vector<std::string>* dropped) const {
  const auto note = [dropped](const std::string& what) {
    if (dropped != nullptr &&
        std::find(dropped->begin(), dropped->end(), what) == dropped->end()) {
      dropped->push_back(what);
    }
  };

  star::Encoder encoder;
  encoder.initialize();
  if (options.top_feed_dots > 0) {
    encoder.feedDots(options.top_feed_dots);
  }
  if (banner) {
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
    if (!profile.star.raster_line_mode) {
      note("raster image: this profile does not enable Star raster line mode");
    } else if (raster.width > 0 && raster.height > 0 && !raster.gray.empty()) {
      encoder.rasterGrayscale(raster.gray.data(), raster.width, raster.height,
                              config_.width_dots, raster.binarization, raster.threshold);
    }
  } else if (std::holds_alternative<DocumentPayload>(payload.content)) {
    // The document tier is escpos::Encoder output — including everything the receipt DSL
    // renders — so it is transcoded rather than refused. What has no Star equivalent is
    // named in `dropped` and reaches the caller as a declared degradation.
    star::TranscodeOptions transcode;
    transcode.raster_line_mode = profile.star.raster_line_mode;
    star::TranscodeResult converted = star::transcodeFromEscPos(
        std::get<DocumentPayload>(payload.content).bytes, transcode);
    encoder.raw(converted.bytes);
    for (const std::string& entry : converted.dropped) {
      note(entry);
    }
  } else {
    // Tier 3 is documented as passed through verbatim (docs/api.md §3): the caller owns
    // these bytes, and on a Star profile that means the caller owes Star bytes. Silently
    // transcoding them would break the one tier whose whole contract is that nothing
    // touches it.
    encoder.raw(std::get<RawPayload>(payload.content).bytes);
  }

  if (options.open_drawer) {
    note("cash drawer kick: Star's drawer command varies by model and interface and is "
         "not established here");
  }
  if (options.print_verification_id) {
    // The printed verification identifier is the GS ( H wire token promoted onto paper
    // (docs/api.md §14). A Star fence carries a counter, not a per-job token, so there is
    // nothing to print — and inventing one would put a code on a receipt that resolves to
    // nothing.
    note("printed verification identifier: the Star fences carry a counter, not a "
         "per-job token, so there is no wire token to promote onto the paper");
  }
  encoder.feedLines(profile.final_feed_lines);
  return encoder.take();
}

void PrinterRuntime::runStarJob(const std::shared_ptr<PrintJob>& job,
                                const Payload& payload, const JobOptions& options,
                                uint32_t attempt, bool banner) {
  const CapabilityProfile profile = this->profile();
  const ConfidenceLevel ceiling = profile.maxConfidence();
  const auto timeout = std::chrono::milliseconds(
      options.timeout_ms != 0 ? options.timeout_ms : profile.completion_timeout_ms);
  ConfidenceLevel confidence = ConfidenceLevel::TransportAccepted;
  const auto reached = [&] { return clampTo(confidence, ceiling); };
  const JobEvidence transport_evidence = evidenceFor(CompletionMechanism::None);
  const JobEvidence no_evidence{ConfidenceGrade::E_TransportOnly,
                                CompletionAuthority::TransportOnly, "none"};

  std::string error;
  if (!ensureConnected(&error)) {
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::TransportUnreachable, reached())
                  .with(no_evidence));
    return;
  }
  beginJobIo();
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    star_mode_ = true;
    star_parser_.setAsbBlockBytes(profile.star.asb_block_bytes);
  }

  // There is no preflight here, and that is a declared degradation rather than an
  // oversight: the realtime DLE EOT query this engine refuses to print without on
  // ESC/POS has no equivalent in the Star subset established in
  // docs/wire-protocols.md §2, so a Star job cannot refuse before printing on a
  // cover-open. The fence is where the truth comes out instead.

  if (profile.completion == CompletionMechanism::StarEtb) {
    // Enable ASB and zero the counter, so the first fence has a known baseline rather
    // than whatever the previous owner of this printer left in it.
    escpos::Bytes preamble = star::asbEnable();
    const escpos::Bytes clear = star::clearEtbCounter();
    preamble.insert(preamble.end(), clear.begin(), clear.end());
    if (!sendPaced(profile, preamble).ok) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached())
                    .with(no_evidence));
      return;
    }
    std::lock_guard<std::mutex> lock(io_mutex_);
    star_counter_ = 0;
  }

  std::vector<std::string> dropped;
  const escpos::Bytes wire =
      buildStarPayload(profile, payload, options, attempt, job->key(), banner, &dropped);

  // Same ordering rule as the ESC/POS path (docs/techspec.md §5.1): SendStarted is
  // durable before the socket sees byte one.
  advance(job, JobState::SendStarted, reached(), FailureReason::None);

  const TransportResult sent = sendPaced(profile, wire);
  if (!sent.ok) {
    if (sent.bytes_written == 0) {
      terminate(job, JobState::FailedKnown,
                JobResult::failed(FailureReason::TransportUnreachable, reached())
                    .with(no_evidence));
    } else {
      terminate(job, JobState::Unknown,
                JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown}
                    .with(transport_evidence));
    }
    return;
  }

  const CutVariant cut = effectiveCut(options.cut, profile);

  if (profile.completion == CompletionMechanism::None) {
    if (cut != CutVariant::None) {
      star::Encoder cutter;
      cutter.cut(cut == CutVariant::Full ? star::Cut::Full : star::Cut::Partial);
      sendPaced(profile, cutter.take());
    }
    advance(job, JobState::BytesSent, reached(), FailureReason::None);
    terminate(job, JobState::DoneSoftware,
              JobResult::done(clampTo(ConfidenceLevel::TransportAccepted, ceiling))
                  .with(profile.evidence()));
    return;
  }

  const escpos::Bytes print_fence = armStarFence(profile);
  if (!sendPaced(profile, print_fence).ok) {
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown}
                  .with(transport_evidence));
    return;
  }
  advance(job, JobState::BytesSent, reached(), FailureReason::None);

  const WaitOutcome print_ack = awaitStarFence(timeout);
  if (print_ack != WaitOutcome::Signalled) {
    // Bytes went out and the fence never came back. The receipt may well be on the
    // counter, so this is Unknown and never Failed — including the case where an ASB
    // counter moved for somebody else's job, which the misattribution guard in onBytes()
    // deliberately refuses to accept as ours.
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(),
                        print_ack == WaitOutcome::Timeout
                            ? FailureReason::TimeoutAwaitingCompletion
                            : FailureReason::Unknown}
                  .with(transport_evidence));
    return;
  }
  confidence = raise(confidence, ConfidenceLevel::PrintConfirmed);
  advance(job, JobState::PrintConfirmed, reached(), FailureReason::None);

  if (cut == CutVariant::None) {
    terminate(job, JobState::DoneSoftware,
              JobResult::done(reached()).with(profile.evidence()));
    return;
  }

  star::Encoder cutter;
  cutter.cut(cut == CutVariant::Full ? star::Cut::Full : star::Cut::Partial);
  escpos::Bytes cut_bytes = cutter.take();
  const escpos::Bytes cut_fence = armStarFence(profile);
  cut_bytes.insert(cut_bytes.end(), cut_fence.begin(), cut_fence.end());
  if (!sendPaced(profile, cut_bytes).ok) {
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(), FailureReason::Unknown}
                  .with(transport_evidence));
    return;
  }
  const WaitOutcome cut_ack = awaitStarFence(timeout);
  if (cut_ack != WaitOutcome::Signalled) {
    terminate(job, JobState::Unknown,
              JobResult{JobOutcome::Unknown, reached(),
                        cut_ack == WaitOutcome::Timeout
                            ? FailureReason::TimeoutAwaitingCompletion
                            : FailureReason::Unknown}
                  .with(transport_evidence));
    return;
  }
  confidence = raise(confidence, ConfidenceLevel::CutProcessed);
  advance(job, JobState::CutCommandProcessed, reached(), FailureReason::None);

  // Neither Star fence carries a cutter-fault bit, so the claim stops at "the cut command
  // was processed" — the same ceiling GS r 1 has, for the same reason.
  terminate(job, JobState::DoneSoftware,
            JobResult::done(reached()).with(profile.evidence()));
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
                                                 JobOptions options, bool reprint,
                                                 bool banner) {
  std::string key = options.key;
  uint32_t attempt = 1;

  std::lock_guard<std::mutex> index_lock(index_->mutex);
  if (!key.empty()) {
    const auto existing = index_->by_key.find(key);
    if (existing != index_->by_key.end()) {
      const std::shared_ptr<PrintJob>& previous = existing->second.job;
      // docs/api.md §4: a failed job printed nothing, so resubmitting its key is safe
      // and is exactly what an app does after showing the failure. Returning the dead
      // job instead swallows the retry — the regression the hardware soak found.
      // Done and Unknown keep the strict rule: one is already on paper and the other
      // may be, and a duplicate kitchen ticket is as damaging as a missing one.
      const bool failed_and_finished =
          previous && previous->isTerminal() && previous->state() == JobState::FailedKnown;
      if (!reprint && !failed_and_finished) {
        return previous;
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
  task.run = [this, job, shared_payload, options, attempt, banner] {
    runJob(job, *shared_payload, options, attempt, banner);
  };
  task.cancel = [this, job] {
    // Never dequeued, so provably zero bytes on the wire, and no token was ever minted
    // for it: the identifiers belong to a print, not to a submission.
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::Unknown, ConfidenceLevel::TransportAccepted));
  };
  push(std::move(task));
  return job;
}

std::shared_ptr<PrintJob> PrinterRuntime::reserve(const std::string& key, PayloadKind kind,
                                                  uint64_t payload_bytes, bool* created) {
  if (created != nullptr) {
    *created = false;
  }
  std::lock_guard<std::mutex> index_lock(index_->mutex);
  std::string effective_key = key;
  if (!effective_key.empty()) {
    const auto existing = index_->by_key.find(effective_key);
    if (existing != index_->by_key.end()) {
      // Same rule as submit(): an existing key never produces a second copy, whether
      // the first one is printing, queued, held or long finished.
      return existing->second.job;
    }
  } else {
    effective_key = "auto-" + newJobId();
  }

  const std::string job_id = newJobId();
  auto job = std::shared_ptr<PrintJob>(new PrintJob(job_id, effective_key, config_.id, 1));

  JobRecord record;
  record.id = job_id;
  record.key = effective_key;
  record.printer_id = config_.id;
  record.attempt = 1;
  record.payload_kind = kind;
  record.payload_bytes = payload_bytes;
  store_->createJob(record);
  job->emit(JobState::Queued, ConfidenceLevel::TransportAccepted, std::nullopt);

  JobEntry entry;
  entry.job = job;
  entry.printer_id = config_.id;
  entry.payload = nullptr;  // filled in by submitReserved, once the bytes are released
  entry.attempt = 1;
  index_->by_key[effective_key] = entry;

  if (created != nullptr) {
    *created = true;
  }
  return job;
}

void PrinterRuntime::submitReserved(const std::shared_ptr<PrintJob>& job,
                                    std::shared_ptr<Payload> payload, JobOptions options) {
  if (!job || !payload || job->isTerminal()) {
    return;
  }
  {
    // Recording the payload now is what makes forceReprint work on a job that was
    // held: the reprint path reuses the entry's payload.
    std::lock_guard<std::mutex> index_lock(index_->mutex);
    const auto entry = index_->by_key.find(job->key());
    if (entry != index_->by_key.end()) {
      entry->second.payload = payload;
      entry->second.options = options;
    }
  }
  if (stopping_.load()) {
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::TransportUnreachable,
                                ConfidenceLevel::TransportAccepted));
    return;
  }
  Task task;
  task.run = [this, job, payload, options] { runJob(job, *payload, options, 1, false); };
  task.cancel = [this, job] {
    terminate(job, JobState::FailedKnown,
              JobResult::failed(FailureReason::Unknown, ConfidenceLevel::TransportAccepted));
  };
  push(std::move(task));
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
      if (sendPaced(this->profile(), probe).ok) {
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
    sendPaced(this->profile(), encoder.bytes());
  };
  push(std::move(task));
}

// --- M14: the verified opening sequence (docs/cash-drawer.md) -----------------------

void PrinterRuntime::applyStoredDrawerPolarity() {
  if (!drawer_polarities_) {
    return;
  }
  const std::optional<bool> stored = drawer_polarities_->find(config_.id);
  if (!stored.has_value()) {
    return;
  }
  std::lock_guard<std::mutex> lock(profile_mutex_);
  config_.profile.drawer.status.polarity.calibrated = true;
  config_.profile.drawer.status.polarity.high_means_open = *stored;
}

std::optional<bool> PrinterRuntime::drawerPin() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return drawer_pin_;
}

std::optional<bool> PrinterRuntime::pollDrawerPin(const CapabilityProfile& profile,
                                                  std::chrono::milliseconds timeout) {
  const auto forget = [this] {
    std::lock_guard<std::mutex> lock(io_mutex_);
    // An expectation nobody answered would consume the *next* queued answer and
    // attribute a stale byte to a fresh question, so it is dropped with the phase that
    // asked for it — the same rule clearRealtime() applies to DLE EOT.
    if (parser_.outstandingQueued() > 0) {
      parser_.reset();
    }
    queued_answers_ = 0;
    queued_bytes_.clear();
  };
  {
    std::lock_guard<std::mutex> lock(io_mutex_);
    parser_.expectQueued();
  }
  if (!sendPaced(profile, escpos::gsDrawerStatus()).ok) {
    forget();
    return std::nullopt;
  }
  uint8_t answer = 0;
  if (awaitQueued(timeout, &answer) != WaitOutcome::Signalled) {
    forget();
    return std::nullopt;
  }
  const bool high = drawerPinHigh(answer);
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.observed = true;
    drawer_pin_ = high;
  }
  return high;
}

DrawerOpenResult PrinterRuntime::runDrawerSequence(const CapabilityProfile& profile,
                                                   const DrawerRequest& request) {
  const DrawerCapabilities& caps = profile.drawer;
  DrawerOpenResult result;
  result.channel = caps.channelFor(request.channel);
  result.pulse_ms = caps.pulseFor(request.pulse_ms);

  std::string error;
  if (!ensureConnected(&error)) {
    // Nothing reached the link, so nothing is known and nothing is claimed.
    result.pulse_ms = 0;
    return result;
  }
  beginJobIo();

  const auto poll_timeout = std::chrono::milliseconds(
      caps.status.poll_interval_ms != 0 ? caps.status.poll_interval_ms : 100);
  const bool sensor = caps.sensorReadable();

  // Step 1 — read the sensor first. A drawer that is already out is never pulsed
  // again: the solenoid would buzz against an open latch for nothing.
  std::optional<bool> before;
  if (sensor) {
    before = pollDrawerPin(profile, std::chrono::milliseconds(profile.preflight_timeout_ms));
  }
  if (before.has_value()) {
    result.previous_state = drawerStateFrom(*before, caps.status.polarity);
    if (result.previous_state == DrawerState::Open) {
      result.pulse_ms = 0;
      result.state = DrawerState::Open;
      return result;
    }
  } else {
    // No switch on this port, or a switch whose answer never came back. The two are
    // different facts and both end the sequence at KickSentUnverified below.
    result.previous_state = sensor ? DrawerState::Unknown : DrawerState::NoSensor;
  }

  // Step 8, of the *previous* pulse: the manufacturer cooldown is a property of the
  // solenoid, not of a call, so it is enforced here rather than left to the caller.
  if (drawer_pulsed_ && caps.kick.cooldown_ms != 0) {
    const auto cooldown = std::chrono::milliseconds(caps.kick.cooldown_ms);
    const auto since = MonotonicClock::now() - last_drawer_pulse_;
    if (since < cooldown) {
      std::this_thread::sleep_for(cooldown - since);
    }
  }

  // Step 3 — one queued pulse, at the profile's duration, on the requested channel.
  const MonotonicTime pulse_at = MonotonicClock::now();
  const TransportResult sent =
      sendPaced(profile, escpos::drawerKick(result.channel, result.pulse_ms));
  last_drawer_pulse_ = MonotonicClock::now();
  drawer_pulsed_ = true;
  if (!sent.ok) {
    // A short or refused write: the pulse may or may not have been received, which is
    // exactly the Unknown that the rest of this SDK refuses to round off.
    result.state = DrawerState::Unknown;
    return result;
  }

  if (!before.has_value()) {
    // Steps 4-6 need something that can answer, and nothing here can. This is the
    // print-server rule of docs/cash-drawer.md: the kick travels forward while the
    // sensor answer does not come back, and that path supports KickSentUnverified —
    // never OpenVerified.
    result.state = DrawerState::KickSentUnverified;
    return result;
  }

  // Steps 4-6 — watch the switch. The polls are GS r 2; an ASB frame reporting a
  // drawer-connector change arrives on its own and is picked up from the same place.
  const auto window = std::chrono::milliseconds(
      caps.status.verify_window_ms != 0 ? caps.status.verify_window_ms : 1500);
  const MonotonicTime deadline = pulse_at + window;
  const DrawerPolarity polarity = caps.status.polarity;
  bool verified = false;
  bool answered = false;
  for (;;) {
    std::optional<bool> level = pollDrawerPin(profile, poll_timeout);
    if (!level.has_value()) {
      level = drawerPin();
    }
    if (level.has_value()) {
      answered = true;
      const bool opened = polarity.calibrated
                              ? *level == polarity.high_means_open
                              // Uncalibrated: the direction has no name, but a switch
                              // that moved is still a switch that moved, and that is
                              // the claim OpenVerified makes.
                              : *level != *before;
      if (opened) {
        verified = true;
        break;
      }
    }
    if (stopping_.load() || MonotonicClock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(poll_timeout);
  }
  result.elapsed_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(MonotonicClock::now() -
                                                            pulse_at)
          .count());
  if (verified) {
    result.state = DrawerState::OpenVerified;
  } else if (!answered) {
    // It answered before the pulse and stopped answering after it. Reporting
    // FailedToOpen would claim a measurement that was never taken.
    result.state = DrawerState::KickSentUnverified;
  } else if (polarity.calibrated) {
    result.state = DrawerState::FailedToOpen;
  } else {
    // Uncalibrated and unchanged: this cannot tell "it was already open" from "it
    // never opened", so it says so instead of picking one.
    result.state = DrawerState::KickSentUnverified;
  }
  return result;
}

DrawerOpenResult PrinterRuntime::openDrawer(const DrawerRequest& request) {
  const CapabilityProfile profile = this->profile();
  const DrawerCapabilities& caps = profile.drawer;

  DrawerOpenResult refused;
  refused.channel = caps.channelFor(request.channel);
  // Refusal writes zero bytes and claims nothing. Three ways to get here: no drawer
  // port on this model, a kick method this engine cannot drive (Star's peripheral
  // command, ePOS, a vendor SDK), and — the giant-letters rule — a port whose
  // electrical standard nobody has established.
  if (!caps.kickable() || !profile.drivableByEscposEngine() || stopping_.load()) {
    return refused;
  }

  auto done = std::make_shared<std::promise<DrawerOpenResult>>();
  auto future = done->get_future();
  Task task;
  task.run = [this, profile, request, done] {
    done->set_value(runDrawerSequence(profile, request));
  };
  task.cancel = [done, refused] { done->set_value(refused); };
  pushPeripheral(std::move(task), caps.kick.can_kick_during_print);
  future.wait();
  return future.get();
}

DrawerReading PrinterRuntime::readDrawerSensor(std::chrono::milliseconds timeout) {
  const CapabilityProfile profile = this->profile();
  const DrawerCapabilities& caps = profile.drawer;

  DrawerReading empty;
  empty.available = caps.sensorReadable();
  empty.needs_calibration = !caps.status.polarity.calibrated;
  if (!empty.available || !profile.drivableByEscposEngine() || stopping_.load()) {
    return empty;
  }

  auto done = std::make_shared<std::promise<DrawerReading>>();
  auto future = done->get_future();
  Task task;
  task.run = [this, profile, timeout, done, empty] {
    DrawerReading reading = empty;
    std::string error;
    if (!ensureConnected(&error)) {
      done->set_value(reading);
      return;
    }
    beginJobIo();
    const std::optional<bool> level = pollDrawerPin(profile, timeout);
    if (level.has_value()) {
      reading.answered = true;
      reading.pin_high = *level;
      reading.state = drawerStateFrom(*level, profile.drawer.status.polarity);
    }
    done->set_value(reading);
  };
  task.cancel = [done, empty] { done->set_value(empty); };
  // A read is non-destructive — it cannot energise anything — so it goes in front of
  // queued jobs whatever canKickDuringPrint says. It still runs as its own task on the
  // one worker, so it cannot land inside a receipt.
  pushPeripheral(std::move(task), true);
  future.wait();
  return future.get();
}

bool PrinterRuntime::calibrateDrawerPolarity(bool high_means_open) {
  {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    config_.profile.drawer.status.polarity.calibrated = true;
    config_.profile.drawer.status.polarity.high_means_open = high_means_open;
  }
  if (!drawer_polarities_ || !drawer_polarities_->persistent()) {
    // The calibration still applies to this process; it simply will not outlive it.
    return false;
  }
  drawer_polarities_->save(config_.id, high_means_open);
  return true;
}

// --- end M14 -------------------------------------------------------------------------

}  // namespace detail

// --- Printer ---------------------------------------------------------------------

Printer::Printer(std::shared_ptr<detail::PrinterRuntime> runtime)
    : rt_(std::move(runtime)) {}

Printer::~Printer() = default;

const std::string& Printer::id() const noexcept { return rt_->id(); }
uint32_t Printer::widthDots() const noexcept { return rt_->widthDots(); }
CapabilityProfile Printer::profile() const { return rt_->profile(); }
std::optional<CapabilityFindings> Printer::findings() const { return rt_->findings(); }

std::shared_ptr<PrintJob> Printer::print(Payload payload, const JobOptions& options) {
  return rt_->submit(std::make_shared<Payload>(std::move(payload)), options, false, false);
}

std::shared_ptr<PrintJob> Printer::forceReprint(const std::string& key,
                                                const ReprintOptions& options) {
  JobOptions effective = options.job;
  effective.key = key;
  return rt_->submit(nullptr, effective, true, options.banner);
}

std::shared_ptr<PrintJob> Printer::forceReprint(const std::string& key, Payload payload,
                                                const ReprintOptions& options) {
  JobOptions effective = options.job;
  effective.key = key;
  return rt_->submit(std::make_shared<Payload>(std::move(payload)), effective, true,
                     options.banner);
}

DeviceStatus Printer::status() const { return rt_->status(); }

DeviceStatus Printer::refreshStatus(std::chrono::milliseconds timeout) {
  return rt_->refreshStatus(timeout);
}

void Printer::subscribe(DeviceEventCallback callback) {
  rt_->subscribeDevice(std::move(callback));
}

void Printer::openCashDrawer() { rt_->openCashDrawer(); }

// --- M14: cash drawer ---------------------------------------------------------------

DrawerOpenResult Printer::openDrawer(const DrawerRequest& request) {
  return rt_->openDrawer(request);
}

DrawerReading Printer::readDrawerSensor(std::chrono::milliseconds timeout) {
  return rt_->readDrawerSensor(timeout);
}

bool Printer::calibrateDrawerPolarity(bool high_means_open) {
  return rt_->calibrateDrawerPolarity(high_means_open);
}

DrawerCapabilities Printer::drawerCapabilities() const { return rt_->profile().drawer; }

DrawerPolarity Printer::drawerPolarity() const { return rt_->drawerPolarity(); }

// --- end M14 -------------------------------------------------------------------------

void Printer::drain() { rt_->drain(); }

std::shared_ptr<PrintJob> Printer::reserveJob(const std::string& key, PayloadKind kind,
                                              uint64_t payload_bytes, bool* created) {
  return rt_->reserve(key, kind, payload_bytes, created);
}

void Printer::submitReserved(const std::shared_ptr<PrintJob>& job, Payload payload,
                             const JobOptions& options) {
  rt_->submitReserved(job, std::make_shared<Payload>(std::move(payload)), options);
}

// --- PrinterDriver ---------------------------------------------------------------

PrinterDriver::PrinterDriver(StorageConfig storage)
    : store_(std::make_shared<JobStore>(storage)),
      capabilities_(std::make_shared<FindingsStore>(storage.directory)),
      // After the store, which is what creates the directory the nonce lives in.
      markers_(std::make_shared<detail::MarkerAllocator>(
          loadOrCreateNonce(storage.directory))) {
  // M14. Same directory, same "describes the installation, not this run" rule as the
  // instance nonce above.
  drawer_polarities_ = std::make_shared<DrawerPolarityStore>(storage.directory);
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
    // The journal is the only source of truth once a job outlives the process that
    // ran it, so a reloaded job reports whatever grade/authority/method it actually
    // persisted rather than the struct's E_TransportOnly default — including through
    // a same-key dedupe return, which hands back this very JobResult.
    outcome.with(JobEvidence{record.grade, record.authority, record.method.c_str()});
    job->setTokens(record.print_token, record.cut_token);
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
    // Paper outlives the process: a receipt printed yesterday still has to resolve to
    // the job that printed it (docs/api.md §14). Journal order is creation order, so
    // registering here keeps most-recent-first correct across the restart too.
    index_->registerToken(record.print_token, job);
    index_->registerToken(record.cut_token, job);
  }
}

const std::string& PrinterDriver::instanceNonce() const noexcept {
  return markers_->nonce();
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
  auto runtime = std::make_shared<detail::PrinterRuntime>(
      std::move(config), store_, markers_, hub_, index_, capabilities_,
      drawer_polarities_);  // M14
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

std::shared_ptr<PrintJob> PrinterDriver::jobByToken(const std::string& token) const {
  std::lock_guard<std::mutex> lock(index_->mutex);
  const auto it = index_->by_token.find(token);
  if (it == index_->by_token.end() || it->second.empty()) {
    return nullptr;
  }
  return it->second.back();
}

std::shared_ptr<PrintJob> PrinterDriver::forceReprint(const std::string& key,
                                                      const ReprintOptions& options) {
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
                                                      const ReprintOptions& options) {
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
