#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "printerdriver/types.hpp"

// File-backed job store (docs/sdk-spec.md §4, docs/techspec.md §5.1).
//
// The entire point of this class is one ordering rule: a state transition is durable
// on disk *before* the action it authorizes happens. SendStarted is fsync'd before the
// first payload byte reaches the socket, so a process that dies mid-send can never
// come back believing the job never started. Everything else here — the append-only
// journal, compaction, the key index — exists to make that rule cheap enough to obey
// on every transition.

namespace pd {

class StoreError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class PayloadKind { Raster, Document, Raw };

const char* to_string(PayloadKind) noexcept;

struct StorageConfig {
  // Empty directory means in-memory only: no journal, no durability, no recovery.
  // Useful for tests and for callers that genuinely do not want a disk footprint;
  // never appropriate for kitchen tickets.
  std::string directory;
  std::string journal_name = "jobs.journal";
  // Off makes writes ~100x faster and the durability guarantee worthless. Only for
  // tests that assert on file content rather than on crash behaviour.
  bool fsync_enabled = true;

  static StorageConfig inMemory() { return StorageConfig{}; }
  static StorageConfig at(std::string dir) {
    StorageConfig config;
    config.directory = std::move(dir);
    return config;
  }
};

struct JobRecord {
  std::string id;
  std::string key;
  std::string printer_id;
  uint64_t created_unix_ms = 0;
  uint32_t attempt = 1;
  PayloadKind payload_kind = PayloadKind::Raw;
  uint64_t payload_bytes = 0;

  JobState state = JobState::Queued;
  ConfidenceLevel confidence = ConfidenceLevel::TransportAccepted;
  FailureReason reason = FailureReason::None;
  // What the current state rests on (docs/device-database.md "Confidence grades for
  // every route"). A line written before these existed has none of the three; load()
  // derives them conservatively from `confidence` instead of leaving them blank.
  ConfidenceGrade grade = ConfidenceGrade::E_TransportOnly;
  CompletionAuthority authority = CompletionAuthority::TransportOnly;
  std::string method = "none";
  uint64_t updated_unix_ms = 0;

  // Set by load(), not by the journal: this job was in flight when the process
  // stopped and was reclassified (docs/api.md §4 crash recovery).
  bool recovered = false;

  bool isTerminal() const noexcept;
};

// One parsed journal line. Exposed because reading the raw journal is the only way to
// prove the ordering rule from outside the store, which the engine tests do.
struct JournalEntry {
  enum class Kind { Job, State } kind = Kind::Job;
  // Kind::Job: creation fields. Kind::State: id/state/confidence/reason/grade/
  // authority/method.
  JobRecord record;
};

std::vector<JournalEntry> readJournal(const std::string& path);

class JobStore {
 public:
  explicit JobStore(StorageConfig config);
  ~JobStore();

  JobStore(const JobStore&) = delete;
  JobStore& operator=(const JobStore&) = delete;

  // Appends the creation record plus its initial Queued transition and fsyncs.
  void createJob(const JobRecord& record);

  // Appends a transition and fsyncs before returning. The caller must not perform
  // the action this transition authorizes until it has returned. `evidence` defaults
  // to none, for the many transitions that have not earned any yet; the caller that
  // has something to report passes it explicitly.
  void recordState(const std::string& job_id, JobState state, ConfidenceLevel confidence,
                   FailureReason reason, const JobEvidence& evidence = {});

  std::optional<JobRecord> findByKey(const std::string& key) const;
  std::optional<JobRecord> findById(const std::string& id) const;
  std::vector<JobRecord> all() const;
  size_t size() const;

  // Empty when the store is in-memory.
  const std::string& journalPath() const noexcept { return journal_path_; }
  bool persistent() const noexcept { return !journal_path_.empty(); }

  // Number of jobs the last load() reclassified as Unknown or FailedKnown.
  size_t recoveredCount() const noexcept { return recovered_count_; }

 private:
  void load();
  void compact();
  void append(const std::string& line);
  void openJournal();

  mutable std::mutex mutex_;
  StorageConfig config_;
  std::string journal_path_;
  int fd_ = -1;
  size_t recovered_count_ = 0;
  std::vector<std::string> order_;
  std::unordered_map<std::string, JobRecord> by_id_;
  std::unordered_map<std::string, std::string> id_by_key_;
};

uint64_t unixMillis() noexcept;

}  // namespace pd
