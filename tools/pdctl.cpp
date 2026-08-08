#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "printerdriver/capability_probe.hpp"
#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/identity.hpp"
#include "printerdriver/response_parser.hpp"
#include "printerdriver/transport.hpp"

// Diagnostic front end for the core. `print` deliberately goes through the whole
// engine — preflight, fences, job store, tri-state result — because a CLI that
// bypassed the engine would prove nothing about the engine.
//
// The commands split in two. `status`, `probe`, `identify` and `print` are safe to
// point at a printer in a working venue. `recover`, `counters`, `test-print` and
// `settings` are deliberate operator actions: they either consume paper or change what
// the device does with a job that is already in its buffer, so each one prints a
// warning banner naming exactly what it is about to send.

namespace {

using namespace std::chrono_literals;

constexpr int kExitDone = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUnknown = 2;
constexpr int kExitUsage = 64;

constexpr int kLabelWidth = 26;

struct Endpoint {
  std::string host;
  uint16_t port = 9100;
};

bool parseEndpoint(const std::string& text, Endpoint* out) {
  const size_t colon = text.rfind(':');
  if (colon == std::string::npos) {
    out->host = text;
    return !out->host.empty();
  }
  out->host = text.substr(0, colon);
  const std::string port_text = text.substr(colon + 1);
  if (out->host.empty() || port_text.empty()) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(port_text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || value <= 0 || value > 65535) {
    return false;
  }
  out->port = static_cast<uint16_t>(value);
  return true;
}

std::string hex(const std::vector<uint8_t>& bytes) {
  std::ostringstream os;
  os << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) {
      os << ' ';
    }
    os << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return os.str();
}

void section(const char* title) {
  std::cout << "\n" << title << "\n"
            << "----------------------------------------------------\n";
}

void row(const char* label, const std::string& value) {
  std::cout << "  " << std::left << std::setw(kLabelWidth) << label << value << "\n";
}

void row(const char* label, const char* value) { row(label, std::string(value)); }

// Three states, printed as three words: a capability nobody established is not the
// same as one that is absent.
std::string tri(const std::optional<bool>& value) {
  if (!value.has_value()) {
    return "UNKNOWN";
  }
  return *value ? "YES" : "NO";
}

std::string yesNo(bool value) { return value ? "YES" : "NO"; }

void printFlag(const char* label, const std::optional<bool>& value) {
  std::cout << "    " << std::left << std::setw(24) << label << std::right;
  if (!value.has_value()) {
    std::cout << "not reported\n";
  } else {
    std::cout << (*value ? "yes" : "no") << "\n";
  }
}

// Keeps the raw stream and the parsed events side by side, because on unfamiliar
// hardware the bytes nobody could classify are the interesting part.
class Listener {
 public:
  void attach(pd::Transport& transport) {
    transport.onBytes([this](const uint8_t* data, size_t size) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        raw_.insert(raw_.end(), data, data + size);
        for (pd::escpos::ParsedEvent& event : parser_.feed(data, size)) {
          events_.push_back(event);
        }
      }
      cv_.notify_all();
    });
    transport.onDisconnected([this](pd::TransportError, const std::string& message) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnected_ = true;
        disconnect_message_ = message;
      }
      cv_.notify_all();
    });
  }

  void expectRealtime(pd::escpos::DleEotKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.expectRealtime(kind);
  }

  bool waitForEvents(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] {
      return disconnected_ || events_.size() >= count;
    }) && events_.size() >= count;
  }

  bool waitForBytes(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, count] { return disconnected_ || raw_.size() >= count; });
  }

  std::vector<uint8_t> raw() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raw_;
  }
  std::vector<pd::escpos::ParsedEvent> events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    raw_.clear();
    events_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  pd::escpos::ResponseParser parser_;
  std::vector<uint8_t> raw_;
  std::vector<pd::escpos::ParsedEvent> events_;
  bool disconnected_ = false;
  std::string disconnect_message_;
};

