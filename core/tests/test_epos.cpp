#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "printerdriver/device_profiles.hpp"
#include "printerdriver/epos.hpp"
#include "printerdriver/net_platform.hpp"
#include "test_harness.hpp"

// M13b. The Epson ePOS-Print service (docs/wire-protocols.md §1).
//
// The centre of gravity of this file is one rule: **on a printer with the spooler
// enabled, the first success="true" is an enqueue acknowledgement and not a print.** It
// arrives with a status of 0x00000002 ("printing completed") before any paper has moved,
// so an implementation that reads the status field and stops has produced a confident,
// top-of-the-hierarchy "printed" for a receipt that is still queued behind a cover-open
// fault. Several tests here exist purely to make that impossible to reintroduce.
//
// The client is driven over a real loopback socket rather than through an injected
// transport, because a protocol client that never crossed a socket proves nothing about
// the socket — the same reason the agent suite runs its real HTTP server.

using namespace pd;

namespace {

// A scriptable in-process ePOS service. Answers each POST with the next scripted body and
// closes, which is what the client's `Connection: close` asks for.
class FakeEposServer {
 public:
  ~FakeEposServer() { stop(); }

  void setScript(std::vector<std::string> bodies) {
    std::lock_guard<std::mutex> lock(mutex_);
    bodies_ = std::move(bodies);
    served_ = 0;
  }

  void setHttpStatus(int status) { http_status_.store(status); }

  bool start() {
    if (!net::startup()) {
      return false;
    }
    listen_socket_ = net::create(AF_INET, SOCK_STREAM, 0);
    if (!net::valid(listen_socket_)) {
      return false;
    }
    net::setIntOption(listen_socket_, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = net::loopbackAddress();
    address.sin_port = 0;
    if (::bind(listen_socket_, reinterpret_cast<sockaddr*>(&address),
               static_cast<net::SockLen>(sizeof(address))) != 0 ||
        ::listen(listen_socket_, 8) != 0) {
      net::closeSocket(listen_socket_);
      listen_socket_ = net::invalidSocket();
      return false;
    }
    net::SockLen length = static_cast<net::SockLen>(sizeof(address));
    if (::getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      net::closeSocket(listen_socket_);
      listen_socket_ = net::invalidSocket();
      return false;
    }
    port_ = net::fromNetwork16(address.sin_port);
    thread_ = std::thread([this] { serve(); });
    return true;
  }

  uint16_t port() const { return port_; }

  void stop() {
    running_.store(false);
    if (net::valid(listen_socket_)) {
      net::shutdownBoth(listen_socket_);
      net::closeSocket(listen_socket_);
      listen_socket_ = net::invalidSocket();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::vector<std::string> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }
  size_t requestCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.size();
  }

 private:
  std::string nextBody() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bodies_.empty()) {
      return std::string();
    }
    // The last scripted answer repeats, so a poll loop that asks once more than the test
    // predicted keeps getting a terminal answer instead of an empty one.
    const size_t index = served_ < bodies_.size() ? served_ : bodies_.size() - 1;
    ++served_;
    return bodies_[index];
  }

  void serve() {
    while (running_.load()) {
      net::PollFd waiter;
      waiter.socket = listen_socket_;
      waiter.events = net::kPollIn;
      if (!net::valid(listen_socket_) || net::poll(&waiter, 1, 50) <= 0) {
        continue;
      }
      const net::Socket client =
          static_cast<net::Socket>(::accept(listen_socket_, nullptr, nullptr));
      if (!net::valid(client)) {
        continue;
      }
      std::string raw;
      char buffer[2048];
      size_t expected = 0;
      bool have_headers = false;
      while (running_.load()) {
        net::PollFd reader;
        reader.socket = client;
        reader.events = net::kPollIn;
        if (net::poll(&reader, 1, 500) <= 0) {
          break;
        }
        const int64_t got = net::recvSome(client, buffer, sizeof(buffer));
        if (got <= 0) {
          break;
        }
        raw.append(buffer, static_cast<size_t>(got));
        const size_t header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
          continue;
        }
        if (!have_headers) {
          have_headers = true;
          const size_t marker = raw.find("Content-Length:");
          expected = marker == std::string::npos
                         ? 0
                         : static_cast<size_t>(std::strtoul(
                               raw.c_str() + marker + 15, nullptr, 10));
        }
        if (raw.size() >= header_end + 4 + expected) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(raw);
          }
          break;
        }
      }
      const std::string body = nextBody();
      std::string response;
      response += "HTTP/1.1 " + std::to_string(http_status_.load()) + " OK\r\n";
      response += "Content-Type: text/xml; charset=utf-8\r\n";
      response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
      response += "Connection: close\r\n\r\n";
      response += body;
      size_t sent = 0;
      while (sent < response.size()) {
        const int64_t wrote =
            net::sendSome(client, response.data() + sent, response.size() - sent);
        if (wrote <= 0) {
          break;
        }
        sent += static_cast<size_t>(wrote);
      }
      net::shutdownBoth(client);
      net::closeSocket(client);
    }
  }

  mutable std::mutex mutex_;
  std::vector<std::string> bodies_;
  std::vector<std::string> requests_;
  size_t served_ = 0;
  net::Socket listen_socket_ = net::invalidSocket();
  uint16_t port_ = 0;
  std::atomic<int> http_status_{200};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

