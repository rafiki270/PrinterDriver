#include "printerdriver/job_store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace pd {
namespace {

// Line-oriented, tab-separated, one record per line. Chosen over a binary format
// because a stuck kitchen printer gets debugged at 2 a.m. with `tail`, and over JSON
// because a JSON parser is a dependency this core is not allowed to have.
//
//   #PDJOURNAL<TAB>2
//   J<TAB>id<TAB>key<TAB>printer<TAB>created_ms<TAB>attempt<TAB>payload_kind<TAB>bytes
//   S<TAB>id<TAB>state<TAB>confidence<TAB>reason<TAB>updated_ms<TAB>grade<TAB>authority<TAB>method
//
// A J line is immutable; the current state of a job is its last S line. Text fields
// are backslash-escaped so a tab or newline inside an idempotency key cannot forge a
// record boundary. grade/authority/method are version-2 fields
// (docs/device-database.md "Confidence grades for every route"): an S line written
// before they existed has only six fields, and parseLine derives them conservatively
// rather than refusing the line — see the version-2 comment below.
constexpr char kHeader[] = "#PDJOURNAL\t2";
constexpr char kJobPrefix = 'J';
constexpr char kStatePrefix = 'S';

std::string escapeField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string unescapeField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      out += value[i];
      continue;
    }
    ++i;
    switch (value[i]) {
      case 't': out += '\t'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case '\\': out += '\\'; break;
      default: out += value[i]; break;
    }
  }
  return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  for (const char c : line) {
    if (c == '\t') {
      fields.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  fields.push_back(current);
  return fields;
}

template <typename Enum, size_t N>
bool parseEnum(const std::string& name, const std::array<Enum, N>& all, Enum& out) {
  for (const Enum value : all) {
    if (name == to_string(value)) {
      out = value;
      return true;
    }
  }
  return false;
}

// The conservative line for a version-1 S line (docs/api.md §4): a completion that
// reached PrintConfirmed or better was at least ordered by the physical printer,
// even though which mechanism ordered it was never journaled.
bool atLeastPrintConfirmed(ConfidenceLevel confidence) noexcept {
  return static_cast<int>(confidence) >= static_cast<int>(ConfidenceLevel::PrintConfirmed);
}

bool parseUint(const std::string& text, uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  out = value;
  return true;
}

constexpr std::array<PayloadKind, 3> kAllPayloadKinds{PayloadKind::Raster,
                                                      PayloadKind::Document,
                                                      PayloadKind::Raw};

std::string jobLine(const JobRecord& record) {
  std::ostringstream os;
  os << kJobPrefix << '\t' << escapeField(record.id) << '\t' << escapeField(record.key)
     << '\t' << escapeField(record.printer_id) << '\t' << record.created_unix_ms << '\t'
     << record.attempt << '\t' << to_string(record.payload_kind) << '\t'
     << record.payload_bytes;
  return os.str();
}

std::string stateLine(const std::string& id, JobState state, ConfidenceLevel confidence,
                      FailureReason reason, uint64_t at_ms, ConfidenceGrade grade,
                      CompletionAuthority authority, const std::string& method) {
  std::ostringstream os;
  os << kStatePrefix << '\t' << escapeField(id) << '\t' << to_string(state) << '\t'
     << to_string(confidence) << '\t' << to_string(reason) << '\t' << at_ms << '\t'
     << to_string(grade) << '\t' << to_string(authority) << '\t' << escapeField(method);
  return os.str();
}

std::optional<JournalEntry> parseLine(const std::string& line) {
  if (line.empty() || line[0] == '#') {
    return std::nullopt;
  }
  const std::vector<std::string> fields = splitTabs(line);
  if (fields.size() < 2 || fields[0].size() != 1) {
    return std::nullopt;
  }
  JournalEntry entry;
  if (fields[0][0] == kJobPrefix) {
    if (fields.size() < 8) {
      return std::nullopt;
    }
    entry.kind = JournalEntry::Kind::Job;
    entry.record.id = unescapeField(fields[1]);
    entry.record.key = unescapeField(fields[2]);
    entry.record.printer_id = unescapeField(fields[3]);
    uint64_t created = 0;
    uint64_t attempt = 0;
    uint64_t payload_bytes = 0;
    if (!parseUint(fields[4], created) || !parseUint(fields[5], attempt) ||
        !parseUint(fields[7], payload_bytes)) {
      return std::nullopt;
    }
    if (!parseEnum(fields[6], kAllPayloadKinds, entry.record.payload_kind)) {
      return std::nullopt;
    }
    entry.record.created_unix_ms = created;
    entry.record.attempt = static_cast<uint32_t>(attempt);
    entry.record.payload_bytes = payload_bytes;
    return entry;
  }
  if (fields[0][0] == kStatePrefix) {
    if (fields.size() < 6) {
      return std::nullopt;
    }
    entry.kind = JournalEntry::Kind::State;
    entry.record.id = unescapeField(fields[1]);
    uint64_t at_ms = 0;
    if (!parseEnum(fields[2], kAllJobStates, entry.record.state) ||
        !parseEnum(fields[3], kAllConfidenceLevels, entry.record.confidence) ||
        !parseEnum(fields[4], kAllFailureReasons, entry.record.reason) ||
        !parseUint(fields[5], at_ms)) {
      return std::nullopt;
    }
    entry.record.updated_unix_ms = at_ms;
    // Fields 6-8 (grade/authority/method) are version 2. A version-1 line, or one
    // whose tail is corrupt, gets the conservative derivation instead of being
    // refused (docs/api.md §4): strong enough to have reached PrintConfirmed becomes
    // an ordered device response; anything short of that keeps the JobRecord default
    // of E_TransportOnly/TransportOnly/"none" set above.
    const bool has_evidence =
        fields.size() >= 9 &&
        parseEnum(fields[6], kAllConfidenceGrades, entry.record.grade) &&
        parseEnum(fields[7], kAllCompletionAuthorities, entry.record.authority);
    if (has_evidence) {
      entry.record.method = unescapeField(fields[8]);
    } else if (atLeastPrintConfirmed(entry.record.confidence)) {
      entry.record.grade = ConfidenceGrade::B_OrderedDeviceResponse;
      entry.record.authority = CompletionAuthority::PhysicalPrinter;
      entry.record.method = "journal-legacy";
    }
    return entry;
  }
  // Unknown record type: a newer core wrote it. Skipping keeps an old binary usable
  // against a newer journal instead of refusing to start.
  return std::nullopt;
}

void fsyncDirectory(const std::string& path) {
  const int dir_fd = ::open(path.c_str(), O_RDONLY);
  if (dir_fd < 0) {
    return;
  }
  ::fsync(dir_fd);
  ::close(dir_fd);
}

}  // namespace