int usage() {
  std::cout <<
      "pdctl - PrinterDriver core diagnostics\n"
      "\n"
      "read-only commands:\n"
      "  pdctl status <host[:port]>\n"
      "      DLE EOT 1-4 decoded, plus the raw response bytes.\n"
      "\n"
      "  pdctl probe <host[:port]> [--mac <address>] [--quiet-paper]\n"
      "      Full discovery report: identity, media, interfaces, ESC/POS\n"
      "      capabilities, completion mechanism, cutter, ranked transports and\n"
      "      the selected profile. Non-destructive: DLE EOT, GS I, GS ( H, GS r,\n"
      "      ASB on/off. Never DLE ENQ, never a power-off, never a buffer clear.\n"
      "      Prints two short test lines and does not cut (--quiet-paper skips them,\n"
      "      which weakens the completion answers).\n"
      "\n"
      "  pdctl identify <host[:port]> [--mac <address>] [--vendor <name>]\n"
      "      Fingerprint only. GS I is never trusted on its own.\n"
      "\n"
      "  pdctl print <host[:port]> --text \"...\" [options]\n"
      "      Print a small receipt through the full engine and stream job events.\n"
      "      --key <k>                   idempotency key (default: generated)\n"
      "      --profile <name>            capability profile (default: xp-s260m;\n"
      "                                  --profile list prints the database)\n"
      "      --no-cut                    do not cut after printing\n"
      "      --width <dots>              384 | 504 | 576 (default: 576)\n"
      "      --store <dir>               job store directory\n"
      "                                  (default: $HOME/.printerdriver)\n"
      "      --skip-preflight            do not refuse on cover/paper/error\n"
      "      --timeout <ms>              completion-wait budget per phase\n"
      "\n"
      "operator commands - these change the device or consume paper:\n"
      "  pdctl recover <host[:port]> --resume|--clear\n"
      "      DLE ENQ 1 / DLE ENQ 2. --resume reprints from the line the error\n"
      "      happened on, which can duplicate a ticket; --clear discards the\n"
      "      buffers, which can lose one. Never sent automatically.\n"
      "  pdctl counters <host[:port]>    GS g 2 maintenance counters\n"
      "  pdctl test-print <host[:port]>  GS ( A built-in test print (uses paper)\n"
      "  pdctl settings <host[:port]>    GS ( E fn 4 / fn 6 settings readback\n"
      "\n"
      "default port is 9100\n"
      "exit codes: 0 done, 1 failed, 2 unknown, 64 usage\n"
      "\n"
      "The core owns the printer connection exclusively: stop CUPS and every other\n"
      "client before pointing pdctl at a printer, or acknowledgements cannot be\n"
      "attributed to a receipt.\n";
  return kExitUsage;
}

// --- status -----------------------------------------------------------------------