std::string envelope(const std::string& response_attributes) {
  // Deliberately spelled with a `soap:` prefix rather than Epson's `s:`. Both are correct
  // XML and a parser that greps for one of them is broken on the other.
  return std::string(
             "<?xml version=\"1.0\"?>"
             "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
             "<soap:Body><response xmlns=\"http://www.epson-pos.com/schemas/2011/03/"
             "epos-print\" ") +
         response_attributes + "/></soap:Body></soap:Envelope>";
}

epos::ClientConfig configFor(const FakeEposServer& server, bool spooler) {
  epos::ClientConfig config;
  config.host = "127.0.0.1";
  config.port = server.port();
  config.spooler = spooler;
  config.connect_timeout_ms = 2000;
  config.http_timeout_ms = 3000;
  config.poll_interval_ms = 5;
  config.poll_budget_ms = 2000;
  return config;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// --- Request shape ------------------------------------------------------------------

PD_TEST(epos_print_envelope_carries_devid_timeout_and_printjobid) {
  epos::Document document;
  document.line("Hello, World!").cut();
  const std::string body =
      epos::buildPrintEnvelope("local_printer", 60000, "ABC123", document.body());

  CHECK(contains(body, "<devid>local_printer</devid>"));
  CHECK(contains(body, "<timeout>60000</timeout>"));
  CHECK(contains(body, "<printjobid>ABC123</printjobid>"));
  CHECK(contains(body, "http://www.epson-pos.com/schemas/2011/03/epos-print"));
  CHECK(contains(body, "<text>Hello, World!&#10;</text>"));
  CHECK(contains(body, "<cut"));
}

PD_TEST(epos_service_timeout_is_capped_at_the_documented_maximum) {
  const std::string body = epos::buildPrintEnvelope("local_printer", 900000, "J1", "");
  // 300 000 ms is the service's cap (ePOS-Print XML User's Manual rev. AC). Asking for
  // more is a request the service rejects, so it is clamped rather than sent.
  CHECK(contains(body, "<timeout>300000</timeout>"));
}

PD_TEST(epos_poll_envelope_is_header_only_with_an_empty_body) {
  const std::string body = epos::buildPollEnvelope("ABC123");
  CHECK(contains(body, "<printjobid>ABC123</printjobid>"));
  CHECK(!contains(body, "<devid>"));
  // The emptiness IS the retrieval verb. A document here would print the receipt twice.
  CHECK(contains(body, "<epos-print"));
  CHECK(!contains(body, "<text>"));
  CHECK(!contains(body, "<cut"));
}

PD_TEST(epos_job_ids_are_validated_before_the_receipt_is_sent) {
  CHECK(epos::isValidJobId("ABC123"));
  CHECK(epos::isValidJobId("order_2026-08-09.1"));
  CHECK(epos::isValidJobId("a"));
  CHECK(epos::isValidJobId(std::string(30, 'x')));
  CHECK(!epos::isValidJobId(""));
  CHECK(!epos::isValidJobId(std::string(31, 'x')));
  CHECK(!epos::isValidJobId("has space"));
  CHECK(!epos::isValidJobId("has/slash"));
}

// --- Parsing --------------------------------------------------------------------------

PD_TEST(epos_response_is_matched_by_namespace_and_local_name_not_by_prefix) {
  epos::Response parsed;
  // Epson's own spelling.
  CHECK(epos::parseResponse(
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
      "<response xmlns=\"http://www.epson-pos.com/schemas/2011/03/epos-print\" "
      "success=\"true\" code=\"\" status=\"2\" battery=\"0\"/></s:Body></s:Envelope>",
      &parsed));
  CHECK(parsed.parsed);
  CHECK(parsed.success);

  // A different prefix bound to the same namespaces is the same document.
  epos::Response other;
  CHECK(epos::parseResponse(
      "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "xmlns:e=\"http://www.epson-pos.com/schemas/2011/03/epos-print\">"
      "<SOAP-ENV:Body><e:response success=\"true\" status=\"2\"/></SOAP-ENV:Body>"
      "</SOAP-ENV:Envelope>",
      &other));
  CHECK(other.parsed);
  CHECK(other.success);

  // And an `s:response` whose prefix is bound to somebody else's namespace is NOT an
  // ePOS response, however familiar it looks. This is the case a prefix-matching parser
  // gets wrong in the direction that matters.
  epos::Response impostor;
  CHECK(!epos::parseResponse(
      "<s:Envelope xmlns:s=\"http://example.invalid/other\"><s:Body>"
      "<s:response success=\"true\" status=\"2\"/></s:Body></s:Envelope>",
      &impostor));
  CHECK(!impostor.parsed);
}

PD_TEST(epos_status_is_read_as_unsigned_32_bit) {
  epos::Response parsed;
  // 0x80000000 — spooler stopped. It does not fit in a signed 32-bit integer, and a
  // parser that reaches for one reports it as negative or as garbage.
  CHECK(epos::parseResponse(envelope("success=\"true\" status=\"2147483648\""), &parsed));
  CHECK_EQ(parsed.status, epos::status::kSpoolerStopped);
  CHECK((parsed.status & epos::status::kSpoolerStopped) != 0u);

  epos::Response all_bits;
  CHECK(epos::parseResponse(envelope("success=\"true\" status=\"4294967295\""), &all_bits));
  CHECK_EQ(all_bits.status, 0xFFFFFFFFu);
}

PD_TEST(epos_response_codes_round_trip_and_only_two_are_non_terminal) {
  CHECK_EQ(epos::responseCodeFrom("EPTR_COVER_OPEN"), epos::ResponseCode::EPTR_COVER_OPEN);
  CHECK_EQ(epos::responseCodeFrom("EPTR_REC_EMPTY"), epos::ResponseCode::EPTR_REC_EMPTY);
  CHECK_EQ(epos::responseCodeFrom("JobNotFound"), epos::ResponseCode::JobNotFound);
  CHECK_EQ(epos::responseCodeFrom("EX_SPOOLER"), epos::ResponseCode::EX_SPOOLER);
  CHECK_EQ(epos::responseCodeFrom(""), epos::ResponseCode::None);
  // A code this build has never heard of is Unrecognised, not silently a failure.
  CHECK_EQ(epos::responseCodeFrom("EPTR_FUTURE_THING"), epos::ResponseCode::Unrecognised);

  CHECK(epos::isNonTerminal(epos::ResponseCode::Printing));
  CHECK(epos::isNonTerminal(epos::ResponseCode::JobSpooling));
  CHECK(!epos::isNonTerminal(epos::ResponseCode::None));
  CHECK(!epos::isNonTerminal(epos::ResponseCode::JobNotFound));
  CHECK(!epos::isNonTerminal(epos::ResponseCode::EPTR_COVER_OPEN));
}

PD_TEST(epos_status_mask_lowers_to_device_events) {
  const std::vector<DeviceEvent> cover =
      epos::toDeviceEvents(epos::status::kCoverOpen | epos::status::kOffline);
  CHECK(std::find(cover.begin(), cover.end(), DeviceEvent::CoverOpen) != cover.end());
  CHECK(std::find(cover.begin(), cover.end(), DeviceEvent::Offline) != cover.end());

  const std::vector<DeviceEvent> paper = epos::toDeviceEvents(epos::status::kPaperEnd);
  CHECK(std::find(paper.begin(), paper.end(), DeviceEvent::PaperOut) != paper.end());

  const std::vector<DeviceEvent> near_end = epos::toDeviceEvents(epos::status::kRollNearEnd);
  CHECK(std::find(near_end.begin(), near_end.end(), DeviceEvent::PaperNearEnd) !=
        near_end.end());

  const std::vector<DeviceEvent> cutter = epos::toDeviceEvents(epos::status::kCutterError);
  CHECK(std::find(cutter.begin(), cutter.end(), DeviceEvent::CutterError) != cutter.end());
}

// --- The accepted-not-printed discipline ------------------------------------------------

PD_TEST(epos_first_success_on_a_spooler_printer_is_not_a_print) {
  // The single most damaging mistake available on this protocol, isolated: status carries
  // 0x2, "printing completed", and it means nothing yet.
  epos::Response ack;
  CHECK(epos::parseResponse(envelope("success=\"true\" code=\"\" status=\"2\""), &ack));

  const epos::Outcome spooled = epos::Client::classify(ack, /*spooler=*/true,
                                                       /*first=*/true);
  CHECK(!spooled.terminal);
  CHECK(spooled.accepted_not_printed);
  CHECK_EQ(spooled.result.outcome, JobOutcome::Unknown);

  // The same bytes on a printer with no spooler ARE completion: the response did not come
  // back until the data had printed.
  const epos::Outcome direct = epos::Client::classify(ack, /*spooler=*/false,
                                                      /*first=*/true);
  CHECK(direct.terminal);
  CHECK_EQ(direct.result.outcome, JobOutcome::Done);
  CHECK_EQ(direct.result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(direct.result.authority, CompletionAuthority::PhysicalPrinter);
}

PD_TEST(epos_initial_spooler_stopped_bit_is_not_a_failure_either) {
  // docs/wire-protocols.md §1: the enqueue acknowledgement's status is 0x00000002 **or**
  // 0x80000000. Neither is consulted on the first answer, so a spooler-stopped bit on the
  // ack does not fail a job that has not been asked about yet.
  epos::Response ack;
  CHECK(epos::parseResponse(envelope("success=\"true\" status=\"2147483648\""), &ack));
  const epos::Outcome outcome = epos::Client::classify(ack, true, true);
  CHECK(!outcome.terminal);
  CHECK_EQ(outcome.result.outcome, JobOutcome::Unknown);
}

// --- Over a socket -----------------------------------------------------------------------

PD_TEST(epos_spooled_job_walks_ack_printing_completed_and_earns_a_plus) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({
      envelope("success=\"true\" code=\"\" status=\"2\" battery=\"0\""),  // enqueue ack
      envelope("success=\"false\" code=\"Printing\" status=\"0\""),        // still working
      envelope("success=\"true\" code=\"\" status=\"2\""),                 // retrieved result
  });

  epos::Client client(configFor(server, /*spooler=*/true));
  epos::Document document;
  document.line("TABLE 4").cut();
  const epos::Outcome outcome = client.print(document.body(), "ABC123");

  CHECK(outcome.terminal);
  CHECK(outcome.accepted_not_printed);
  CHECK_EQ(outcome.result.outcome, JobOutcome::Done);
  CHECK_EQ(outcome.state, JobState::DoneSoftware);
  // The top of docs/compatibility-brief.md §24, and the only grade in this SDK that
  // survives losing the connection between submission and answer.
  CHECK_EQ(outcome.result.grade, ConfidenceGrade::APlus_DurableQueryableJob);
  CHECK_EQ(outcome.result.authority, CompletionAuthority::VendorSpooler);
  CHECK_EQ(outcome.result.method, std::string("ePOS JobID"));
  CHECK(outcome.polls >= 2);

  const std::vector<std::string> requests = server.requests();
  CHECK_EQ(requests.size(), static_cast<size_t>(3));
  // The submission carries the document; every retrieval is header-only with an empty
  // body, or the receipt would print once per poll.
  CHECK(contains(requests[0], "TABLE 4"));
  CHECK(contains(requests[0], "<printjobid>ABC123</printjobid>"));
  for (size_t i = 1; i < requests.size(); ++i) {
    CHECK(!contains(requests[i], "TABLE 4"));
    CHECK(contains(requests[i], "<printjobid>ABC123</printjobid>"));
  }
  // The documented headers.
  CHECK(contains(requests[0], "POST /cgi-bin/epos/service.cgi"));
  CHECK(contains(requests[0], "Content-Type: text/xml; charset=utf-8"));
  CHECK(contains(requests[0], "SOAPAction: \"\""));
  CHECK(contains(requests[0], "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT"));
  // The print service documents no authentication, so none is offered — and WebConfig's
  // administrative credentials are for a different service entirely.
  CHECK(!contains(requests[0], "Authorization"));
  server.stop();
}

PD_TEST(epos_job_spooling_is_non_terminal_just_like_printing) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({
      envelope("success=\"true\" status=\"2\""),
      envelope("success=\"false\" code=\"JobSpooling\" status=\"0\""),
      envelope("success=\"true\" status=\"2\""),
  });
  epos::Client client(configFor(server, true));
  const epos::Outcome outcome = client.print("<text>x</text>", "J2");
  CHECK_EQ(outcome.result.outcome, JobOutcome::Done);
  CHECK(outcome.polls >= 2);
  server.stop();
}

