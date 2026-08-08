#include "printerdriver/print_queue.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

namespace pd {
namespace {

bool isTerminalState(JobState state) noexcept {
  return state == JobState::DoneSoftware || state == JobState::PhysicallyVerified ||
         state == JobState::FailedKnown || state == JobState::Unknown;
}

uint64_t payloadInputBytes(const Payload& payload) noexcept {
  if (std::holds_alternative<RasterPayload>(payload.content)) {
    return std::get<RasterPayload>(payload.content).gray.size();
  }
  if (std::holds_alternative<DocumentPayload>(payload.content)) {
    return std::get<DocumentPayload>(payload.content).bytes.size();
  }
  return std::get<RawPayload>(payload.content).bytes.size();
}

}  // namespace

// --- State -------------------------------------------------------------------------

namespace detail {

struct QueueItem {
  std::shared_ptr<PrintJob> job;
  std::shared_ptr<Payload> payload;
  JobOptions options;
  int32_t priority = 0;
  uint64_t seq = 0;
  bool has_deadline = false;
  MonotonicTime deadline{};
};

struct QueueLane {
  std::shared_ptr<Printer> printer;
  std::vector<QueueItem> waiting;

  // Only faults with a paired clearing event are latched here. CutterError and
  // UnrecoverableError deliberately are not: the core has no "cutter recovered" event
  // to clear them with, so latching would wedge the lane forever with no way back
  // except an operator call. Those faults surface per job, through preflight.
  bool offline = false;
  bool cover_open = false;
  bool paper_out = false;
  bool link_down = false;

  bool blocked = false;    // a job on this lane ended Unknown (§12 rule 1)
  bool paused = false;     // operator hold
  bool in_flight = false;  // one drained job at a time, so rule 1 can still stop it

  bool usable() const noexcept {
    return !offline && !cover_open && !paper_out && !link_down;
  }
};

struct QueueCompletion {
  std::string printer_id;
  JobState state = JobState::Queued;
};

struct QueueState {
  // Lane state. Held while calling into the core, which is safe because nothing the
  // core does synchronously from those calls comes back here: both core callbacks
  // touch the inbox only.
  mutable std::mutex mutex;
  std::unordered_map<std::string, QueueLane> lanes;
  uint64_t next_seq = 0;
  size_t expired = 0;
  size_t overflowed = 0;
  size_t drained = 0;

  // Inbox: the leaf lock. Device events arrive on the transport's reader thread, job
  // terminal notices on a printer's worker thread; both append here and wake the queue
  // thread, and neither ever waits on `mutex`. That is what keeps a queue-thread call
  // into the core from being able to deadlock against its own callbacks.
  std::mutex inbox_mutex;
  std::condition_variable cv;
  std::vector<QueueCompletion> completions;
  std::vector<std::pair<std::string, DeviceEvent>> device_events;
  bool pending = false;

  std::atomic<bool> stopping{false};
  std::thread worker;
};

}  // namespace detail

// --- Construction ------------------------------------------------------------------

PrintQueue::PrintQueue(PrinterDriver& driver, QueuePolicy policy)
    : policy_(policy), driver_(driver), state_(std::make_shared<detail::QueueState>()) {
  // Weak, because PrinterDriver has no unsubscribe: the hub outlives this queue and
  // must find nothing rather than a dangling this.
  std::weak_ptr<detail::QueueState> weak = state_;
  driver_.subscribeDevices([weak](const std::string& printer_id, DeviceEvent event) {
    const std::shared_ptr<detail::QueueState> state = weak.lock();
    if (!state) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state->inbox_mutex);
      state->device_events.emplace_back(printer_id, event);
      state->pending = true;
    }
    state->cv.notify_all();
  });
  state_->worker = std::thread([this] { run(); });
}

PrintQueue::~PrintQueue() { stop(); }