int runStatus(const Endpoint& endpoint) {
  pd::TcpConfig config;
  config.host = endpoint.host;
  config.port = endpoint.port;
  pd::TcpTransport transport(config);
  Listener listener;
  listener.attach(transport);

  const pd::TransportResult connected = transport.connect();
  if (!connected.ok) {
    std::cout << "connect failed: " << connected.message << "\n";
    return kExitFailed;
  }
  std::cout << "connected to " << transport.describe() << "\n";

  const pd::escpos::DleEotKind kinds[] = {
      pd::escpos::DleEotKind::PrinterStatus, pd::escpos::DleEotKind::OfflineCause,
      pd::escpos::DleEotKind::ErrorCause, pd::escpos::DleEotKind::PaperSensor};
  pd::escpos::Bytes request;
  for (const pd::escpos::DleEotKind kind : kinds) {
    listener.expectRealtime(kind);
    const pd::escpos::Bytes command = pd::escpos::dleEot(kind);
    request.insert(request.end(), command.begin(), command.end());
  }
  const pd::TransportResult written = transport.write(request);
  if (!written.ok) {
    std::cout << "write failed: " << written.message << "\n";
    transport.close();
    return kExitFailed;
  }
  listener.waitForEvents(4, 2000ms);

  const std::vector<uint8_t> raw = listener.raw();
  std::cout << "raw response: " << (raw.empty() ? "NO RESPONSE" : hex(raw)) << "\n";
  if (raw.empty()) {
    std::cout << "\nno backchannel on this interface: the printer may still print,\n"
                 "but no completion fence can ever be verified over it\n";
    transport.close();
    return kExitFailed;
  }

  static const char* kNames[] = {"DLE EOT 1 (printer status)", "DLE EOT 2 (offline cause)",
                                 "DLE EOT 3 (error cause)", "DLE EOT 4 (paper sensor)"};
  for (const pd::escpos::ParsedEvent& event : listener.events()) {
    if (event.kind != pd::escpos::ParsedEventKind::RealtimeStatus &&
        event.kind != pd::escpos::ParsedEventKind::AsbStatus) {
      std::cout << "\n  " << pd::escpos::to_string(event.kind) << ": 0x" << std::hex
                << std::uppercase << static_cast<unsigned>(event.byte) << std::dec << "\n";
      continue;
    }
    if (event.kind == pd::escpos::ParsedEventKind::AsbStatus) {
      std::cout << "\n  ASB status frame\n";
    } else {
      const size_t index = static_cast<size_t>(event.realtime_kind) - 1;
      std::cout << "\n  " << (index < 4 ? kNames[index] : "DLE EOT ?") << "  = 0x"
                << std::hex << std::uppercase << static_cast<unsigned>(event.byte)
                << std::dec << "\n";
    }
    printFlag("online", event.flags.online);
    printFlag("cover open", event.flags.cover_open);
    printFlag("paper out", event.flags.paper_out);
    printFlag("paper near end", event.flags.paper_near_end);
    printFlag("cutter error", event.flags.cutter_error);
    printFlag("unrecoverable error", event.flags.unrecoverable_error);
    printFlag("recoverable error", event.flags.auto_recoverable_error);
    printFlag("drawer pin high", event.flags.drawer_pin_high);
  }
  transport.close();
  return kExitDone;
}

// --- probe / identify ---------------------------------------------------------------

struct DiscoveryArgs {
  std::string mac;
  std::string vendor_hint;
  bool print_test_lines = true;
};

struct Discovery {
  bool connected = false;
  std::string mac;
  pd::CapabilityFindings findings;
  pd::IdentityAssessment assessment;
  pd::CapabilityProfile profile;
  bool port_9100 = false;
  bool port_631 = false;
  bool port_515 = false;
  bool port_80 = false;
};

bool interrogate(const Endpoint& endpoint, const DiscoveryArgs& args, Discovery* out) {
  pd::TcpConfig config;
  config.host = endpoint.host;
  config.port = endpoint.port;
  pd::TcpTransport transport(config);

  pd::ProbeOptions options;
  options.endpoint = transport.describe();
  options.print_test_lines = args.print_test_lines;
  options.hints.mac = args.mac;
  options.hints.vendor_hint = args.vendor_hint;
  pd::CapabilityProbe probe(options);
  transport.onBytes([&probe](const uint8_t* data, size_t size) {
    probe.onBytes(data, size);
  });
  transport.onDisconnected([](pd::TransportError, const std::string&) {});

  const pd::TransportResult connected = transport.connect();
  if (!connected.ok) {
    std::cout << "connect failed: " << connected.message << "\n";
    return false;
  }
  out->connected = true;
  out->mac = args.mac;
  out->findings = probe.run([&transport](const pd::escpos::Bytes& bytes) {
    return transport.write(bytes).ok;
  });
  transport.close();

  out->assessment = pd::identify(out->findings.reported,
                                 pd::IdentityHints{args.mac, args.vendor_hint},
                                 out->findings.behaviour());
  out->profile = pd::promote(pd::devices::byName(out->assessment.profile_guess),
                             out->findings);

  out->port_9100 = pd::tcpPortOpen(endpoint.host, 9100, 700);
  out->port_631 = pd::tcpPortOpen(endpoint.host, 631, 700);
  out->port_515 = pd::tcpPortOpen(endpoint.host, 515, 700);
  out->port_80 = pd::tcpPortOpen(endpoint.host, 80, 700);
  return true;
}