PD_TEST(epos_job_not_found_is_unknown_and_never_done_or_failed) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({
      envelope("success=\"true\" status=\"2\""),
      envelope("success=\"false\" code=\"JobNotFound\" status=\"0\""),
  });
  epos::Client client(configFor(server, true));
  const epos::Outcome outcome = client.print("<text>x</text>", "GONE1");

  // The spooler has no record. The durability this transport was chosen for is exactly
  // what failed, so the fate of the paper is genuinely unknown — and claiming either
  // answer would be worse than saying so.
  CHECK(outcome.terminal);
  CHECK_EQ(outcome.result.outcome, JobOutcome::Unknown);
  CHECK_EQ(outcome.state, JobState::Unknown);
  CHECK_EQ(outcome.result.grade, ConfidenceGrade::E_TransportOnly);
  server.stop();
}

PD_TEST(epos_device_faults_map_to_honest_failures) {
  struct Case {
    const char* code;
    FailureReason reason;
  };
  const Case cases[] = {
      {"EPTR_COVER_OPEN", FailureReason::PreflightCoverOpen},
      {"EPTR_REC_EMPTY", FailureReason::PreflightPaperOut},
      {"EPTR_CUTTER", FailureReason::CutterFault},
      {"EPTR_MECHANICAL", FailureReason::PreflightHardwareError},
      {"EPTR_UNRECOVERABLE", FailureReason::PreflightHardwareError},
      {"EPTR_BATTERY_LOW", FailureReason::PreflightHardwareError},
      {"ERROR_WAIT_EJECT", FailureReason::PreflightHardwareError},
      {"EX_SPOOLER", FailureReason::QueueOverflow},
      {"TooManyRequests", FailureReason::QueueOverflow},
      {"SchemaError", FailureReason::Unsupported},
      {"RequestEntityTooLarge", FailureReason::Unsupported},
      {"DeviceNotFound", FailureReason::TransportUnreachable},
      {"EX_BADPORT", FailureReason::TransportUnreachable},
      {"EX_TIMEOUT", FailureReason::TimeoutAwaitingCompletion},
  };
  for (const Case& item : cases) {
    FakeEposServer server;
    CHECK(server.start());
    server.setScript({envelope(std::string("success=\"false\" code=\"") + item.code +
                               "\" status=\"8\"")});
    epos::Client client(configFor(server, /*spooler=*/false));
    const epos::Outcome outcome = client.print("<text>x</text>", "E1");
    CHECK_EQ(outcome.result.outcome, JobOutcome::Failed);
    CHECK_EQ(outcome.state, JobState::FailedKnown);
    CHECK_EQ(outcome.result.reason, item.reason);
    // A fault the spooler reported is device status taken around the transmission: grade
    // C, and the authority is the spooler because that is what spoke.
    CHECK_EQ(outcome.result.grade, ConfidenceGrade::C_DeviceStatusAround);
    CHECK_EQ(outcome.result.authority, CompletionAuthority::VendorSpooler);
    server.stop();
  }
}

