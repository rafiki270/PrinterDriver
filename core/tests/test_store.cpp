#include "printerdriver/job_store.hpp"

#include <fstream>

#include "fake_printer.hpp"
#include "test_harness.hpp"

using namespace pd;

namespace {

JobRecord makeRecord(const std::string& id, const std::string& key) {
  JobRecord record;
  record.id = id;
  record.key = key;
  record.printer_id = "printer-1";
  record.attempt = 1;
  record.payload_kind = PayloadKind::Document;
  record.payload_bytes = 42;
  return record;
}

size_t countLines(const std::string& path) {
  std::ifstream input(path);
  size_t count = 0;
  std::string line;
  while (std::getline(input, line)) {
    ++count;
  }
  return count;
}

}  // namespace

PD_TEST(store_in_memory_keeps_jobs_without_a_journal) {
  JobStore store(StorageConfig::inMemory());
  CHECK(!store.persistent());
  CHECK(store.journalPath().empty());
  store.createJob(makeRecord("job-1", "order-A"));
  store.recordState("job-1", JobState::SendStarted, ConfidenceLevel::PrinterHealthy,
                    FailureReason::None);
  const auto found = store.findByKey("order-A");
  CHECK(found.has_value());
  CHECK_EQ(found->state, JobState::SendStarted);
  CHECK_EQ(found->confidence, ConfidenceLevel::PrinterHealthy);
}

PD_TEST(store_indexes_jobs_by_idempotency_key) {
  pdfake::TempDir dir("store-index");
  JobStore store(StorageConfig::at(dir.path()));
  store.createJob(makeRecord("job-1", "order-A"));
  store.createJob(makeRecord("job-2", "order-B"));
  CHECK_EQ(store.size(), static_cast<size_t>(2));
  CHECK(store.findByKey("order-A").has_value());
  CHECK_EQ(store.findByKey("order-A")->id, std::string("job-1"));
  CHECK_EQ(store.findById("job-2")->key, std::string("order-B"));
  CHECK(!store.findByKey("order-missing").has_value());
  CHECK(!store.findById("nope").has_value());
}

PD_TEST(store_rejects_duplicate_job_ids) {
  JobStore store(StorageConfig::inMemory());
  store.createJob(makeRecord("job-1", "order-A"));
  CHECK_THROWS(store.createJob(makeRecord("job-1", "order-B")), StoreError);
  CHECK_THROWS(store.recordState("missing", JobState::BytesSent,
                                 ConfidenceLevel::TransportAccepted,
                                 FailureReason::None),
               StoreError);
}

PD_TEST(store_journal_holds_creation_and_every_transition) {
  pdfake::TempDir dir("store-journal");
  const std::string path = dir.path() + "/jobs.journal";
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-1", "order-A"));
    store.recordState("job-1", JobState::PreflightOk, ConfidenceLevel::PrinterHealthy,
                      FailureReason::None);
    store.recordState("job-1", JobState::SendStarted, ConfidenceLevel::PrinterHealthy,
                      FailureReason::None);
  }
  const std::vector<JournalEntry> entries = readJournal(path);
  CHECK_EQ(entries.size(), static_cast<size_t>(4));
  CHECK_EQ(entries[0].kind, JournalEntry::Kind::Job);
  CHECK_EQ(entries[0].record.payload_kind, PayloadKind::Document);
  CHECK_EQ(entries[0].record.payload_bytes, static_cast<uint64_t>(42));
  CHECK_EQ(entries[1].record.state, JobState::Queued);
  CHECK_EQ(entries[2].record.state, JobState::PreflightOk);
  CHECK_EQ(entries[3].record.state, JobState::SendStarted);
}

PD_TEST(store_recovers_in_flight_jobs_as_unknown) {
  pdfake::TempDir dir("store-recover");
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-send", "order-send"));
    store.recordState("job-send", JobState::SendStarted, ConfidenceLevel::PrinterHealthy,
                      FailureReason::None);
    store.createJob(makeRecord("job-bytes", "order-bytes"));
    store.recordState("job-bytes", JobState::BytesSent, ConfidenceLevel::PrinterHealthy,
                      FailureReason::None);
    store.createJob(makeRecord("job-print", "order-print"));
    store.recordState("job-print", JobState::PrintConfirmed,
                      ConfidenceLevel::PrintConfirmed, FailureReason::None);
    store.createJob(makeRecord("job-cut", "order-cut"));
    store.recordState("job-cut", JobState::CutCommandProcessed,
                      ConfidenceLevel::CutProcessed, FailureReason::None);
  }
  JobStore reloaded(StorageConfig::at(dir.path()));
  CHECK_EQ(reloaded.recoveredCount(), static_cast<size_t>(4));
  for (const char* key : {"order-send", "order-bytes", "order-print", "order-cut"}) {
    const auto record = reloaded.findByKey(key);
    CHECK(record.has_value());
    CHECK_EQ(record->state, JobState::Unknown);
    CHECK(record->recovered);
  }
  // Confidence survives recovery: an operator needs to know how far the job got.
  CHECK_EQ(reloaded.findByKey("order-cut")->confidence, ConfidenceLevel::CutProcessed);
}