void printIdentitySection(const Discovery& discovery) {
  const pd::IdentityAssessment& assessment = discovery.assessment;
  section("Identity");
  row("probable vendor", assessment.vendor_guess);
  row("probable profile", assessment.profile_guess);
  row("GS I manufacturer", assessment.reported_manufacturer.empty()
                               ? "not reported"
                               : assessment.reported_manufacturer);
  row("GS I model", assessment.reported_model.empty() ? "not reported"
                                                      : assessment.reported_model);
  row("GS I firmware", assessment.firmware.empty() ? "not reported" : assessment.firmware);
  row("GS I serial", assessment.serial.empty() ? "not reported" : assessment.serial);
  row("MAC OUI", assessment.oui_vendor.empty()
                     ? (discovery.mac.empty() ? "not supplied" : "unknown to the table")
                     : assessment.oui_vendor);
  row("identity trusted", yesNo(assessment.identity_trusted));
  row("impersonation suspected", yesNo(assessment.impersonation_suspected));
  row("confidence", std::to_string(assessment.confidence_percent) + "%");
  row("findings key", discovery.findings.key);
  for (const std::string& signal : assessment.signals) {
    std::cout << "    - " << signal << "\n";
  }
}

int runProbe(const Endpoint& endpoint, const DiscoveryArgs& args) {
  Discovery discovery;
  if (!interrogate(endpoint, args, &discovery)) {
    return kExitFailed;
  }
  const pd::CapabilityFindings& findings = discovery.findings;
  const pd::CapabilityProfile& profile = discovery.profile;

  std::cout << "Printer Discovery Report - " << endpoint.host << ":" << endpoint.port
            << "\n";

  printIdentitySection(discovery);

  section("Media");
  // Roll width and raster width are separate facts; the probe cannot measure either,
  // so both come from the selected profile and both are worth checking by hand.
  row("nominal paper", std::to_string(profile.media.nominal_roll_width_mm) + " mm");
  row("printable width", std::to_string(profile.media.printable_width_dots) + " dots");
  row("resolution", std::to_string(profile.media.dpi) + " dpi");
  row("58 mm guide", yesNo(profile.media.paper_guide_58mm));
  row("paper-end sensor", yesNo(profile.media.paper_end_sensor));
  row("near-end sensor", yesNo(profile.media.near_end_sensor));
  row("black-mark sensor", yesNo(profile.media.black_mark_sensor));
  row("cover sensor", yesNo(profile.media.cover_sensor));

  section("Interfaces");
  // The port that answered this probe is a fact; the rest are a scan of the ports a
  // receipt printer commonly exposes, and each one is a different transport with
  // different completion semantics (docs/device-database.md).
  row("probed port", std::to_string(endpoint.port) + " open");
  row("TCP 9100 raw", yesNo(discovery.port_9100));
  row("TCP 631 IPP", yesNo(discovery.port_631));
  row("TCP 515 LPR", yesNo(discovery.port_515));
  row("TCP 80 HTTP", yesNo(discovery.port_80));

  section("ESC/POS capabilities");
  row("DLE EOT 1", tri(findings.dle_eot_1));
  row("DLE EOT 2", tri(findings.dle_eot_2));
  row("DLE EOT 3", tri(findings.dle_eot_3));
  row("DLE EOT 4", tri(findings.dle_eot_4));
  row("GS r 1", tri(findings.gs_r1));
  row("GS ( H fn48", tri(findings.gs_h_process_id));
  row("GS a ASB", tri(findings.asb));
  row("GS I", tri(findings.gs_i));
  if (!findings.unclassified.empty()) {
    row("unclassified bytes", hex(findings.unclassified));
  }

  section("Completion");
  const pd::JobEvidence evidence = profile.evidence();
  row("mechanism", to_string(profile.completion));
  row("ordered fence", yesNo(profile.completion != pd::CompletionMechanism::None));
  row("correlated per receipt",
      yesNo(profile.completion == pd::CompletionMechanism::GsParenH));
  row("confidence grade", std::string(pd::gradeLetter(evidence.grade)) + " (" +
                              pd::to_string(evidence.grade) + ")");
  row("authority", pd::to_string(evidence.authority));
  row("method", evidence.method);
  row("evidence ceiling", pd::to_string(profile.maxConfidence()));
  row("one job in flight", yesNo(profile.completion_caps.one_job_in_flight));

  section("Cutter");
  row("autocutter", yesNo(profile.media.cutter));
  row("full cut", yesNo(profile.media.full_cut));
  row("partial cut", yesNo(profile.media.partial_cut));
  row("cutter error status", tri(findings.cutter_error_status));

  section("Recovery (operator only, never automatic)");
  row("DLE ENQ 1 resume", yesNo(profile.recovery.dle_enq_resume));
  row("DLE ENQ 2 clear", yesNo(profile.recovery.dle_enq_clear));
  row("buffer clear", yesNo(profile.recovery.clear_buffers));

  section("Recommended transports");
  const bool strong = profile.completion == pd::CompletionMechanism::GsParenH;
  row("direct TCP 9100", discovery.port_9100 ? (strong ? "*****" : "****") : "-----");
  row("ePOS", profile.transport.epos ? "****" : "-----");
  row("CUPS custom backend", discovery.port_631 ? "****" : "-----");
  row("generic CUPS socket", discovery.port_9100 ? "**" : "-----");
  row("LPD", discovery.port_515 ? "*" : "-----");

  section("Profile");
  row("selected", profile.name);
  row("language", pd::to_string(profile.language));
  row("promoted by probe", yesNo(profile.probed));
  row("unreliable identity", yesNo(profile.quirks.unreliable_identity));
  row("code page", std::to_string(static_cast<int>(profile.code_page)));
  row("final feed lines", std::to_string(profile.final_feed_lines));

  std::cout << "\nprobe results are keyed by identity and cached by the driver, so a\n"
               "printer only answers these questions once per firmware revision\n";
  return findings.empty() ? kExitFailed : kExitDone;
}