const char* to_string(PayloadKind kind) noexcept {
  switch (kind) {
    case PayloadKind::Raster: return "Raster";
    case PayloadKind::Document: return "Document";
    case PayloadKind::Raw: return "Raw";
  }
  return "Raw";
}

bool JobRecord::isTerminal() const noexcept {
  return state == JobState::DoneSoftware || state == JobState::PhysicallyVerified ||
         state == JobState::FailedKnown || state == JobState::Unknown;
}

uint64_t unixMillis() noexcept {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::vector<JournalEntry> readJournal(const std::string& path) {
  std::vector<JournalEntry> entries;
  std::ifstream input(path);
  if (!input) {
    return entries;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (auto entry = parseLine(line)) {
      entries.push_back(*entry);
    }
  }
  return entries;
}

JobStore::JobStore(StorageConfig config) : config_(std::move(config)) {
  if (config_.directory.empty()) {
    return;
  }
  if (::mkdir(config_.directory.c_str(), 0700) != 0 && errno != EEXIST) {
    throw StoreError("cannot create storage directory " + config_.directory + ": " +
                     std::strerror(errno));
  }
  journal_path_ = config_.directory;
  if (journal_path_.back() != '/') {
    journal_path_ += '/';
  }
  journal_path_ += config_.journal_name;
  load();
  compact();
  openJournal();
}

JobStore::~JobStore() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void JobStore::load() {
  const std::vector<JournalEntry> entries = readJournal(journal_path_);
  for (const JournalEntry& entry : entries) {
    if (entry.kind == JournalEntry::Kind::Job) {
      if (by_id_.find(entry.record.id) != by_id_.end()) {
        continue;
      }
      by_id_.emplace(entry.record.id, entry.record);
      order_.push_back(entry.record.id);
      if (!entry.record.key.empty()) {
        id_by_key_[entry.record.key] = entry.record.id;
      }
      continue;
    }
    const auto it = by_id_.find(entry.record.id);
    if (it == by_id_.end()) {
      continue;  // State line for a job whose creation record was lost.
    }
    it->second.state = entry.record.state;
    it->second.confidence = entry.record.confidence;
    it->second.reason = entry.record.reason;
    it->second.grade = entry.record.grade;
    it->second.authority = entry.record.authority;
    it->second.method = entry.record.method;
    it->second.updated_unix_ms = entry.record.updated_unix_ms;
  }

  // Recovery (docs/techspec.md §5.1, docs/api.md §4). Anything that reached
  // SendStarted without an acknowledgement is Unknown: bytes may have printed, and
  // deciding either way here is exactly the bug that duplicates kitchen tickets.
  // Anything still before SendStarted provably sent nothing, so it is a known
  // failure with no cause recorded rather than an ambiguous one.
  for (const std::string& id : order_) {
    JobRecord& record = by_id_[id];
    if (record.isTerminal()) {
      continue;
    }
    switch (record.state) {
      case JobState::SendStarted:
      case JobState::BytesSent:
      case JobState::PrintConfirmed:
      case JobState::CutCommandProcessed:
        record.state = JobState::Unknown;
        record.reason = FailureReason::Unknown;
        break;
      default:
        record.state = JobState::FailedKnown;
        record.reason = FailureReason::Unknown;
        break;
    }
    // A job the process never got to finish never earned whatever evidence a
    // version-1 line's confidence might otherwise imply; recovery is itself the
    // reason the claim cannot stand.
    record.grade = ConfidenceGrade::E_TransportOnly;
    record.authority = CompletionAuthority::TransportOnly;
    record.method = "none";
    record.recovered = true;
    record.updated_unix_ms = unixMillis();
    ++recovered_count_;
  }
}

void JobStore::compact() {
  // Rewrite as one J plus one S per job, via a temp file and rename, so a crash
  // during compaction leaves either the old journal or the new one — never a
  // half-written history.
  const std::string temp_path = journal_path_ + ".tmp";
  const int temp_fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (temp_fd < 0) {
    throw StoreError("cannot write journal " + temp_path + ": " + std::strerror(errno));
  }
  std::string content = std::string(kHeader) + "\n";
  for (const std::string& id : order_) {
    const JobRecord& record = by_id_.at(id);
    content += jobLine(record) + "\n";
    content += stateLine(record.id, record.state, record.confidence, record.reason,
                         record.updated_unix_ms, record.grade, record.authority,
                         record.method) +
               "\n";
  }
  const ssize_t written = ::write(temp_fd, content.data(), content.size());
  const bool ok = written == static_cast<ssize_t>(content.size());
  if (ok && config_.fsync_enabled) {
    ::fsync(temp_fd);
  }
  ::close(temp_fd);
  if (!ok) {
    ::unlink(temp_path.c_str());
    throw StoreError("short write compacting journal " + temp_path);
  }
  if (::rename(temp_path.c_str(), journal_path_.c_str()) != 0) {
    ::unlink(temp_path.c_str());
    throw StoreError("cannot replace journal " + journal_path_ + ": " +
                     std::strerror(errno));
  }
  if (config_.fsync_enabled) {
    fsyncDirectory(config_.directory);
  }
}

void JobStore::openJournal() {
  fd_ = ::open(journal_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd_ < 0) {
    throw StoreError("cannot open journal " + journal_path_ + ": " +
                     std::strerror(errno));
  }
}

void JobStore::append(const std::string& line) {
  if (fd_ < 0) {
    return;
  }
  const std::string payload = line + "\n";
  size_t offset = 0;
  while (offset < payload.size()) {
    const ssize_t written =
        ::write(fd_, payload.data() + offset, payload.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw StoreError("journal write failed: " + std::string(std::strerror(errno)));
    }
    offset += static_cast<size_t>(written);
  }
  if (config_.fsync_enabled && ::fsync(fd_) != 0) {
    throw StoreError("journal fsync failed: " + std::string(std::strerror(errno)));
  }
}

void JobStore::createJob(const JobRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (by_id_.find(record.id) != by_id_.end()) {
    throw StoreError("duplicate job id " + record.id);
  }
  JobRecord stored = record;
  if (stored.created_unix_ms == 0) {
    stored.created_unix_ms = unixMillis();
  }
  stored.updated_unix_ms = stored.created_unix_ms;
  stored.state = JobState::Queued;
  stored.confidence = ConfidenceLevel::TransportAccepted;
  stored.reason = FailureReason::None;
  stored.grade = ConfidenceGrade::E_TransportOnly;
  stored.authority = CompletionAuthority::TransportOnly;
  stored.method = "none";
  stored.recovered = false;

  std::string batch = jobLine(stored);
  batch += "\n";
  batch += stateLine(stored.id, stored.state, stored.confidence, stored.reason,
                     stored.updated_unix_ms, stored.grade, stored.authority,
                     stored.method);
  append(batch);

  by_id_.emplace(stored.id, stored);
  order_.push_back(stored.id);
  if (!stored.key.empty()) {
    id_by_key_[stored.key] = stored.id;
  }
}

void JobStore::recordState(const std::string& job_id, JobState state,
                           ConfidenceLevel confidence, FailureReason reason,
                           const JobEvidence& evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = by_id_.find(job_id);
  if (it == by_id_.end()) {
    throw StoreError("unknown job id " + job_id);
  }
  const uint64_t now = unixMillis();
  append(stateLine(job_id, state, confidence, reason, now, evidence.grade,
                   evidence.authority, evidence.method));
  it->second.state = state;
  it->second.confidence = confidence;
  it->second.reason = reason;
  it->second.grade = evidence.grade;
  it->second.authority = evidence.authority;
  it->second.method = evidence.method;
  it->second.updated_unix_ms = now;
}

std::optional<JobRecord> JobStore::findByKey(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto key_it = id_by_key_.find(key);
  if (key_it == id_by_key_.end()) {
    return std::nullopt;
  }
  const auto it = by_id_.find(key_it->second);
  if (it == by_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<JobRecord> JobStore::findById(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = by_id_.find(id);
  if (it == by_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<JobRecord> JobStore::all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<JobRecord> records;
  records.reserve(order_.size());
  for (const std::string& id : order_) {
    records.push_back(by_id_.at(id));
  }
  return records;
}

size_t JobStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return by_id_.size();
}

}  // namespace pd
