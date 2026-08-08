#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/driver.hpp"
#include "printerdriver/types.hpp"

// Print-queue addon (docs/sdk-spec.md §12, sketched in docs/api.md §11).
//
// Layered on the public API, never part of it. The core already contains the only
// queue correctness requires — one active job per printer with completion fences
// between jobs — and that part is not optional and not policy. Everything here is
// policy: holding while a printer is unusable, draining on recovery, expiry, priority,
// depth limits. Different apps want different answers, which is exactly what makes
// this an addon.
//
// Three rules from §12 are load-bearing and are implemented rather than documented:
//
//   1. A queue is not a retry engine. A job that ends Unknown blocks its printer's
//      lane and no further job drains onto that printer until unblock() is called by
//      someone who has looked at the paper. Nothing here re-drives a job that may have
//      printed.
//   2. Idempotency keys flow through. Enqueueing a key that already has a job — held,
//      printing, or finished months ago — returns that job and prints nothing. The key
//      is claimed in the driver's own index at enqueue time, so a direct
//      Printer::print of the same key dedupes against a job that is still held.
//   3. No bypass. Draining goes down the identical engine path a direct print takes:
//      same worker, same preflight, same fences, same confidence grading. A queued job
//      is an ordinary PrintJob throughout — same handle, same id, same event stream,
//      with HeldOffline appearing in it while the bytes are parked.
//
// Threading: the queue owns one thread that expires and drains. Device events and job
// events arrive on the core's threads and only ever hand work to that thread, so
// nothing in this addon calls back into the core from a core callback.
//
// Lifetime: destroy the PrintQueue before the PrinterDriver it was built on.

namespace pd {

namespace detail {
struct QueueState;
struct QueueLane;
struct QueueItem;
}  // namespace detail

enum class DrainOrder {
  Fifo,      // submission order; the safe default for tickets that must stay in sequence
  Priority,  // higher `priority` first, submission order within equal priorities
};

struct QueuePolicy {
  // Park jobs while the printer is known to be offline, coverless or out of paper
  // instead of letting them fail one by one. False makes the queue a pure serializer.
  bool hold_while_offline = true;
  // Shelf life for a held job, in milliseconds; 0 means it never expires. A kitchen
  // ticket must not print into a recovered kitchen half an hour late.
  uint32_t default_ttl_ms = 0;
  // Held jobs per printer. Refusing at depth N beats silently accumulating tickets for
  // a dead printer — that is the printer's own buffer problem recreated one layer up.
  // 0 means unlimited, which is a choice, not a default.
  size_t max_depth = 64;
  DrainOrder drain_order = DrainOrder::Fifo;
};

struct QueueOptions {
  std::string key;       // idempotency key; empty → generated, no dedupe protection
  uint32_t ttl_ms = 0;   // 0 → the policy default
  int32_t priority = 0;  // orders the waiting set only; in-flight jobs are never preempted
  CutSetting cut = CutSetting::Profile;
  bool open_drawer = false;
  PreflightMode preflight = PreflightMode::Strict;
  uint32_t timeout_ms = 0;
};

class PrintQueue {
 public:
  explicit PrintQueue(PrinterDriver& driver, QueuePolicy policy = {});
  ~PrintQueue();

  PrintQueue(const PrintQueue&) = delete;
  PrintQueue& operator=(const PrintQueue&) = delete;

  const QueuePolicy& policy() const noexcept { return policy_; }

  // Returns an ordinary PrintJob. It is already sent when the printer is usable and
  // its lane is free; otherwise it is in state HeldOffline, or already terminal with
  // FailedKnown(QueueOverflow) when the lane is full. Never returns nullptr.
  std::shared_ptr<PrintJob> enqueue(const std::shared_ptr<Printer>& printer,
                                    Payload payload, const QueueOptions& options = {});

  size_t depth() const;                                 // held jobs, all lanes
  size_t depth(const std::string& printer_id) const;    // held jobs on one lane
  std::vector<std::shared_ptr<PrintJob>> waiting(const std::string& printer_id) const;

  // True once a job on this printer ended Unknown. The lane drains nothing more until
  // unblock() — an operator decision, not a timer (docs/sdk-spec.md §12 rule 1).
  bool blocked(const std::string& printer_id) const;
  void unblock(const std::string& printer_id);

  // Operator hold, independent of what the device is reporting.
  void pause(const std::string& printer_id);
  void resume(const std::string& printer_id);
  bool paused(const std::string& printer_id) const;

  size_t expiredCount() const;
  size_t overflowCount() const;
  size_t drainedCount() const;

  // Runs one expiry-and-drain pass on the calling thread. The queue's own thread does
  // this on every device event and whenever a TTL comes due; this is here for hosts
  // that would rather pump it themselves, and for tests.
  void tick();

  // Stops the queue thread. Held jobs stay held and stay non-terminal: the queue does
  // not invent an outcome for a job whose fate it does not know. Called by the
  // destructor.
  void stop();

 private:
  void run();
  void signal();

  detail::QueueLane& laneLocked(const std::shared_ptr<Printer>& printer);
  bool drainableLocked(const detail::QueueLane& lane) const;
  size_t selectLocked(const detail::QueueLane& lane) const;
  void startLocked(detail::QueueLane& lane, detail::QueueItem& item);

  // The two transitions the engine structurally cannot make, because it never sees
  // these jobs: parking one, and ending one that was never sent.
  void holdLocked(const std::shared_ptr<PrintJob>& job);
  void finishLocked(const std::shared_ptr<PrintJob>& job, FailureReason reason);

  void applyInboxLocked();
  std::optional<MonotonicTime> pumpLocked();

  QueuePolicy policy_;
  PrinterDriver& driver_;
  std::shared_ptr<detail::QueueState> state_;
};

}  // namespace pd