int runIdentify(const Endpoint& endpoint, DiscoveryArgs args) {
  // Identification still runs the behaviour questions: a device claiming to be an
  // Epson TM that cannot answer DLE EOT is contradicting itself, and that is the
  // strongest signal there is. No test lines, so nothing is printed.
  args.print_test_lines = false;
  Discovery discovery;
  if (!interrogate(endpoint, args, &discovery)) {
    return kExitFailed;
  }
  std::cout << "Fingerprint - " << endpoint.host << ":" << endpoint.port << "\n";
  printIdentitySection(discovery);

  section("Behaviour");
  row("DLE EOT", tri(discovery.findings.dle_eot));
  row("GS ( H fn48", tri(discovery.findings.gs_h_process_id));
  row("GS r 1", tri(discovery.findings.gs_r1));
  row("GS a ASB", tri(discovery.findings.asb));

  if (!discovery.assessment.identity_trusted) {
    std::cout << "\nidentity NOT trusted: GS I is a string the firmware chooses, and\n"
                 "at least one printer family ships answering as somebody else's model\n";
  }
  return discovery.findings.reported.answered() ? kExitDone : kExitFailed;
}

// --- operator commands ---------------------------------------------------------------

void warningBanner(const std::string& what, const std::string& consequence) {
  std::cout << "\n"
            << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
            << "!!  DELIBERATE OPERATOR COMMAND\n"
            << "!!  about to send: " << what << "\n"
            << "!!  consequence:   " << consequence << "\n"
            << "!!  the driver never sends this by itself\n"
            << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n";
}