PD_TEST(epos_spooler_stopped_on_a_retrieval_is_a_failure) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({
      envelope("success=\"true\" status=\"2147483648\""),  // ack: means nothing yet
      envelope("success=\"true\" status=\"2147483648\""),  // retrieval: the queue is halted
  });
  epos::Client client(configFor(server, true));
  const epos::Outcome outcome = client.print("<text>x</text>", "STOP1");

  CHECK(outcome.terminal);
  CHECK_EQ(outcome.result.outcome, JobOutcome::Failed);
  CHECK_EQ(outcome.result.reason, FailureReason::PreflightHardwareError);
  server.stop();
}

PD_TEST(epos_cutter_error_bit_in_the_status_fails_the_job) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({envelope("success=\"true\" code=\"\" status=\"2050\"")});  // 0x802
  epos::Client client(configFor(server, /*spooler=*/false));
  const epos::Outcome outcome = client.print("<text>x</text>", "C1");
  CHECK_EQ(outcome.result.outcome, JobOutcome::Failed);
  CHECK_EQ(outcome.result.reason, FailureReason::CutterFault);
  server.stop();
}

PD_TEST(epos_non_spooler_printer_completes_on_the_submission) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({envelope("success=\"true\" code=\"\" status=\"2\"")});
  epos::Client client(configFor(server, /*spooler=*/false));
  const epos::Outcome outcome = client.print("<text>x</text>", "N1");

  CHECK(outcome.terminal);
  CHECK(!outcome.accepted_not_printed);
  CHECK_EQ(outcome.result.outcome, JobOutcome::Done);
  // Grade A and not A+: strong, immediate, and gone the moment the connection is lost.
  CHECK_EQ(outcome.result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(outcome.result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(outcome.polls, 0u);
  CHECK_EQ(server.requestCount(), static_cast<size_t>(1));
  server.stop();
}

PD_TEST(epos_running_out_of_patience_is_unknown_and_not_failed) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({
      envelope("success=\"true\" status=\"2\""),
      envelope("success=\"false\" code=\"Printing\" status=\"0\""),  // repeats forever
  });
  epos::ClientConfig config = configFor(server, true);
  config.poll_budget_ms = 60;
  epos::Client client(config);
  const epos::Outcome outcome = client.print("<text>x</text>", "SLOW1");

  // Out of *our* patience, not the printer's: the job may still print, so this can never
  // be Failed.
  CHECK_EQ(outcome.result.outcome, JobOutcome::Unknown);
  CHECK_EQ(outcome.result.reason, FailureReason::TimeoutAwaitingCompletion);
  server.stop();
}