void PrintQueue::stop() {
  if (state_->stopping.exchange(true)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state_->inbox_mutex);
    state_->pending = true;
  }
  state_->cv.notify_all();
  if (state_->worker.joinable()) {
    state_->worker.join();
  }
}

void PrintQueue::signal() {
  {
    std::lock_guard<std::mutex> lock(state_->inbox_mutex);
    state_->pending = true;
  }
  state_->cv.notify_all();
}

void PrintQueue::run() {
  for (;;) {
    if (state_->stopping.load()) {
      return;
    }
    std::optional<MonotonicTime> next;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      next = pumpLocked();
    }
    std::unique_lock<std::mutex> lock(state_->inbox_mutex);
    if (state_->pending) {
      state_->pending = false;
      continue;
    }
    if (state_->stopping.load()) {
      return;
    }
    if (next.has_value()) {
      state_->cv.wait_until(lock, *next);
    } else {
      state_->cv.wait(lock);
    }
    state_->pending = false;
  }
}

// --- Enqueue -----------------------------------------------------------------------

detail::QueueLane& PrintQueue::laneLocked(const std::shared_ptr<Printer>& printer) {
  detail::QueueLane& lane = state_->lanes[printer->id()];
  if (lane.printer) {
    return lane;
  }
  lane.printer = printer;
  // Seeded from the last snapshot, never from an assumption: a printer nothing has
  // been heard from yet is treated as usable, because holding a job on a device the
  // SDK has never talked to would hold it forever. The core's preflight is the real
  // gate; this policy layer only reacts to states it has actually observed.
  const DeviceStatus status = printer->status();
  if (status.observed) {
    lane.offline = status.online.has_value() && !*status.online;
    lane.cover_open = status.cover_open.value_or(false);
    lane.paper_out = status.paper_out.value_or(false);
  }
  lane.link_down = false;
  return lane;
}

bool PrintQueue::drainableLocked(const detail::QueueLane& lane) const {
  if (lane.paused || lane.blocked || lane.in_flight) {
    return false;
  }
  return !policy_.hold_while_offline || lane.usable();
}

std::shared_ptr<PrintJob> PrintQueue::enqueue(const std::shared_ptr<Printer>& printer,
                                              Payload payload,
                                              const QueueOptions& options) {
  if (!printer) {
    return nullptr;
  }
  auto shared_payload = std::make_shared<Payload>(std::move(payload));

  JobOptions job_options;
  job_options.cut = options.cut;
  job_options.open_drawer = options.open_drawer;
  job_options.preflight = options.preflight;
  job_options.timeout_ms = options.timeout_ms;

  std::lock_guard<std::mutex> lock(state_->mutex);
  detail::QueueLane& lane = laneLocked(printer);

  // Claiming the key in the driver's index is what makes rule 2 hold in both
  // directions: this returns the existing job for a key already queued or printed, and
  // a later Printer::print of the same key finds the job sitting in this queue.
  bool created = false;
  std::shared_ptr<PrintJob> job = printer->reserveJob(
      options.key, shared_payload->kind(), payloadInputBytes(*shared_payload), &created);
  if (!job || !created) {
    return job;
  }
  job_options.key = job->key();

  if (drainableLocked(lane)) {
    detail::QueueItem item;
    item.job = job;
    item.payload = shared_payload;
    item.options = job_options;
    startLocked(lane, item);
    return job;
  }

  if (policy_.max_depth != 0 && lane.waiting.size() >= policy_.max_depth) {
    // Loud, immediate, and terminal. The alternative is a queue that keeps accepting
    // tickets for a printer that will never print them.
    finishLocked(job, FailureReason::QueueOverflow);
    ++state_->overflowed;
    return job;
  }

  const uint32_t ttl_ms = options.ttl_ms != 0 ? options.ttl_ms : policy_.default_ttl_ms;
  detail::QueueItem item;
  item.job = job;
  item.payload = shared_payload;
  item.options = job_options;
  item.priority = options.priority;
  item.seq = state_->next_seq++;
  item.has_deadline = ttl_ms != 0;
  if (item.has_deadline) {
    item.deadline = MonotonicClock::now() + std::chrono::milliseconds(ttl_ms);
  }
  holdLocked(job);
  lane.waiting.push_back(std::move(item));
  signal();
  return job;
}