// Sends a sequence of labelled requests and dumps whatever comes back, raw and, where
// the device used the Header/data/NUL framing, decoded.
int runOperatorCommand(const Endpoint& endpoint, const std::string& what,
                       const std::string& consequence,
                       const std::vector<std::pair<std::string, pd::escpos::Bytes>>& steps,
                       std::chrono::milliseconds wait) {
  warningBanner(what, consequence);

  pd::TcpConfig config;
  config.host = endpoint.host;
  config.port = endpoint.port;
  pd::TcpTransport transport(config);
  Listener listener;
  listener.attach(transport);

  const pd::TransportResult connected = transport.connect();
  if (!connected.ok) {
    std::cout << "connect failed: " << connected.message << "\n";
    return kExitFailed;
  }
  std::cout << "connected to " << transport.describe() << "\n";

  bool any_answer = false;
  for (const auto& step : steps) {
    listener.clear();
    const pd::TransportResult written = transport.write(step.second);
    if (!written.ok) {
      std::cout << "write failed: " << written.message << "\n";
      transport.close();
      return kExitFailed;
    }
    listener.waitForBytes(1, wait);
    const std::vector<uint8_t> raw = listener.raw();
    std::cout << "  " << std::left << std::setw(kLabelWidth) << step.first
              << (raw.empty() ? "no answer" : hex(raw));
    if (const std::optional<std::string> text = pd::parseGsIString(raw)) {
      std::cout << "   \"" << *text << "\"";
    }
    std::cout << "\n";
    any_answer = any_answer || !raw.empty();
  }
  transport.close();
  return any_answer ? kExitDone : kExitUnknown;
}

int runRecover(const Endpoint& endpoint, bool clear_buffers) {
  const std::string what = clear_buffers ? "DLE ENQ 2 (10 05 02)" : "DLE ENQ 1 (10 05 01)";
  const std::string consequence =
      clear_buffers
          ? "discards the receive and print buffers: a ticket in flight is LOST"
          : "resumes from the line the error occurred on: a ticket may be DUPLICATED";
  std::vector<std::pair<std::string, pd::escpos::Bytes>> steps;
  steps.emplace_back(clear_buffers ? "DLE ENQ 2 clear" : "DLE ENQ 1 resume",
                     pd::escpos::dleEnq(clear_buffers ? pd::escpos::kDleEnqClearBuffers
                                                      : pd::escpos::kDleEnqResume));
  // These commands answer with nothing on most firmware, so a silent success is the
  // expected outcome rather than a fault.
  const int result = runOperatorCommand(endpoint, what, consequence, steps, 1500ms);
  return result == kExitUnknown ? kExitDone : result;
}

int runCounters(const Endpoint& endpoint) {
  // Counter numbering is model-specific. The answers are printed raw, with the
  // number that produced them, rather than under invented labels.
  const uint16_t counters[] = {10, 11, 20, 21, 50, 70};
  std::vector<std::pair<std::string, pd::escpos::Bytes>> steps;
  for (const uint16_t counter : counters) {
    steps.emplace_back("GS g 2 counter " + std::to_string(counter),
                       pd::escpos::gsMaintenanceCounter(counter));
  }
  return runOperatorCommand(endpoint, "GS g 2 (maintenance counter reads)",
                            "read-only, but unsupported counter numbers make some "
                            "firmware print or stall",
                            steps, 1500ms);
}

int runTestPrint(const Endpoint& endpoint) {
  std::vector<std::pair<std::string, pd::escpos::Bytes>> steps;
  steps.emplace_back("GS ( A status sheet", pd::escpos::gsTestPrint(0, 2));
  return runOperatorCommand(endpoint, "GS ( A (built-in test print)",
                            "the printer prints its own status sheet and consumes paper",
                            steps, 3000ms);
}

int runSettings(const Endpoint& endpoint) {
  std::vector<std::pair<std::string, pd::escpos::Bytes>> steps;
  for (uint8_t switch_number = 1; switch_number <= 8; ++switch_number) {
    steps.emplace_back("GS ( E fn4 memory sw " + std::to_string(switch_number),
                       pd::escpos::gsReadMemorySwitch(switch_number));
  }
  for (uint8_t setting = 1; setting <= 6; ++setting) {
    steps.emplace_back("GS ( E fn6 custom " + std::to_string(setting),
                       pd::escpos::gsReadCustomizedSetting(setting));
  }
  return runOperatorCommand(
      endpoint, "GS ( E fn 4 / fn 6 (settings readback)",
      "reads only, but this is the command family that also writes settings", steps,
      1200ms);
}

// --- print ------------------------------------------------------------------------