PD_TEST(epos_unreachable_service_is_a_known_failure) {
  epos::ClientConfig config;
  config.host = "127.0.0.1";
  config.port = 1;  // nothing listens here
  config.connect_timeout_ms = 300;
  config.http_timeout_ms = 300;
  epos::Client client(config);
  const epos::Outcome outcome = client.print("<text>x</text>", "U1");

  // Nothing was accepted anywhere, which is the one ePOS failure that is provably not on
  // paper.
  CHECK_EQ(outcome.result.outcome, JobOutcome::Failed);
  CHECK_EQ(outcome.result.reason, FailureReason::TransportUnreachable);
  CHECK(!outcome.error.empty());
}

PD_TEST(epos_a_spooled_job_without_a_job_id_cannot_be_retrieved) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({envelope("success=\"true\" status=\"2\"")});
  epos::Client client(configFor(server, /*spooler=*/true));
  const epos::Outcome outcome = client.print("<text>x</text>", "");

  // The service assigns its own id when one is omitted, and we never learn it. There is
  // therefore nothing durable to ask about, and saying so beats a poll loop against
  // nothing.
  CHECK(outcome.terminal);
  CHECK_EQ(outcome.result.outcome, JobOutcome::Unknown);
  CHECK(!outcome.error.empty());
  server.stop();
}