PD_TEST(store_recovers_pre_send_jobs_as_failed_known) {
  pdfake::TempDir dir("store-presend");
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-queued", "order-queued"));
    store.createJob(makeRecord("job-preflight", "order-preflight"));
    store.recordState("job-preflight", JobState::PreflightOk,
                      ConfidenceLevel::PrinterHealthy, FailureReason::None);
  }
  JobStore reloaded(StorageConfig::at(dir.path()));
  // Provably zero bytes on the wire, so this is a known failure rather than the
  // ambiguous one (docs/techspec.md §6 retry policy).
  CHECK_EQ(reloaded.findByKey("order-queued")->state, JobState::FailedKnown);
  CHECK_EQ(reloaded.findByKey("order-preflight")->state, JobState::FailedKnown);
}

PD_TEST(store_leaves_terminal_jobs_untouched) {
  pdfake::TempDir dir("store-terminal");
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-done", "order-done"));
    store.recordState("job-done", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None);
    store.createJob(makeRecord("job-failed", "order-failed"));
    store.recordState("job-failed", JobState::FailedKnown,
                      ConfidenceLevel::TransportAccepted,
                      FailureReason::PreflightPaperOut);
  }
  JobStore reloaded(StorageConfig::at(dir.path()));
  CHECK_EQ(reloaded.recoveredCount(), static_cast<size_t>(0));
  CHECK_EQ(reloaded.findByKey("order-done")->state, JobState::DoneSoftware);
  CHECK_EQ(reloaded.findByKey("order-done")->confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(reloaded.findByKey("order-failed")->reason, FailureReason::PreflightPaperOut);
  CHECK(!reloaded.findByKey("order-done")->recovered);
}