struct PrintArgs {
  std::string text;
  std::string key;
  std::string profile = "xp-s260m";
  std::string store;
  uint32_t width = pd::escpos::kWidth80mm;
  bool cut = true;
  bool preflight = true;
  uint32_t timeout_ms = 0;
};

std::string defaultStore() {
  const char* home = std::getenv("HOME");
  return std::string(home != nullptr ? home : ".") + "/.printerdriver";
}

pd::CapabilityProfile profileByName(const std::string& name) {
  if (name == "xp-s260m") {
    return pd::xp_s260m();
  }
  if (name == "generic") {
    return pd::generic_escpos();
  }
  return pd::devices::byName(name);
}

bool profileExists(const std::string& name) {
  return name == "xp-s260m" || name == "generic" || pd::devices::exists(name);
}

int runPrint(const Endpoint& endpoint, const PrintArgs& args) {
  const pd::CapabilityProfile profile = profileByName(args.profile);

  pd::PrinterDriver driver(pd::StorageConfig::at(args.store));
  pd::PrinterConfig config;
  config.id = endpoint.host + ":" + std::to_string(endpoint.port);
  config.transport = pd::tcp(endpoint.host, endpoint.port, 3000);
  config.width_dots = args.width;
  config.profile = profile;
  auto printer = driver.addPrinter(config);

  std::cout << "printer  " << config.id << "\n"
            << "profile  " << profile.name << " (" << to_string(profile.completion)
            << ", ceiling " << pd::to_string(profile.maxConfidence()) << ", grade "
            << pd::gradeLetter(profile.evidence().grade) << ")\n"
            << "width    " << args.width << " dots\n"
            << "store    " << driver.store().journalPath() << "\n\n";

  printer->subscribe([](pd::DeviceEvent event) {
    std::cout << "  device  " << pd::to_string(event) << "\n" << std::flush;
  });

  pd::escpos::Encoder receipt;
  receipt.selectCodePage(profile.code_page)
      .align(pd::escpos::Alignment::Center)
      .bold(true)
      .textSize(2, 2)
      .line("PRINTERDRIVER")
      .textSize(1, 1)
      .bold(false)
      .line("pdctl test receipt")
      .feed()
      .align(pd::escpos::Alignment::Left)
      .line("--------------------------------")
      .line(args.text)
      .line("--------------------------------");
  if (!args.key.empty()) {
    receipt.line("ORDER: " + args.key).qr(args.key, 4);
  }

  pd::JobOptions options;
  options.key = args.key;
  options.cut = args.cut ? pd::CutSetting::Profile : pd::CutSetting::None;
  options.preflight = args.preflight ? pd::PreflightMode::Strict : pd::PreflightMode::Skip;
  options.timeout_ms = args.timeout_ms;

  auto job = printer->print(pd::Payload::document(receipt), options);
  std::cout << "job      " << job->id() << "\nkey      " << job->key() << "\n\n";
  job->subscribe([](const pd::JobEvent& event) {
    std::cout << "  " << std::left << std::setw(22) << pd::to_string(event.state)
              << std::setw(20) << pd::to_string(event.confidence);
    if (event.reason.has_value() && *event.reason != pd::FailureReason::None) {
      std::cout << pd::to_string(*event.reason);
    }
    std::cout << "\n" << std::flush;
  });

  const pd::JobResult result = job->result();
  std::cout << "\nresult   " << pd::to_string(result.outcome) << "\nevidence "
            << pd::to_string(result.confidence) << "\ngrade    "
            << pd::gradeLetter(result.grade) << " (" << pd::to_string(result.grade)
            << ")\nauthority " << pd::to_string(result.authority) << "\nmethod   "
            << result.method << "\n";
  if (result.reason != pd::FailureReason::None) {
    std::cout << "reason   " << pd::to_string(result.reason) << "\n";
  }
  switch (result.outcome) {
    case pd::JobOutcome::Done:
      return kExitDone;
    case pd::JobOutcome::Failed:
      return kExitFailed;
    case pd::JobOutcome::Unknown:
      // Bytes were sent and nothing acknowledged them. Never retried automatically:
      // for a kitchen ticket a duplicate is as damaging as a missing one.
      std::cout << "\nthis job is UNKNOWN, not failed: bytes were sent and the receipt\n"
                   "may have printed. Resolve it by inspecting the paper, then either\n"
                   "accept it or reprint deliberately with the same key.\n";
      return kExitUnknown;
  }
  return kExitUnknown;
}