PD_TEST(epos_status_events_are_reported_from_every_answer) {
  FakeEposServer server;
  CHECK(server.start());
  server.setScript({
      envelope("success=\"true\" status=\"2\""),
      envelope("success=\"false\" code=\"Printing\" status=\"131072\""),  // roll near end
      envelope("success=\"true\" status=\"2\""),
  });
  epos::Client client(configFor(server, true));
  const epos::Outcome outcome = client.print("<text>x</text>", "EV1");
  CHECK_EQ(outcome.result.outcome, JobOutcome::Done);
  bool near_end = false;
  for (const DeviceEvent event : outcome.events) {
    near_end = near_end || event == DeviceEvent::PaperNearEnd;
  }
  CHECK(near_end);
  server.stop();
}

PD_TEST(epos_declared_degradations_are_stated_rather_than_implied) {
  const std::vector<std::string>& degradations = epos::declaredDegradations();
  CHECK(degradations.size() >= 4);
  bool mentions_auth = false;
  bool mentions_tls = false;
  for (const std::string& entry : degradations) {
    mentions_auth = mentions_auth || entry.find("authentication") != std::string::npos;
    mentions_tls = mentions_tls || entry.find("TLS") != std::string::npos;
  }
  CHECK(mentions_auth);
  CHECK(mentions_tls);
}