// --- Transitions the engine cannot make --------------------------------------------

void PrintQueue::holdLocked(const std::shared_ptr<PrintJob>& job) {
  // Persist before publishing, the same ordering rule the engine obeys: an observer
  // must never learn of a transition the journal has not committed.
  driver_.store().recordState(job->id(), JobState::HeldOffline,
                              ConfidenceLevel::TransportAccepted, FailureReason::None);
  job->emit(JobState::HeldOffline, ConfidenceLevel::TransportAccepted, std::nullopt);
}

void PrintQueue::finishLocked(const std::shared_ptr<PrintJob>& job, FailureReason reason) {
  driver_.store().recordState(job->id(), JobState::FailedKnown,
                              ConfidenceLevel::TransportAccepted, reason);
  job->emit(JobState::FailedKnown, ConfidenceLevel::TransportAccepted, reason);
  // FailedKnown, not Unknown: this job never reached a worker, so provably not one
  // byte of it went to a printer.
  job->finish(JobResult::failed(reason, ConfidenceLevel::TransportAccepted));
}

void PrintQueue::startLocked(detail::QueueLane& lane, detail::QueueItem& item) {
  lane.in_flight = true;
  ++state_->drained;

  const std::string printer_id = lane.printer->id();
  std::weak_ptr<detail::QueueState> weak = state_;
  item.job->subscribe([weak, printer_id](const JobEvent& event) {
    if (!isTerminalState(event.state)) {
      return;
    }
    const std::shared_ptr<detail::QueueState> state = weak.lock();
    if (!state) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state->inbox_mutex);
      state->completions.push_back(detail::QueueCompletion{printer_id, event.state});
      state->pending = true;
    }
    state->cv.notify_all();
  });

  // §12 rule 3: the identical engine path a direct print takes.
  lane.printer->submitReserved(item.job, std::move(*item.payload), item.options);
}

// --- The pump ----------------------------------------------------------------------

void PrintQueue::applyInboxLocked() {
  std::vector<detail::QueueCompletion> completions;
  std::vector<std::pair<std::string, DeviceEvent>> events;
  {
    std::lock_guard<std::mutex> lock(state_->inbox_mutex);
    completions.swap(state_->completions);
    events.swap(state_->device_events);
    state_->pending = false;
  }

  for (const auto& event : events) {
    const auto found = state_->lanes.find(event.first);
    if (found == state_->lanes.end()) {
      continue;  // nothing queued for that printer, so nothing to hold or release
    }
    detail::QueueLane& lane = found->second;
    switch (event.second) {
      case DeviceEvent::Online: lane.offline = false; break;
      case DeviceEvent::Offline: lane.offline = true; break;
      case DeviceEvent::CoverOpen: lane.cover_open = true; break;
      case DeviceEvent::CoverClosed: lane.cover_open = false; break;
      case DeviceEvent::PaperOut: lane.paper_out = true; break;
      case DeviceEvent::PaperNearEnd: lane.paper_out = false; break;
      case DeviceEvent::PaperOk: lane.paper_out = false; break;
      case DeviceEvent::ConnectionLost: lane.link_down = true; break;
      case DeviceEvent::ConnectionRestored: lane.link_down = false; break;
      case DeviceEvent::CutterError:
      case DeviceEvent::RecoverableError:
      case DeviceEvent::UnrecoverableError:
        break;
    }
  }

  for (const detail::QueueCompletion& completion : completions) {
    const auto found = state_->lanes.find(completion.printer_id);
    if (found == state_->lanes.end()) {
      continue;
    }
    found->second.in_flight = false;
    if (completion.state == JobState::Unknown) {
      // Bytes went out and nothing came back. Someone has to look at the paper before
      // this printer takes another job (docs/sdk-spec.md §12 rule 1).
      found->second.blocked = true;
    }
  }
}

