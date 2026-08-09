#include "printerdriver/self_test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "fake_printer.hpp"
#include "printerdriver/driver.hpp"
#include "test_harness.hpp"

// M15 — PrinterDriver::autoDetect against real loopback sockets (docs/api.md §15).
//
// Three listeners, three honest answers. The suite exists to hold one property still
// above all others: **nothing prints and nothing fires**. Every case therefore asserts on
// the scripted device's print-data counter as well as on the classification, because a
// detection sweep that costs a venue a roll of paper per run is a sweep nobody will run.

using namespace pd;

namespace {

// A loopback ESC/POS printer. Real sockets on purpose: an auto-detection that never
// crossed one would prove nothing about connect timeouts, refusals or silence.
struct Listener {
  std::shared_ptr<pdfake::FakePrinter> device = std::make_shared<pdfake::FakePrinter>();
  std::unique_ptr<pdfake::FakePrinterServer> server;

  explicit Listener(const pdfake::Script& script) {
    device->setScript(script);
    server.reset(new pdfake::FakePrinterServer(device));
    if (!server->start()) {
      server.reset();
    }
  }

  bool ok() const { return server != nullptr; }
  uint16_t port() const { return server ? server->port() : 0; }
  std::string endpoint() const { return "127.0.0.1:" + std::to_string(port()); }
  void stop() {
    if (server) {
      server->stop();
    }
  }
};

pdfake::Script talkativeScript() {
  pdfake::Script script;
  script.answer_identity = true;  // "EPOSN" / "TM-T88V" — the impersonation case
  return script;
}

pdfake::Script silentScript() {
  pdfake::Script script;
  script.answer_realtime = false;
  script.answer_identity = false;
  script.answer_process_id = false;
  script.answer_queued_status = false;
  script.answer_asb = false;
  return script;
}

AutoDetectOptions fastOptions() {
  AutoDetectOptions options;
  options.connect_timeout_ms = 500;
  options.response_timeout_ms = 150;
  options.status_timeout_ms = 120;
  options.identity_timeout_ms = 120;
  options.completion_timeout_ms = 200;
  options.concurrency = 4;
  return options;
}

const DetectedPrinter* find(const std::vector<DetectedPrinter>& all,
                            const std::string& endpoint) {
  for (const DetectedPrinter& one : all) {
    if (one.endpoint == endpoint) {
      return &one;
    }
  }
  return nullptr;
}

bool anyContains(const std::vector<std::string>& lines, const std::string& needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

PD_TEST(autodetect_classifies_answering_silent_and_refusing_listeners_apart) {
  Listener answering(talkativeScript());
  Listener silent(silentScript());
  Listener gone(talkativeScript());
  CHECK(answering.ok());
  CHECK(silent.ok());
  CHECK(gone.ok());
  const std::string refused = gone.endpoint();
  gone.stop();  // the port is now closed: a refusal, deterministically

  PrinterDriver driver(StorageConfig::inMemory());
  AutoDetectOptions options = fastOptions();
  options.endpoints = {answering.endpoint(), silent.endpoint(), refused};

  std::vector<std::string> progress;
  const std::vector<DetectedPrinter> found =
      driver.autoDetect(options, [&progress](const DetectedPrinter& one, uint64_t, uint64_t) {
        progress.push_back(one.endpoint);
      });

  CHECK_EQ(found.size(), static_cast<size_t>(3));
  CHECK_EQ(progress.size(), static_cast<size_t>(3));

  // 1. It answered. Identity, a fence and a profile — none of it believed further than
  //    the evidence goes.
  const DetectedPrinter* talker = find(found, answering.endpoint());
  CHECK(talker != nullptr);
  if (talker != nullptr) {
    CHECK_EQ(talker->status, DetectionStatus::Answered);
    CHECK(talker->port_open);
    CHECK(!talker->from_cache);
    CHECK_EQ(talker->identity().model, std::string("TM-T88V"));
    CHECK(!talker->identity().trusted);
    CHECK(talker->summary.identity_fresh);
    CHECK_EQ(talker->completion(), CompletionMechanism::GsParenH);
    CHECK_EQ(talker->gradeCeiling(), ConfidenceGrade::A_JobLevelConfirmation);
    CHECK(!talker->profileId().empty());
    // The printless probe promotes the flag and NOT its provenance: an echo out of an
    // empty buffer proves the command exists, not that it fences a print.
    CHECK_EQ(talker->summary.completion_provenance, Provenance::Unverified);
    CHECK(anyContains(talker->summary.degradations, "empty buffer"));
    // The sweep's own three bytes came back.
    CHECK(!talker->dle_eot_response.empty());
  }

  // 2. The port accepted the connection and said nothing. A real finding: the interface
  //    that does not forward status bytes.
  const DetectedPrinter* quiet = find(found, silent.endpoint());
  CHECK(quiet != nullptr);
  if (quiet != nullptr) {
    CHECK_EQ(quiet->status, DetectionStatus::Silent);
    CHECK(quiet->port_open);
    CHECK(quiet->dle_eot_response.empty());
    CHECK_EQ(quiet->identity().vendor, std::string("Unknown"));
    CHECK(anyContains(quiet->summary.degradations, "answered nothing"));
  }

  // 3. Nothing is listening.
  const DetectedPrinter* dead = find(found, refused);
  CHECK(dead != nullptr);
  if (dead != nullptr) {
    CHECK_EQ(dead->status, DetectionStatus::Unreachable);
    CHECK(!dead->port_open);
    CHECK(dead->profileId().empty());
    CHECK_EQ(dead->completion(), CompletionMechanism::None);
  }

  // The whole point: not one printable byte reached either live device.
  CHECK_EQ(answering.device->printDataBytes(), static_cast<size_t>(0));
  CHECK_EQ(answering.device->cuts(), static_cast<size_t>(0));
  CHECK_EQ(answering.device->drawerKicks(), static_cast<size_t>(0));
  CHECK_EQ(silent.device->printDataBytes(), static_cast<size_t>(0));
  CHECK_EQ(silent.device->cuts(), static_cast<size_t>(0));

  answering.stop();
  silent.stop();
}

PD_TEST(autodetect_leaves_untouched_devices_alone_when_probe_unknown_is_off) {
  Listener answering(talkativeScript());
  CHECK(answering.ok());

  PrinterDriver driver(StorageConfig::inMemory());
  AutoDetectOptions options = fastOptions();
  options.endpoints = {answering.endpoint()};
  options.probe_unknown = false;

  const std::vector<DetectedPrinter> found = driver.autoDetect(options);
  CHECK_EQ(found.size(), static_cast<size_t>(1));
  CHECK_EQ(found[0].status, DetectionStatus::Unverified);
  CHECK(found[0].port_open);
  CHECK(anyContains(found[0].summary.degradations, "not interrogated"));
  // Reachable and deliberately untouched: no GS I ever went out.
  CHECK_EQ(answering.device->identityRequests().size(), static_cast<size_t>(0));
  CHECK_EQ(answering.device->printDataBytes(), static_cast<size_t>(0));

  answering.stop();
}

PD_TEST(autodetect_reuses_stored_findings_instead_of_interrogating_twice) {
  Listener answering(talkativeScript());
  CHECK(answering.ok());
  pdfake::TempDir store("autodetect");

  AutoDetectOptions options = fastOptions();
  options.endpoints = {answering.endpoint()};

  {
    PrinterDriver driver(StorageConfig::at(store.path()));
    const std::vector<DetectedPrinter> first = driver.autoDetect(options);
    CHECK_EQ(first.size(), static_cast<size_t>(1));
    CHECK_EQ(first[0].status, DetectionStatus::Answered);
    CHECK(!first[0].from_cache);
  }
  const size_t identity_requests = answering.device->identityRequests().size();
  CHECK(identity_requests > 0);

  {
    // A second driver on the same storage directory: probe results are keyed by identity
    // and persisted so a fleet does not re-interrogate every printer on every boot.
    PrinterDriver driver(StorageConfig::at(store.path()));
    const std::vector<DetectedPrinter> second = driver.autoDetect(options);
    CHECK_EQ(second.size(), static_cast<size_t>(1));
    CHECK(second[0].from_cache);
    CHECK_EQ(second[0].status, DetectionStatus::Answered);
    CHECK(!second[0].summary.identity_fresh);
    CHECK(anyContains(second[0].summary.degradations, "stored findings"));
  }
  // Nothing was asked the second time round.
  CHECK_EQ(answering.device->identityRequests().size(), identity_requests);
  CHECK_EQ(answering.device->printDataBytes(), static_cast<size_t>(0));

  answering.stop();
}

PD_TEST(autodetect_over_a_cidr_sweep_reports_only_the_open_ports) {
  Listener answering(talkativeScript());
  CHECK(answering.ok());

  PrinterDriver driver(StorageConfig::inMemory());
  AutoDetectOptions options = fastOptions();
  // /32 is one address, which is what makes this deterministic on a loopback interface.
  options.subnet_cidr = "127.0.0.1/32";
  options.port = answering.port();

  const std::vector<DetectedPrinter> found = driver.autoDetect(options);
  CHECK_EQ(found.size(), static_cast<size_t>(1));
  CHECK_EQ(found[0].status, DetectionStatus::Answered);
  CHECK_EQ(found[0].port, answering.port());
  CHECK_EQ(found[0].host, std::string("127.0.0.1"));
  CHECK_EQ(answering.device->printDataBytes(), static_cast<size_t>(0));

  answering.stop();
}

PD_TEST(autodetect_summary_reads_as_one_provenance_line) {
  Listener answering(talkativeScript());
  CHECK(answering.ok());

  PrinterDriver driver(StorageConfig::inMemory());
  AutoDetectOptions options = fastOptions();
  options.endpoints = {answering.endpoint()};

  const std::vector<DetectedPrinter> found = driver.autoDetect(options);
  CHECK_EQ(found.size(), static_cast<size_t>(1));
  const std::string line = found[0].summary.provenanceSummary();
  CHECK(line.find("GS(H) fn48") != std::string::npos);
  CHECK(line.find("Unverified") != std::string::npos);
  CHECK(line.find("untrusted") != std::string::npos);

  answering.stop();
}