PD_TEST(epos_spooler_matrix_is_recorded_per_model_and_not_inferred) {
  // docs/wire-protocols.md §1. The matrix is by exact model and firmware or it is
  // worthless: "OmniLink" in a product name is not a capability proxy, and model-number
  // order is not capability order — the TM-T88VI has the spooler and the TM-T88VII that
  // replaces it does not.
  const CapabilityProfile with_spooler[] = {
      devices::epson_tm_i(),
      devices::epson_tm_t88vi(),
  };
  for (const CapabilityProfile& profile : with_spooler) {
    CHECK(profile.epos.spooler);
    CHECK(profile.epos.job_id);
    CHECK_EQ(profile.epos.spooler_provenance, Provenance::Documented);
    CHECK(profile.transport.epos);
  }

  // Documented-ABSENT, which is a fact and not a gap: it is what stops a caller waiting
  // for a JobID result that will never exist.
  const CapabilityProfile without_spooler[] = {
      devices::epson_tm_t88vii(), devices::epson_tm_m10(), devices::epson_tm_m30(),
      devices::epson_tm_m30ii(),
  };
  for (const CapabilityProfile& profile : without_spooler) {
    CHECK(!profile.epos.spooler);
    CHECK(!profile.epos.job_id);
    CHECK_EQ(profile.epos.spooler_provenance, Provenance::Documented);
  }

  // Everything nobody has checked stays Unverified, which is what "generic" means.
  CHECK_EQ(devices::generic_80().epos.spooler_provenance, Provenance::Unverified);
  CHECK(!devices::generic_80().epos.spooler);

  // The service defaults the whole database carries.
  CHECK_EQ(devices::epson_tm_i().epos.device_id, std::string("local_printer"));
  CHECK_EQ(devices::epson_tm_i().epos.timeout_ms, 60000u);
}

PD_TEST(epos_mfi_strings_are_recorded_facts_and_citizen_is_blocked_by_policy) {
  // docs/wire-protocols.md §4. The MFi protocol string cannot be derived — an EASession
  // opened with the wrong one simply never opens — so it is carried per family.
  CHECK_EQ(devices::epson_tm_p20ii().transport.bluetooth.mfi_protocol,
           std::string("com.epson.escpos"));
  CHECK_EQ(devices::star_tsp100iv().transport.bluetooth.mfi_protocol,
           std::string("jp.star-m.starpro"));
  CHECK_EQ(devices::bixolon_spp_r310().transport.bluetooth.mfi_protocol,
           std::string("com.bixolon.protocol"));

  // Citizen's is vendor-gated: issued only through MFi registration and approval. Empty
  // *and* flagged, so "we have not looked it up" and "it exists and we are not allowed to
  // know it yet" stop being the same empty string.
  const CapabilityProfile citizen = devices::citizen_cmp_30ii();
  CHECK(citizen.transport.bluetooth.mfi_protocol.empty());
  CHECK(citizen.transport.bluetooth.mfi_protocol_vendor_gated);

  // None of the three publishes a raw GATT map, so none may be mapped onto a generic
  // BLE-UART profile by a scan that happens to find an FFE1 characteristic.
  CHECK(devices::epson_tm_p20ii().transport.bluetooth.ble_profile_unknown);
  CHECK(devices::star_sm_l200().transport.bluetooth.ble_profile_unknown);
  CHECK(devices::bixolon_spp_r310().transport.bluetooth.ble_profile_unknown);
}