size_t PrintQueue::selectLocked(const detail::QueueLane& lane) const {
  size_t best = 0;
  for (size_t i = 1; i < lane.waiting.size(); ++i) {
    const detail::QueueItem& candidate = lane.waiting[i];
    const detail::QueueItem& incumbent = lane.waiting[best];
    if (policy_.drain_order == DrainOrder::Priority &&
        candidate.priority != incumbent.priority) {
      if (candidate.priority > incumbent.priority) {
        best = i;
      }
      continue;
    }
    if (candidate.seq < incumbent.seq) {
      best = i;
    }
  }
  return best;
}

std::optional<MonotonicTime> PrintQueue::pumpLocked() {
  applyInboxLocked();

  const MonotonicTime now = MonotonicClock::now();
  std::optional<MonotonicTime> next;

  for (auto& entry : state_->lanes) {
    detail::QueueLane& lane = entry.second;

    // Expiry runs regardless of whether the lane is blocked or paused: a stale ticket
    // is stale whatever the printer is doing, and it has still sent zero bytes.
    for (size_t i = 0; i < lane.waiting.size();) {
      if (lane.waiting[i].has_deadline && lane.waiting[i].deadline <= now) {
        finishLocked(lane.waiting[i].job, FailureReason::Expired);
        ++state_->expired;
        lane.waiting.erase(lane.waiting.begin() + static_cast<long>(i));
        continue;
      }
      ++i;
    }

    if (!lane.waiting.empty() && drainableLocked(lane)) {
      const size_t index = selectLocked(lane);
      detail::QueueItem picked = std::move(lane.waiting[index]);
      lane.waiting.erase(lane.waiting.begin() + static_cast<long>(index));
      startLocked(lane, picked);
    }

    for (const detail::QueueItem& item : lane.waiting) {
      if (item.has_deadline && (!next.has_value() || item.deadline < *next)) {
        next = item.deadline;
      }
    }
  }
  return next;
}

void PrintQueue::tick() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  pumpLocked();
}

// --- Inspection and control --------------------------------------------------------

size_t PrintQueue::depth() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  size_t total = 0;
  for (const auto& entry : state_->lanes) {
    total += entry.second.waiting.size();
  }
  return total;
}

size_t PrintQueue::depth(const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->lanes.find(printer_id);
  return found == state_->lanes.end() ? 0 : found->second.waiting.size();
}

std::vector<std::shared_ptr<PrintJob>> PrintQueue::waiting(
    const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::vector<std::shared_ptr<PrintJob>> jobs;
  const auto found = state_->lanes.find(printer_id);
  if (found == state_->lanes.end()) {
    return jobs;
  }
  for (const detail::QueueItem& item : found->second.waiting) {
    jobs.push_back(item.job);
  }
  return jobs;
}

bool PrintQueue::blocked(const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->lanes.find(printer_id);
  return found != state_->lanes.end() && found->second.blocked;
}

void PrintQueue::unblock(const std::string& printer_id) {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->lanes.find(printer_id);
    if (found == state_->lanes.end()) {
      return;
    }
    found->second.blocked = false;
  }
  signal();
}

void PrintQueue::pause(const std::string& printer_id) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->lanes[printer_id].paused = true;
}

void PrintQueue::resume(const std::string& printer_id) {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->lanes.find(printer_id);
    if (found == state_->lanes.end()) {
      return;
    }
    found->second.paused = false;
  }
  signal();
}

bool PrintQueue::paused(const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->lanes.find(printer_id);
  return found != state_->lanes.end() && found->second.paused;
}

size_t PrintQueue::expiredCount() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->expired;
}

size_t PrintQueue::overflowCount() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->overflowed;
}

size_t PrintQueue::drainedCount() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->drained;
}

}  // namespace pd