int listProfiles() {
  std::cout << "device database (docs/device-database.md):\n\n";
  for (const std::string& name : pd::devices::names()) {
    const pd::CapabilityProfile profile = pd::devices::byName(name);
    std::cout << "  " << std::left << std::setw(20) << name << std::setw(18)
              << to_string(profile.completion) << std::setw(8)
              << (std::to_string(profile.media.nominal_roll_width_mm) + "mm")
              << std::setw(10)
              << (std::to_string(profile.media.printable_width_dots) + " dots")
              << profile.identity.vendor << " " << profile.identity.model << "\n";
  }
  std::cout << "\n  plus xp-s260m and generic, the two built-in engine profiles\n";
  return kExitDone;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "print" && std::string(argv[2]) == "list") {
    return listProfiles();
  }
  if (argc < 3) {
    return usage();
  }
  const std::string command = argv[1];
  Endpoint endpoint;
  if (!parseEndpoint(argv[2], &endpoint)) {
    std::cout << "invalid host: " << argv[2] << "\n\n";
    return usage();
  }

  try {
    if (command == "status") {
      return runStatus(endpoint);
    }
    if (command == "probe" || command == "identify") {
      DiscoveryArgs args;
      for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--mac" && has_value) {
          args.mac = argv[++i];
        } else if (flag == "--vendor" && has_value) {
          args.vendor_hint = argv[++i];
        } else if (flag == "--quiet-paper") {
          args.print_test_lines = false;
        } else {
          std::cout << "unknown option: " << flag << "\n\n";
          return usage();
        }
      }
      return command == "probe" ? runProbe(endpoint, args) : runIdentify(endpoint, args);
    }
    if (command == "recover") {
      std::optional<bool> clear_buffers;
      for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--resume") {
          clear_buffers = false;
        } else if (flag == "--clear") {
          clear_buffers = true;
        } else {
          std::cout << "unknown option: " << flag << "\n\n";
          return usage();
        }
      }
      if (!clear_buffers.has_value()) {
        std::cout << "recover requires --resume or --clear: which one it is decides\n"
                     "whether a ticket in flight is duplicated or lost\n\n";
        return usage();
      }
      return runRecover(endpoint, *clear_buffers);
    }
    if (command == "counters") {
      return runCounters(endpoint);
    }
    if (command == "test-print") {
      return runTestPrint(endpoint);
    }
    if (command == "settings") {
      return runSettings(endpoint);
    }
    if (command == "print") {
      PrintArgs args;
      args.store = defaultStore();
      for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--text" && has_value) {
          args.text = argv[++i];
        } else if (flag == "--key" && has_value) {
          args.key = argv[++i];
        } else if (flag == "--profile" && has_value) {
          args.profile = argv[++i];
          if (args.profile == "list") {
            return listProfiles();
          }
          if (!profileExists(args.profile)) {
            std::cout << "unknown profile: " << args.profile
                      << " (try --profile list)\n\n";
            return usage();
          }
        } else if (flag == "--store" && has_value) {
          args.store = argv[++i];
        } else if (flag == "--width" && has_value) {
          args.width = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
          if (args.width == 0) {
            std::cout << "invalid width\n\n";
            return usage();
          }
        } else if (flag == "--timeout" && has_value) {
          args.timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (flag == "--no-cut") {
          args.cut = false;
        } else if (flag == "--skip-preflight") {
          args.preflight = false;
        } else {
          std::cout << "unknown option: " << flag << "\n\n";
          return usage();
        }
      }
      if (args.text.empty()) {
        std::cout << "print requires --text\n\n";
        return usage();
      }
      return runPrint(endpoint, args);
    }
  } catch (const std::exception& error) {
    std::cout << "error: " << error.what() << "\n";
    return kExitFailed;
  }

  std::cout << "unknown command: " << command << "\n\n";
  return usage();
}