PD_TEST(store_roundtrips_confidence_grade_authority_and_method) {
  pdfake::TempDir dir("store-evidence-roundtrip");
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-1", "order-A"));
    store.recordState("job-1", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None,
                      JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                                 CompletionAuthority::PhysicalPrinter, "GS(H) fn48"});
    // A non-terminal transition with no evidence argument persists the conservative
    // default rather than carrying the terminal grade backwards.
    store.createJob(makeRecord("job-2", "order-B"));
    store.recordState("job-2", JobState::SendStarted, ConfidenceLevel::PrinterHealthy,
                      FailureReason::None);
  }
  JobStore reloaded(StorageConfig::at(dir.path()));
  const auto record = reloaded.findByKey("order-A");
  CHECK(record.has_value());
  CHECK_EQ(record->confidence, ConfidenceLevel::CutFaultFree);
  CHECK_EQ(record->grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(record->authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(record->method, std::string("GS(H) fn48"));

  // job-2 reloads as Unknown (SendStarted with no ack is in-flight), and recovery
  // never had real evidence to report either way.
  const auto defaulted = reloaded.findByKey("order-B");
  CHECK(defaulted.has_value());
  CHECK_EQ(defaulted->grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(defaulted->authority, CompletionAuthority::TransportOnly);
  CHECK_EQ(defaulted->method, std::string("none"));
}

PD_TEST(store_derives_conservative_evidence_for_legacy_journal_lines) {
  pdfake::TempDir dir("store-evidence-legacy");
  const std::string path = dir.path() + "/jobs.journal";
  { JobStore bootstrap(StorageConfig::at(dir.path())); }  // creates the directory
  {
    // Overwrite with pure version-1 content: no grade/authority/method columns.
    std::ofstream out(path, std::ios::trunc);
    out << "#PDJOURNAL\t1\n";
    out << "J\tjob-strong\torder-strong\tprinter-1\t1000\t1\tRaw\t10\n";
    out << "S\tjob-strong\tDoneSoftware\tCutProcessed\tNone\t1000\n";
    out << "J\tjob-weak\torder-weak\tprinter-1\t1000\t1\tRaw\t10\n";
    out << "S\tjob-weak\tFailedKnown\tTransportAccepted\tTransportUnreachable\t1000\n";
    out << "J\tjob-failed-late\torder-failed-late\tprinter-1\t1000\t1\tRaw\t10\n";
    out << "S\tjob-failed-late\tFailedKnown\tPrintConfirmed\tCutterFault\t1000\n";
  }
  JobStore store(StorageConfig::at(dir.path()));

  // CutProcessed >= PrintConfirmed: treated as an ordered device response rather
  // than downgraded to transport-only just because the line predates evidence.
  const auto strong = store.findByKey("order-strong");
  CHECK(strong.has_value());
  CHECK_EQ(strong->grade, ConfidenceGrade::B_OrderedDeviceResponse);
  CHECK_EQ(strong->authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(strong->method, std::string("journal-legacy"));

  // TransportAccepted is below PrintConfirmed: stays at the conservative default.
  const auto weak = store.findByKey("order-weak");
  CHECK(weak.has_value());
  CHECK_EQ(weak->grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(weak->authority, CompletionAuthority::TransportOnly);
  CHECK_EQ(weak->method, std::string("none"));

  // A failure whose confidence climbed to PrintConfirmed before it failed must NOT be
  // upgraded: grade B with PhysicalPrinter authority on a FailedKnown line would be an
  // operator-facing overclaim. Only completed states earn the legacy derivation.
  const auto failed_late = store.findByKey("order-failed-late");
  CHECK(failed_late.has_value());
  CHECK_EQ(failed_late->state, JobState::FailedKnown);
  CHECK_EQ(failed_late->grade, ConfidenceGrade::E_TransportOnly);
  CHECK_EQ(failed_late->authority, CompletionAuthority::TransportOnly);
  CHECK_EQ(failed_late->method, std::string("none"));
}

PD_TEST(store_roundtrips_verification_tokens_through_compaction) {
  pdfake::TempDir dir("store-tokens");
  const std::string path = dir.path() + "/jobs.journal";
  {
    JobStore store(StorageConfig::at(dir.path()));
    JobRecord created = makeRecord("job-1", "order-A");
    created.print_token = "aZ!\"";  // the alphabet includes quotes and backslashes
    created.cut_token = "aZ!#";
    store.createJob(created);
    // The other path: a record minted before its tokens existed, told about them later.
    store.createJob(makeRecord("job-2", "order-B"));
    store.recordTokens("job-2", "aZ!$", "aZ!%");
    store.recordState("job-2", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None);
  }

  // The T record is a first-class journal entry, not a comment.
  bool saw_token_record = false;
  for (const JournalEntry& entry : readJournal(path)) {
    if (entry.kind == JournalEntry::Kind::Tokens && entry.record.id == "job-2") {
      saw_token_record = true;
      CHECK_EQ(entry.record.print_token, std::string("aZ!$"));
      CHECK_EQ(entry.record.cut_token, std::string("aZ!%"));
    }
  }
  CHECK(saw_token_record);

  // Reopening compacts, which folds the T record into the J line; reopening again
  // proves the folded line reads back the same.
  for (int pass = 0; pass < 2; ++pass) {
    JobStore reloaded(StorageConfig::at(dir.path()));
    CHECK_EQ(reloaded.findByKey("order-A")->print_token, std::string("aZ!\""));
    CHECK_EQ(reloaded.findByKey("order-A")->cut_token, std::string("aZ!#"));
    CHECK_EQ(reloaded.findByKey("order-B")->print_token, std::string("aZ!$"));
    CHECK_EQ(reloaded.findByKey("order-B")->cut_token, std::string("aZ!%"));
  }
}

PD_TEST(store_loads_a_version_two_journal_without_tokens) {
  // Format version 3 added the two trailing J fields (docs/api.md §14). An older
  // journal is still a journal: its jobs load, they simply have no identifier to
  // resolve, which is indistinguishable from a job printed without a GS ( H fence.
  pdfake::TempDir dir("store-v2");
  const std::string path = dir.path() + "/jobs.journal";
  { JobStore bootstrap(StorageConfig::at(dir.path())); }
  {
    std::ofstream out(path, std::ios::trunc);
    out << "#PDJOURNAL\t2\n";
    out << "J\tjob-old\torder-old\tprinter-1\t1000\t1\tRaw\t10\n";
    out << "S\tjob-old\tDoneSoftware\tCutFaultFree\tNone\t1000\tA_JobLevelConfirmation"
           "\tPhysicalPrinter\tGS(H) fn48\n";
  }
  JobStore store(StorageConfig::at(dir.path()));
  const auto record = store.findByKey("order-old");
  CHECK(record.has_value());
  CHECK_EQ(record->state, JobState::DoneSoftware);
  CHECK_EQ(record->grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK(record->print_token.empty());
  CHECK(record->cut_token.empty());
}

PD_TEST(store_compacts_history_on_load) {
  pdfake::TempDir dir("store-compact");
  const std::string path = dir.path() + "/jobs.journal";
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-1", "order-A"));
    for (int i = 0; i < 10; ++i) {
      store.recordState("job-1", JobState::BytesSent, ConfidenceLevel::PrinterHealthy,
                        FailureReason::None);
    }
    store.recordState("job-1", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None);
  }
  CHECK_EQ(countLines(path), static_cast<size_t>(14));  // header + J + 12 transitions
  {
    JobStore reloaded(StorageConfig::at(dir.path()));
    CHECK_EQ(reloaded.size(), static_cast<size_t>(1));
  }
  CHECK_EQ(countLines(path), static_cast<size_t>(3));  // header + J + final state
  JobStore again(StorageConfig::at(dir.path()));
  CHECK_EQ(again.findByKey("order-A")->state, JobState::DoneSoftware);
}

PD_TEST(store_survives_keys_containing_separators) {
  pdfake::TempDir dir("store-escape");
  const std::string nasty = "order\t7F3A\nkitchen\\1";
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-1", nasty));
    store.recordState("job-1", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None);
  }
  JobStore reloaded(StorageConfig::at(dir.path()));
  CHECK_EQ(reloaded.size(), static_cast<size_t>(1));
  const auto record = reloaded.findByKey(nasty);
  CHECK(record.has_value());
  CHECK_EQ(record->state, JobState::DoneSoftware);
}

PD_TEST(store_skips_unparseable_and_unknown_journal_lines) {
  pdfake::TempDir dir("store-forward");
  const std::string path = dir.path() + "/jobs.journal";
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-1", "order-A"));
    store.recordState("job-1", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None);
  }
  {
    std::ofstream out(path, std::ios::app);
    out << "X\tsomething-a-newer-core-writes\n";
    out << "S\tjob-1\tNotAState\tCutFaultFree\tNone\t1\n";
    out << "garbage without tabs\n";
    out << "S\tjob-unknown\tDoneSoftware\tCutFaultFree\tNone\t1\n";
  }
  // J, S(Queued), S(DoneSoftware) and the orphan S; the three malformed lines are
  // skipped rather than refusing to open the journal.
  const std::vector<JournalEntry> entries = readJournal(path);
  CHECK_EQ(entries.size(), static_cast<size_t>(4));
  JobStore reloaded(StorageConfig::at(dir.path()));
  CHECK_EQ(reloaded.size(), static_cast<size_t>(1));
  CHECK_EQ(reloaded.findByKey("order-A")->state, JobState::DoneSoftware);
}

PD_TEST(store_reopens_and_appends_after_reload) {
  pdfake::TempDir dir("store-reopen");
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-1", "order-A"));
    store.recordState("job-1", JobState::DoneSoftware, ConfidenceLevel::CutFaultFree,
                      FailureReason::None);
  }
  {
    JobStore store(StorageConfig::at(dir.path()));
    store.createJob(makeRecord("job-2", "order-B"));
    store.recordState("job-2", JobState::Unknown, ConfidenceLevel::PrintConfirmed,
                      FailureReason::TimeoutAwaitingCompletion);
  }
  JobStore reloaded(StorageConfig::at(dir.path()));
  CHECK_EQ(reloaded.size(), static_cast<size_t>(2));
  CHECK_EQ(reloaded.findByKey("order-A")->state, JobState::DoneSoftware);
  CHECK_EQ(reloaded.findByKey("order-B")->state, JobState::Unknown);
  CHECK_EQ(reloaded.findByKey("order-B")->reason,
           FailureReason::TimeoutAwaitingCompletion);
  const std::vector<JobRecord> all = reloaded.all();
  CHECK_EQ(all.size(), static_cast<size_t>(2));
  CHECK_EQ(all[0].id, std::string("job-1"));
  CHECK_EQ(all[1].id, std::string("job-2"));
}

PD_TEST(store_reports_unix_millis_moving_forward) {
  const uint64_t first = unixMillis();
  CHECK(first > 1600000000000ull);
  CHECK(unixMillis() >= first);
}
