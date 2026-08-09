#include <memory>
#include <string>
#include <vector>

#include "fake_printer.hpp"
#include "printerdriver/probe_path.hpp"
#include "printerdriver/transport.hpp"
#include "test_harness.hpp"

// A print server changes the evidence boundary (docs/compatibility-brief.md §19-§23), and
// the only way to know which side of that boundary a given path leaves you on is to ask
// through it. These tests are the four answers a venue actually gives:
//
//   forwarding   the box passes responses both ways        -> PhysicalPrinter
//   swallowing   the box takes bytes and returns none      -> PRINT_SERVER_ONLY (§21)
//   partial      DLE EOT survives, the correlated echo does not
//   foreign      an echo comes back, but for somebody else's token
//
// The swallowing case runs over a real loopback socket rather than the mock, because the
// finding it produces is a claim about a socket: proving it against a function call would
// prove the classifier and not the path.

namespace {

pd::PathProbeOptions fastOptions() {
  pd::PathProbeOptions options;
  // The scripted device answers inside the write() that asked, or never; production
  // budgets would only make the negative cases wait out eight seconds each.
  options.status_timeout_ms = 300;
  options.fence_timeout_ms = 500;
  options.connect_timeout_ms = 1000;
  return options;
}

// A device behind a path that returns nothing at all: bytes are consumed, no answer is
// produced. This is the dumb USB/Ethernet server of §21, modelled at the only place its
// behaviour is observable from the host.
pdfake::Script swallowingScript() {
  pdfake::Script script;
  script.answer_realtime = false;
  script.answer_process_id = false;
  script.answer_queued_status = false;
  script.answer_asb = false;
  return script;
}

std::unique_ptr<pd::Transport> mockTransport(pdfake::MockLink& link) {
  return link.factory()();
}

bool mentions(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

PD_TEST(a_forwarding_path_returns_the_correlated_echo_and_grades_physical_printer) {
  pdfake::MockLink link;  // default script: answers real-time and echoes process IDs
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(findings.connected);
  CHECK(findings.wrote_status_query);
  CHECK(findings.wrote_fence);
  CHECK(findings.dle_eot_answered);
  CHECK(findings.fence_echoed);
  CHECK(!findings.foreign_token_echoed);
  CHECK_EQ(findings.authority, pd::CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(std::string(pd::pathAuthorityLabel(findings.authority)),
           std::string("PHYSICAL_PRINTER"));
  // The DLE EOT answer is kept raw, not only as a boolean: on an unfamiliar path the
  // actual byte is what a support engineer needs.
  CHECK_EQ(findings.dle_eot_response.size(), static_cast<size_t>(1));
  CHECK_EQ(findings.dle_eot_response[0], static_cast<uint8_t>(0x16));
  // The token the report quotes must be the token that was issued, or the correlation
  // claim is not checkable by anyone reading the report.
  CHECK_EQ(findings.token, std::string(pd::kPathProbeToken));
  CHECK(mentions(findings.rationale, "completionAuthority = PHYSICAL_PRINTER"));
}

PD_TEST(a_swallowing_path_over_a_real_socket_is_print_server_only) {
  // A real loopback socket, so the finding is proven across an actual TCP path: connect,
  // write, and a read side that stays open and silent for the whole budget.
  auto device = std::make_shared<pdfake::FakePrinter>(swallowingScript());
  pdfake::FakePrinterServer server(device);
  CHECK(server.start());

  pd::TcpConfig config;
  config.host = "127.0.0.1";
  config.port = server.port();
  pd::TcpTransport transport(config);

  pd::PathProbeOptions options = fastOptions();
  options.host = config.host;
  options.port = config.port;
  const pd::PathProbeFindings findings = pd::probePath(transport, options);

  CHECK(findings.connected);
  CHECK(findings.wrote_status_query);
  CHECK(findings.wrote_fence);
  CHECK(!findings.pathCarriesResponses());
  CHECK(findings.responsesSwallowed());
  CHECK(!findings.dle_eot_answered);
  CHECK(!findings.fence_echoed);
  CHECK_EQ(findings.authority, pd::CompletionAuthority::PrintServer);
  // docs/compatibility-brief.md §21, in the brief's own words. The report is a CI
  // artefact, so the exact phrase is part of the contract.
  CHECK(mentions(findings.rationale, "completionAuthority = PRINT_SERVER_ONLY"));

  // The bytes really did cross the socket: the far side heard both questions and chose
  // not to answer, which is the whole difference between this and an unreachable port.
  CHECK_EQ(device->realtimeRequests().size(), static_cast<size_t>(1));
  CHECK_EQ(device->markers().size(), static_cast<size_t>(1));
  CHECK_EQ(device->markers()[0].token, std::string(pd::kPathProbeToken));
  server.stop();
}

PD_TEST(a_swallowing_path_over_the_mock_transport_is_print_server_only) {
  pdfake::MockLink link;
  link.device->setScript(swallowingScript());
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(findings.responsesSwallowed());
  CHECK(!findings.correlationLostOnly());
  CHECK_EQ(findings.authority, pd::CompletionAuthority::PrintServer);
  CHECK(mentions(findings.rationale, "completionAuthority = PRINT_SERVER_ONLY"));
  // Never the transport's fault: the write side worked perfectly, which is exactly why
  // TCP success must not be promoted to physical completion here.
  CHECK(findings.write_error.empty());
}

PD_TEST(dle_eot_answering_without_the_fence_is_a_different_finding_from_silence) {
  pdfake::Script script;
  script.answer_realtime = true;    // the backchannel demonstrably reaches this host
  script.answer_process_id = false;  // but the correlated echo never arrives
  pdfake::MockLink link;
  link.device->setScript(script);
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(findings.dle_eot_answered);
  CHECK(!findings.fence_echoed);
  CHECK(findings.pathCarriesResponses());
  // The two negative cases must not collapse: this path is NOT the §21 print server.
  CHECK(findings.correlationLostOnly());
  CHECK(!findings.responsesSwallowed());
  CHECK_EQ(findings.authority, pd::CompletionAuthority::TransportOnly);
  CHECK(mentions(findings.rationale, "completionAuthority = TRANSPORT_ONLY"));
  CHECK(!mentions(findings.rationale, "PRINT_SERVER_ONLY"));
  // Both readings are reported rather than one being picked (brief §28).
  CHECK(mentions(findings.rationale, "does not implement fn 48"));
  CHECK(mentions(findings.rationale, "drops the correlated echo"));
}

PD_TEST(an_echo_carrying_somebody_elses_token_is_not_this_hosts_answer) {
  pdfake::Script script;
  script.answer_process_id = false;      // our token is never echoed
  script.foreign_process_id = "ZZ99";    // a second writer's receipt finishes instead
  pdfake::MockLink link;
  link.device->setScript(script);
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(!findings.fence_echoed);
  CHECK(findings.foreign_token_echoed);
  CHECK_EQ(findings.foreign_tokens.size(), static_cast<size_t>(1));
  CHECK_EQ(findings.foreign_tokens[0], std::string("ZZ99"));
  // A structurally perfect echo for the wrong token proves the path carries fn 48 frames
  // and proves nothing about this host's receipt (docs/sdk-spec.md §14).
  CHECK_EQ(findings.authority, pd::CompletionAuthority::TransportOnly);
  CHECK(mentions(findings.rationale, "ZZ99"));
  CHECK(mentions(findings.rationale, "completionAuthority = TRANSPORT_ONLY"));
}

PD_TEST(our_own_echo_wins_even_when_another_writer_answers_on_the_same_path) {
  pdfake::Script script;
  script.answer_process_id = true;
  script.foreign_process_id = "ZZ99";
  pdfake::MockLink link;
  link.device->setScript(script);
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(findings.fence_echoed);
  CHECK(findings.foreign_token_echoed);
  // The correlated answer arrived; the other writer is a warning on a sound verdict, not
  // a demotion of it.
  CHECK_EQ(findings.authority, pd::CompletionAuthority::PhysicalPrinter);
  CHECK(mentions(findings.rationale, "something else is writing to the same printer"));
}

PD_TEST(a_path_that_never_opens_proves_nothing_about_what_is_behind_it) {
  pdfake::MockLink link;
  link.behaviour.connect_fails = true;
  link.behaviour.connect_error = "connection refused";
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(!findings.connected);
  CHECK(!findings.wrote_status_query);
  CHECK(!findings.responsesSwallowed());
  // Emphatically not PRINT_SERVER_ONLY: an unreachable address is not a print server that
  // eats responses, and grading it as one would invent evidence out of a missing socket.
  CHECK_EQ(findings.authority, pd::CompletionAuthority::TransportOnly);
  CHECK(mentions(findings.rationale, "connection refused"));
  CHECK(mentions(findings.rationale, "completionAuthority = TRANSPORT_ONLY"));
}

PD_TEST(a_path_that_stops_accepting_bytes_is_reported_as_unasked_not_as_silent) {
  pdfake::MockLink link;
  link.behaviour.fail_write_after_bytes = 1;  // the DLE EOT query itself is cut short
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);

  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());

  CHECK(findings.connected);
  CHECK(!findings.wrote_status_query);
  CHECK(!findings.wrote_fence);
  CHECK(!findings.responsesSwallowed());
  CHECK_EQ(findings.authority, pd::CompletionAuthority::TransportOnly);
  CHECK(mentions(findings.rationale, "stopped accepting bytes"));
}

PD_TEST(the_classifier_is_a_pure_function_of_the_observation) {
  // No socket, no device: the rule table of docs/compatibility-brief.md §21 stated once,
  // so a change to it fails here before it can quietly change a report.
  pd::PathProbeFindings observation;
  CHECK_EQ(pd::classifyPath(observation), pd::CompletionAuthority::TransportOnly);

  observation.connected = true;
  observation.wrote_status_query = true;
  observation.wrote_fence = true;
  CHECK_EQ(pd::classifyPath(observation), pd::CompletionAuthority::PrintServer);

  // One byte back — any byte — and the path is no longer the swallowing kind.
  observation.raw.push_back(0x16);
  observation.dle_eot_answered = true;
  CHECK_EQ(pd::classifyPath(observation), pd::CompletionAuthority::TransportOnly);

  observation.fence_echoed = true;
  CHECK_EQ(pd::classifyPath(observation), pd::CompletionAuthority::PhysicalPrinter);

  // A correlated echo is authority even when the write log is incomplete only in the
  // sense of the connect having been asserted; drop the connect and nothing survives.
  observation.connected = false;
  CHECK_EQ(pd::classifyPath(observation), pd::CompletionAuthority::TransportOnly);
}

PD_TEST(every_authority_has_a_brief_shaped_label) {
  CHECK_EQ(std::string(pd::pathAuthorityLabel(pd::CompletionAuthority::PrintServer)),
           std::string("PRINT_SERVER_ONLY"));
  CHECK_EQ(std::string(pd::pathAuthorityLabel(pd::CompletionAuthority::PhysicalPrinter)),
           std::string("PHYSICAL_PRINTER"));
  CHECK_EQ(std::string(pd::pathAuthorityLabel(pd::CompletionAuthority::TransportOnly)),
           std::string("TRANSPORT_ONLY"));
  CHECK_EQ(std::string(pd::pathAuthorityLabel(pd::CompletionAuthority::VendorSpooler)),
           std::string("VENDOR_SPOOLER"));
  CHECK_EQ(std::string(pd::pathAuthorityLabel(pd::CompletionAuthority::PdAgent)),
           std::string("PD_AGENT"));
}

PD_TEST(a_caller_supplied_token_is_used_and_a_malformed_one_is_visibly_replaced) {
  {
    pdfake::MockLink link;
    const std::unique_ptr<pd::Transport> transport = mockTransport(link);
    pd::PathProbeOptions options = fastOptions();
    options.token = "AB12";
    const pd::PathProbeFindings findings = pd::probePath(*transport, options);
    CHECK_EQ(findings.token, std::string("AB12"));
    CHECK(findings.fence_echoed);
    CHECK_EQ(link.device->markers()[0].token, std::string("AB12"));
  }
  {
    // Five bytes is not a process ID. Substituting the default is honest only because the
    // token that was actually issued is reported back; nothing here is silent.
    pdfake::MockLink link;
    const std::unique_ptr<pd::Transport> transport = mockTransport(link);
    pd::PathProbeOptions options = fastOptions();
    options.token = "TOOLONG";
    const pd::PathProbeFindings findings = pd::probePath(*transport, options);
    CHECK_EQ(findings.token, std::string(pd::kPathProbeToken));
    CHECK(findings.fence_echoed);
  }
}

PD_TEST(the_probe_writes_nothing_printable_unless_a_test_line_is_asked_for) {
  pdfake::MockLink link;
  const std::unique_ptr<pd::Transport> transport = mockTransport(link);
  const pd::PathProbeFindings findings = pd::probePath(*transport, fastOptions());
  CHECK(findings.fence_echoed);
  // Nothing reached the print buffer: pointing this command at a venue costs no paper.
  CHECK_EQ(link.device->printDataBytes(), static_cast<size_t>(0));

  pdfake::MockLink printing;
  const std::unique_ptr<pd::Transport> second = mockTransport(printing);
  pd::PathProbeOptions options = fastOptions();
  options.print_test_line = true;
  const pd::PathProbeFindings printed = pd::probePath(*second, options);
  CHECK(printed.fence_echoed);
  // With the option on there is data ahead of the fence, which is what makes the echo an
  // ordered answer rather than a bare capability check.
  CHECK(printing.device->printDataBytes() > 0);
  CHECK(printing.device->markers()[0].print_data_len > 0);
}

PD_TEST(a_forwarding_path_over_a_real_socket_survives_the_socket) {
  auto device = std::make_shared<pdfake::FakePrinter>();
  pdfake::FakePrinterServer server(device);
  CHECK(server.start());

  pd::TcpConfig config;
  config.host = "127.0.0.1";
  config.port = server.port();
  pd::TcpTransport transport(config);

  pd::PathProbeOptions options = fastOptions();
  // A real socket is asynchronous: the answer arrives on the reader thread some time
  // after write() returns, so both budgets have to be wide enough to lose a scheduling
  // slice without turning a working path into a reported print server.
  options.status_timeout_ms = 2000;
  options.fence_timeout_ms = 3000;
  const pd::PathProbeFindings findings = pd::probePath(transport, options);

  CHECK(findings.connected);
  CHECK(findings.dle_eot_answered);
  CHECK(findings.fence_echoed);
  CHECK_EQ(findings.authority, pd::CompletionAuthority::PhysicalPrinter);
  CHECK(findings.endpoint.find("127.0.0.1") != std::string::npos);
  server.stop();
}
