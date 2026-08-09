#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "printerdriver/capability_probe.hpp"
#include "printerdriver/device_profiles.hpp"
#include "printerdriver/discovery.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/identity.hpp"
#include "printerdriver/probe_path.hpp"
#include "printerdriver/receipt_dsl.hpp"
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

// Word wrap for the prose parts of a report. Deliberately never used for the verdict line
// of `probe-path`: `completionAuthority = PRINT_SERVER_ONLY` is the phrase
// docs/compatibility-brief.md §21 defines and the phrase a CI job greps for, so it is
// printed on one line that no wrapping rule is allowed to break in half.
void wrapped(const std::string& first_prefix, const std::string& rest_prefix,
             const std::string& text, size_t width = 70) {
  std::string line;
  bool first = true;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(' ', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string word = text.substr(start, end - start);
    if (!word.empty()) {
      if (!line.empty() && line.size() + 1 + word.size() > width) {
        std::cout << (first ? first_prefix : rest_prefix) << line << "\n";
        line.clear();
        first = false;
      }
      line += line.empty() ? word : " " + word;
    }
    if (end == text.size()) {
      break;
    }
    start = end + 1;
  }
  if (!line.empty()) {
    std::cout << (first ? first_prefix : rest_prefix) << line << "\n";
  }
}

void paragraph(const std::string& text) { wrapped("  ", "  ", text); }
void bullet(const std::string& text) { wrapped("  - ", "    ", text); }

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
      "  pdctl discover [cidr] [--port <n>] [--concurrency <n>]\n"
      "                 [--connect-timeout <ms>] [--response-timeout <ms>]\n"
      "                 [--no-probe] [--quiet]\n"
      "      Sweep a subnet for port-9100 listeners and report whether each one\n"
      "      answers on the backchannel. Default cidr is the local /24.\n"
      "      The only bytes ever written are DLE EOT 1 (10 04 01): a port-9100\n"
      "      device prints what it receives, so nothing printable is sent and no\n"
      "      printer on the subnet ejects paper. --no-probe writes nothing at all.\n"
      "\n"
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
      "  pdctl probe-path --server <host[:port]> [--expect-printer-behind]\n"
      "                   [--timeout <ms>] [--print-test-line]\n"
      "      Classify the completion authority of a path that runs THROUGH a\n"
      "      print server - a USB/Ethernet print server box, or any other 9100\n"
      "      forwarder (docs/compatibility-brief.md §19-§23). Connects to the\n"
      "      endpoint, asks DLE EOT 1 and ONE correlated GS ( H fn 48 fence, and\n"
      "      reports whether the printer's answers survive the path:\n"
      "        PHYSICAL_PRINTER   the correlated echo came back with our token\n"
      "        PRINT_SERVER_ONLY  every byte was accepted and none came back\n"
      "        TRANSPORT_ONLY     something came back, but not this host's answer\n"
      "      The last two are different findings and are never collapsed: a path\n"
      "      that answers DLE EOT but loses the correlated echo is not a path that\n"
      "      swallows responses.\n"
      "      --expect-printer-behind turns the report into a CI assertion: a\n"
      "      PRINT_SERVER_ONLY verdict then exits non-zero. Without it the command\n"
      "      always exits 0 and only reports. Writes nothing printable unless\n"
      "      --print-test-line is given. --timeout is the fence budget in ms.\n"
      "\n"
      "  pdctl drawer-probe <host[:port]> [--profile <name>] [--store <dir>]\n"
      "                     [--no-calibrate]\n"
      "      Cash drawer report that NEVER fires an output: the documented port\n"
      "      (standard, voltage, current, channels, sense pin) and the software\n"
      "      provenance, as two separate columns, followed by a non-destructive\n"
      "      switch test - close the drawer, read; open it by hand, read - which\n"
      "      records which level means OPEN and persists it. Safe on hardware\n"
      "      nobody has classified, which is the hardware it is for.\n"
      "\n"
      "  pdctl verify <token> [--store <dir>]\n"
      "      Paper to job: resolves the four-character V: code on a receipt and\n"
      "      prints that job's full journal history - states, timestamps, grade,\n"
      "      authority, method, attempt. Touches no printer and rewrites nothing.\n"
      "  pdctl render --template <file.json> [--model <file.json>]\n"
      "               [--width <dots>] [--profile <name>]\n"
      "      Bind and lay out a receipt-DSL document without touching a printer:\n"
      "      prints the render report (every degradation, declared) and a character\n"
      "      approximation of the paper. Reads files only; opens no socket.\n"
      "\n"
      "  pdctl autodetect [cidr] [--port <n>] [--concurrency <n>]\n"
      "                   [--connect-timeout <ms>] [--response-timeout <ms>]\n"
      "                   [--endpoint <host[:port]>]... [--no-probe-unknown]\n"
      "                   [--store <dir>] [--quiet]\n"
      "      docs/api.md §15. Discovery -> identify -> the PRINTLESS subset of the\n"
      "      capability probe, with the stored-findings cache respected. Prints one\n"
      "      row per candidate: ip, vendor guess, model, trusted, profile,\n"
      "      completion, grade ceiling, provenance. NOTHING PRINTS AND NOTHING\n"
      "      FIRES. Because the fences are asked out of an empty buffer, a\n"
      "      completion found here proves the command exists and not that its\n"
      "      answer waits for paper: `pdctl probe`, which prints, or a real job is\n"
      "      what promotes it. --no-probe-unknown leaves untouched devices alone\n"
      "      and reports them Unverified; --endpoint may be repeated and skips the\n"
      "      sweep entirely, which is also the only way an unreachable address is\n"
      "      reported (somebody named it).\n"
      "\n"
      "  pdctl print <host[:port]> --text \"...\" [options]\n"
      "  pdctl print <host[:port]> --template <file.json> [--model <file.json>]\n"
      "      Print a small receipt through the full engine and stream job events.\n"
      "      --template <f>              receipt-DSL document or template\n"
      "      --model <f>                 JSON parameter model bound into the template\n"
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
      "  pdctl drawer test <host[:port]> [--channel 1|2] [--pulse <ms>]\n"
      "                     [--profile <name>] [--store <dir>]\n"
      "      Energises the drawer solenoid: the cash drawer WILL open. Runs only\n"
      "      when the profile's electrical standard is known - RJ11/RJ12-looking\n"
      "      drawer connectors are not a universal electrical standard, and an\n"
      "      unclassified port is refused rather than guessed at. Reports the\n"
      "      state before the kick, the pulse, whether the switch changed and\n"
      "      how long it took, and a verdict that distinguishes OPEN_VERIFIED\n"
      "      from KICK_SENT_UNVERIFIED.\n"
      "  pdctl self-test <host[:port]> [--profile <name>] [--width <dots>]\n"
      "                     [--store <dir>] [--refresh-identity] [--no-barcode]\n"
      "      docs/api.md §15. Prints ONE diagnostic ticket through the full fenced\n"
      "      engine: identity, profile and how it was selected, media, completion\n"
      "      mechanism with its grade ceiling and provenance, the drawer\n"
      "      classification, a Czech/Hungarian/Polish charset line, a Code 128\n"
      "      sample, and the job's own verification token in the trailer QR.\n"
      "      Anything the profile cannot draw is printed as a declared degradation.\n"
      "      The ticket is the report and the terminal result is the proof: a Done\n"
      "      at grade A is the statement that the stack works end to end on this\n"
      "      unit. --refresh-identity interrogates the device first (that probe\n"
      "      prints two short test lines). The printer's OWN built-in self-test is\n"
      "      a different document: `pdctl test-print`.\n"
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
  // Both sides of docs/compatibility-brief.md §28: what the shipped database claimed
  // before anything was asked, and what the device turned out to do. Keeping the
  // defaults is the whole point — a report that only showed the promoted profile could
  // not tell "the manual says so" apart from "we measured it".
  pd::CapabilityProfile defaults;
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
  out->defaults = pd::devices::byName(out->assessment.profile_guess);
  out->profile = pd::promote(out->defaults, out->findings);

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

  // docs/compatibility-brief.md §28. Three distinct things per capability, side by
  // side, because collapsing them is the classic ESC/POS mistake: assuming that
  // recognising the print commands implies the Epson feedback extensions.
  //
  //   manufacturer doc — the shipped database default said Documented, i.e. the
  //                      vendor's own command documentation lists it for this model.
  //                      Epson is the only family here where that is ever YES.
  //   device probe     — this interrogation asked the installed hardware over the
  //                      installed interface path. NOT ASKED is a third answer and is
  //                      never rendered as NO.
  //   result           — what the promoted profile now claims, and on what basis.
  section("Capability provenance (brief §28)");
  std::cout << "  " << std::left << std::setw(kLabelWidth) << ""
            << std::setw(20) << "manufacturer doc" << std::setw(14) << "device probe"
            << "result\n";
  const auto provenanceRow = [](const char* label, pd::Provenance documented_default,
                                const std::optional<bool>& probed,
                                pd::Provenance result) {
    std::cout << "  " << std::left << std::setw(kLabelWidth) << label << std::setw(20)
              << (documented_default == pd::Provenance::Documented ? "YES" : "NO")
              << std::setw(14)
              << (probed.has_value() ? (*probed ? "YES" : "NO") : "NOT ASKED")
              << pd::to_string(result) << "\n";
  };
  provenanceRow("GS ( H fn 48", discovery.defaults.completion_caps.process_id_gs_h_provenance,
                findings.gs_h_process_id,
                profile.completion_caps.process_id_gs_h_provenance);
  provenanceRow("GS r 1", discovery.defaults.completion_caps.queued_gs_r_provenance,
                findings.gs_r1, profile.completion_caps.queued_gs_r_provenance);
  provenanceRow("DLE EOT", discovery.defaults.status.dle_eot_provenance, findings.dle_eot,
                profile.status.dle_eot_provenance);
  provenanceRow("GS a ASB", discovery.defaults.status.asb_provenance, findings.asb,
                profile.status.asb_provenance);
  provenanceRow("cutter error status", discovery.defaults.status.cutter_error_provenance,
                findings.cutter_error_status, profile.status.cutter_error_provenance);
  std::cout << "\n  documentation and probe answer different questions: documentation\n"
               "  says what the model supports, the probe says what this unit does over\n"
               "  this interface path. Either can be YES while the other is NO.\n";

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
  if (profile.languages.count() > 1) {
    // A Citizen CMP publishes ESC/POS, CPCL and ZPL2 command references for one
    // printer (§11-§12); which one this core drives is a separate fact from which
    // ones exist.
    std::string all;
    for (const pd::CommandLanguage language : pd::kAllCommandLanguages) {
      if (profile.languages.has(language)) {
        all += (all.empty() ? "" : ", ");
        all += pd::to_string(language);
      }
    }
    row("documented languages", all);
  }
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

// --- probe-path -----------------------------------------------------------------------
//
// docs/compatibility-brief.md §19-§23. Every other command here points at a printer; this
// one points at the *path* and asks who is allowed to claim a completion across it. The
// report is deliberately two-columned in the sense of §28: what this measurement
// established, and — just as prominently — what it did not, because a print-server path
// that answers one question and not the other is exactly where "the write succeeded" gets
// silently promoted to "the receipt printed".

struct PathArgs {
  Endpoint server;
  bool expect_printer_behind = false;
  bool print_test_line = false;
  uint32_t fence_timeout_ms = 0;  // 0 = the core's default budget
};

int runProbePath(const PathArgs& args) {
  pd::PathProbeOptions options;
  options.host = args.server.host;
  options.port = args.server.port;
  options.print_test_line = args.print_test_line;
  if (args.fence_timeout_ms != 0) {
    options.fence_timeout_ms = args.fence_timeout_ms;
  }

  const pd::PathProbeFindings findings = pd::probePath(options);

  std::cout << "Print-server path probe - " << args.server.host << ":" << args.server.port
            << "\n";

  section("Path");
  row("endpoint", findings.endpoint);
  row("connect", findings.connected
                     ? ("ok in " + std::to_string(findings.connect_ms) + " ms")
                     : ("FAILED - " + findings.connect_error));
  row("DLE EOT 1 written", findings.wrote_status_query ? "YES" : "NO");
  row("GS ( H fn 48 written",
      findings.wrote_fence ? ("YES, token " + findings.token) : std::string("NO"));
  if (!findings.write_error.empty()) {
    row("write error", findings.write_error);
  }
  row("closed by far side",
      findings.link_dropped
          ? ("YES - " + (findings.drop_message.empty() ? std::string("no reason given")
                                                       : findings.drop_message))
          : std::string("NO"));

  section("What came back");
  row("bytes returned", std::to_string(findings.raw.size()));
  row("DLE EOT 1 answer", findings.dle_eot_response.empty()
                              ? "NO RESPONSE"
                              : hex(findings.dle_eot_response));
  row("DLE EOT 1 recognised", yesNo(findings.dle_eot_answered));
  row("real-time wait", std::to_string(findings.status_ms) + " ms");
  row("GS ( H fn 48 echo", yesNo(findings.fence_echoed));
  row("fence wait", std::to_string(findings.fence_ms) + " ms");
  if (findings.foreign_token_echoed) {
    std::string tokens;
    for (const std::string& token : findings.foreign_tokens) {
      tokens += (tokens.empty() ? "" : ", ") + token;
    }
    row("foreign token echo", tokens);
  }
  if (!findings.unclassified.empty()) {
    row("unclassified bytes", hex(findings.unclassified));
  }
  if (!findings.raw.empty()) {
    row("raw stream", hex(findings.raw));
  }

  // The two columns of docs/compatibility-brief.md §28, spelled out as prose because on a
  // print-server path the interesting entry in the right-hand column is usually the one
  // an operator would otherwise assume belongs in the left-hand one.
  section("What this path PROVED (brief §28)");
  if (!findings.connected) {
    bullet("nothing: the path never opened, so every statement below is about an "
           "address rather than about a printer");
  } else {
    bullet("the endpoint accepts a TCP connection");
    if (findings.wrote_status_query && findings.wrote_fence) {
      bullet("it accepted every byte this probe wrote, printable or not");
    }
    if (findings.dle_eot_answered) {
      bullet("the printer's real-time status byte traverses printer -> path -> host: "
             "the backchannel of docs/techspec.md §4 is not broken here");
    }
    if (findings.fence_echoed) {
      bullet("a GS ( H fn 48 echo carrying this host's own token (" + findings.token +
             ") crossed the path, so a completion can be correlated to one receipt "
             "(brief §24, grade A)");
    }
    if (findings.foreign_token_echoed) {
      bullet("the path carries GS ( H frames in general - one arrived for a token this "
             "host never issued");
    }
    if (!findings.dle_eot_answered && !findings.fence_echoed &&
        findings.pathCarriesResponses()) {
      bullet("bytes do come back across the path, so it is not one-way - but none of "
             "them was an answer to a question this probe asked");
    }
  }

  section("What this path did NOT prove (brief §28)");
  if (!findings.fence_echoed) {
    bullet("that any completion can be attributed to a receipt: no correlated "
           "GS ( H fn 48 echo carrying token " + findings.token + " came back");
  }
  if (!findings.dle_eot_answered) {
    bullet("that printer status reaches this host at all: DLE EOT 1 went unanswered, so "
           "cover, paper and error state are invisible from here");
  }
  if (findings.responsesSwallowed()) {
    bullet("anything whatsoever about the device on the far side: it never spoke, and a "
           "silent path cannot distinguish a healthy printer from an unplugged one");
  }
  if (findings.correlationLostOnly()) {
    bullet("which of the two possible causes applies: the device may have no fn 48, or "
           "something on the path may drop the correlated echo. No probe standing at "
           "this end can separate them");
  }
  bullet(args.print_test_line
             ? std::string("that anything physically printed: one test line was sent and "
                           "nothing was cut, and no probe of any kind verifies paper")
             : std::string("that anything physically printed: this probe printed nothing "
                           "and cut nothing, by design"));
  bullet("that TCP acceptance is printer completion. A print server acknowledges the "
         "socket, never the paper (brief §21), and CUPS waiteof or a Windows PRINTED "
         "state is the same claim one layer up (brief §23)");
  if (findings.fence_echoed) {
    bullet("that the path behaves this way under load, after a reconnect, or with "
           "another host writing to the same printer: this is one measurement of one "
           "moment");
  }

  section("Classification");
  // One line, unwrapped, in the brief's own words. Everything else in this report is for
  // a human; this line is for a human AND for whatever is grepping the log.
  std::cout << "  completionAuthority = " << pd::pathAuthorityLabel(findings.authority)
            << "\n"
            << "  pd::CompletionAuthority::" << pd::to_string(findings.authority) << "\n\n";
  paragraph(findings.rationale);

  if (!args.expect_printer_behind) {
    std::cout << "\n  exit status 0: without --expect-printer-behind this command reports "
                 "a\n  finding and asserts nothing. PRINT_SERVER_ONLY is a fact about a "
                 "path,\n  not a failure of the tool that measured it.\n";
    return kExitDone;
  }

  section("Assertion (--expect-printer-behind)");
  if (findings.authority == pd::CompletionAuthority::PhysicalPrinter) {
    row("expected", "a physical printer answering across this path");
    row("result", "PASS");
    return kExitDone;
  }
  if (findings.authority == pd::CompletionAuthority::PrintServer) {
    row("expected", "a physical printer answering across this path");
    row("result", "FAIL - PRINT_SERVER_ONLY");
    std::cout << "\n  a printer was asserted behind this path and the path returned "
                 "nothing.\n  Any job sent through it can be graded no higher than "
                 "brief §24 grade E.\n";
    return kExitFailed;
  }
  row("expected", "a physical printer answering across this path");
  if (!findings.connected) {
    row("result", "FAIL - the path did not open");
    std::cout << "\n  the assertion could not be tested at all: nothing accepted a "
                 "connection\n  at this address. A broken path is a failure, not an "
                 "ambiguity.\n";
    return kExitFailed;
  }
  row("result", "UNKNOWN - not established either way");
  std::cout << "\n  unresolved rather than refuted: the path opened, but nothing that\n"
               "  came back correlates to a receipt and nothing proves the responses\n"
               "  were swallowed either. Exit 2 (unknown) is the honest answer;\n"
               "  collapsing it into pass or fail is the mistake this command exists\n"
               "  to prevent.\n";
  return kExitUnknown;
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

// --- receipt DSL (docs/receipt-dsl.md) -----------------------------------------------

// The document tier goes through the same three steps everywhere: parse the template,
// bind the model, render against the media. `render` stops after step three and prints
// what it got; `print` hands the bytes to the identical engine path a --text receipt
// uses, so a template proves the same things about the engine that a plain job does.

bool readFile(const std::string& path, std::string* out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  *out = buffer.str();
  return true;
}

void printDslReport(const pd::dsl::RenderReport& report) {
  if (report.empty()) {
    std::cout << "  nothing degraded: every block rendered as written\n";
    return;
  }
  for (const pd::dsl::ReportEntry& entry : report.entries) {
    std::cout << "  " << std::left << std::setw(22)
              << (entry.block.empty() ? "document" : entry.block) << std::setw(20)
              << pd::dsl::to_string(entry.kind) << "\n"
              << "      requested  " << entry.requested << "\n"
              << "      delivered  " << entry.delivered << " ("
              << pd::dsl::to_string(entry.path) << ")\n";
    if (!entry.detail.empty()) {
      std::cout << "      because    " << entry.detail << "\n";
    }
  }
}

// Reads the two files and runs the shared parse → bind → render pipeline
// (dsl/pipeline.hpp) — the same one pd_render_document and pd_print_document_json walk,
// so this CLI and every wrapper turn one document into the same bytes.
//
// Returns false and explains itself on stdout only for the failures that have no receipt
// on the other side: a file that cannot be read, JSON that is not JSON, a template with
// no model. Everything softer is already an entry in the outcome's render report.
bool loadAndRender(const std::string& template_path, const std::string& model_path,
                   const pd::dsl::DocumentPipelineOptions& options,
                   pd::dsl::DocumentPipelineOutcome* out) {
  std::string template_text;
  if (!readFile(template_path, &template_text)) {
    std::cout << "cannot read template: " << template_path << "\n";
    return false;
  }
  std::string model_text;
  if (!model_path.empty() && !readFile(model_path, &model_text)) {
    std::cout << "cannot read model: " << model_path << "\n";
    return false;
  }
  *out = pd::dsl::renderDocumentJson(
      template_text, model_path.empty() ? nullptr : &model_text, options);
  if (!out->ok) {
    std::cout << out->error << "\n";
    return false;
  }
  return true;
}

struct RenderArgs {
  std::string template_path;
  std::string model_path;
  std::string profile = "xp-s260m";
  uint32_t width = 0;
};

// --- print ------------------------------------------------------------------------

struct PrintArgs {
  std::string text;
  std::string template_path;
  std::string model_path;
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

// ====================================================================================
// M14 — CASH DRAWER (docs/cash-drawer.md)
// ====================================================================================
//
// Two commands, and the split between them is the whole safety model of that document.
//
//   `drawer-probe` NEVER fires an output. It prints what the profile documents about
//   the port and about the commands — two separate columns — and then runs the
//   non-destructive sensor calibration, which is an operator opening and closing the
//   drawer by hand while the tool watches the switch. Safe on hardware nobody has
//   classified, which is exactly the hardware it is for.
//
//   `drawer test` energises a solenoid, and refuses outright unless the electrical
//   standard is established. "The plug fits" is not a classification.

struct DrawerArgs {
  std::string profile = "generic";
  std::string store = defaultStore();
  uint8_t channel = 1;
  uint16_t pulse_ms = 0;  // 0 -> the profile's own default
  bool calibrate = true;
};

void printDrawerElectrical(const pd::DrawerCapabilities& drawer) {
  section("drawer port - electrical (documented)");
  row("port standard", pd::to_string(drawer.electrical.standard));
  row("drawer present", yesNo(drawer.present));
  row("voltage",
      drawer.electrical.voltage != 0
          ? std::to_string(drawer.electrical.voltage) + " V"
          : std::string("not documented"));
  row("max current",
      drawer.electrical.max_current_ma != 0
          ? std::to_string(drawer.electrical.max_current_ma) + " mA"
          : std::string("not documented"));
  row("drive channels", std::to_string(static_cast<int>(drawer.electrical.channel_count)));
  row("sensor pin",
      drawer.electrical.sensor_pin != 0
          ? std::to_string(static_cast<int>(drawer.electrical.sensor_pin))
          : std::string("not documented"));
  row("evidence", pd::to_string(drawer.evidence.electrical));
  switch (drawer.electrical.standard) {
    case pd::DrawerPortStandard::Epson24V6P6C:
      row("cable", "standard Epson-style 24 V drawer cable (sense on pin 3)");
      break;
    case pd::DrawerPortStandard::Star24V6P6C:
      row("cable", "STAR-SPECIFIC cable: +24 V on pin 3, sense on pin 6");
      break;
    case pd::DrawerPortStandard::Generic12V6P6C:
      row("cable", "12 V drawer only - a 24 V drawer is the wrong load here");
      break;
    case pd::DrawerPortStandard::Unknown:
      row("cable", "UNKNOWN - do not connect a drawer on the strength of the plug");
      break;
  }
}

void printDrawerSoftware(const pd::DrawerCapabilities& drawer) {
  section("drawer port - software (provenance)");
  row("kick method", pd::to_string(drawer.kick.method));
  row("driven by this engine", yesNo(drawer.kickable()));
  row("default pulse", std::to_string(drawer.kick.default_pulse_ms) + " ms");
  row("maximum pulse", std::to_string(drawer.kick.max_pulse_ms) + " ms");
  row("cooldown", std::to_string(drawer.kick.cooldown_ms) + " ms");
  row("may kick while printing", yesNo(drawer.kick.can_kick_during_print));
  row("status method", pd::to_string(drawer.status.method));
  row("status available", yesNo(drawer.status.available));
  row("one switch, two drives", yesNo(drawer.status.shared_between_drawers));
  row("port shared with buzzer", yesNo(drawer.port.shared_with_buzzer));
  row("evidence", pd::to_string(drawer.evidence.commands));
  row("polarity calibrated", yesNo(drawer.status.polarity.calibrated));
  if (drawer.status.polarity.calibrated) {
    row("open level",
        drawer.status.polarity.high_means_open ? "HIGH = OPEN" : "LOW = OPEN");
  }
  if (!drawer.note.empty()) {
    std::cout << "\n  note: " << drawer.note << "\n";
  }
}

void unknownStandardWarning(const std::string& profile_name) {
  std::cout
      << "\n"
      << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
      << "!!\n"
      << "!!   R E F U S E D  -  U N K N O W N  D R A W E R  P O R T\n"
      << "!!\n"
      << "!!   RJ11/RJ12-LOOKING DRAWER CONNECTORS ARE NOT A UNIVERSAL\n"
      << "!!   ELECTRICAL STANDARD.\n"
      << "!!\n"
      << "!!   Star's identical-looking 6P6C socket carries +24 V on pin 3 and\n"
      << "!!   the sense line on pin 6 - precisely where Epson puts the sense\n"
      << "!!   line and signal ground. 12 V drawer outputs exist alongside the\n"
      << "!!   common 24 V ones, and solenoid resistance has to match the drive.\n"
      << "!!\n"
      << "!!   No output will be energised on profile '" << profile_name << "'.\n"
      << "!!\n"
      << "!!   Identify the printer model, then its drawer port type, then the\n"
      << "!!   expected voltage, then the correct cable. Actuate last.\n"
      << "!!   `pdctl drawer-probe <host>` is safe on this hardware and reads\n"
      << "!!   the switch without firing anything.\n"
      << "!!\n"
      << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n";
}

// One GS r 2 round trip over the raw transport. `drawer-probe` asks the hardware
// directly rather than through the engine, because a probe's whole job is to find out
// what a profile does not already claim.
std::optional<bool> readDrawerPin(pd::Transport& transport, Listener& listener,
                                  std::chrono::milliseconds wait) {
  listener.clear();
  const pd::TransportResult written = transport.write(pd::escpos::gsDrawerStatus());
  if (!written.ok) {
    return std::nullopt;
  }
  listener.waitForBytes(1, wait);
  const std::vector<uint8_t> raw = listener.raw();
  if (raw.empty()) {
    return std::nullopt;
  }
  return pd::drawerPinHigh(raw.front());
}

// Blocks for the operator. Returns false at end of input, which is what a pipe or a CI
// job looks like: the calibration is skipped rather than guessed at.
bool waitForOperator(const std::string& instruction) {
  std::cout << "  " << instruction << " then press Enter (Ctrl-D to stop): "
            << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) {
    std::cout << "\n";
    return false;
  }
  return true;
}

int runDrawerProbe(const Endpoint& endpoint, const DrawerArgs& args) {
  const pd::CapabilityProfile profile = profileByName(args.profile);
  const pd::DrawerCapabilities& drawer = profile.drawer;

  std::cout << "drawer probe " << endpoint.host << ":" << endpoint.port
            << "  profile=" << args.profile << "\n";
  std::cout << "This command never fires a drawer output.\n";
  printDrawerElectrical(drawer);
  printDrawerSoftware(drawer);

  if (!drawer.electricalKnown()) {
    std::cout << "\n  The port is unclassified, so `pdctl drawer test` will refuse on\n"
              << "  this profile. Reading the switch below is still safe: it asks a\n"
              << "  question and energises nothing.\n";
  }

  section("sensor test - NON-DESTRUCTIVE");
  if (!args.calibrate) {
    std::cout << "  skipped (--no-calibrate)\n";
    return kExitDone;
  }

  pd::TcpConfig config;
  config.host = endpoint.host;
  config.port = endpoint.port;
  pd::TcpTransport transport(config);
  Listener listener;
  listener.attach(transport);

  const pd::TransportResult connected = transport.connect();
  if (!connected.ok) {
    std::cout << "  connect failed: " << connected.message << "\n";
    return kExitFailed;
  }
  std::cout << "  connected to " << transport.describe() << "\n";
  std::cout << "  asking GS r 2 (1D 72 02) - the drawer kick-out connector status.\n\n";

  if (!waitForOperator("Close the drawer completely,")) {
    transport.close();
    return kExitUnknown;
  }
  const std::optional<bool> shut = readDrawerPin(transport, listener, 2000ms);
  std::cout << "    closed -> "
            << (shut.has_value() ? (*shut ? "status=1 (HIGH)" : "status=0 (LOW)")
                                 : std::string("no answer"))
            << "\n";
  if (!shut.has_value()) {
    std::cout << "\n  x  no switch answer on this path.\n"
              << "     Either the drawer port has no sense line wired, or this "
                 "interface\n"
              << "     path does not return the response. A pulse over this path can "
                 "only\n"
              << "     ever report KickSentUnverified, never OpenVerified.\n";
    transport.close();
    return kExitUnknown;
  }

  if (!waitForOperator("Now open the drawer by hand,")) {
    transport.close();
    return kExitUnknown;
  }
  const std::optional<bool> open = readDrawerPin(transport, listener, 2000ms);
  std::cout << "    open   -> "
            << (open.has_value() ? (*open ? "status=1 (HIGH)" : "status=0 (LOW)")
                                 : std::string("no answer"))
            << "\n";
  transport.close();

  if (!open.has_value()) {
    std::cout << "\n  x  the switch answered once and then stopped answering.\n";
    return kExitUnknown;
  }
  if (*open == *shut) {
    std::cout << "\n  x  the level did not change between closed and open.\n"
              << "     The sense line is not wired to this drawer's microswitch, or "
                 "the\n"
              << "     drawer was not actually moved. No polarity recorded - a "
                 "guessed\n"
              << "     one is worse than none.\n";
    return kExitFailed;
  }

  std::cout << "\n  ok  switch detected\n"
            << "  ok  " << (*open ? "HIGH = OPEN" : "LOW = OPEN") << "\n";

  // Persisted under the same printer id `pdctl print` and pd-agent use for this
  // endpoint, so the calibration is in force the next time the engine talks to it.
  const std::string printer_id = endpoint.host + ":" + std::to_string(endpoint.port);
  pd::DrawerPolarityStore polarities(args.store);
  if (!polarities.persistent()) {
    std::cout << "  !   could not open " << args.store
              << " - the polarity applies to this run only\n";
    return kExitUnknown;
  }
  polarities.save(printer_id, *open);
  std::cout << "  ok  polarity persisted for " << printer_id << " in "
            << polarities.path() << "\n";
  return kExitDone;
}

int runDrawerTest(const Endpoint& endpoint, const DrawerArgs& args) {
  const pd::CapabilityProfile profile = profileByName(args.profile);
  const pd::DrawerCapabilities& drawer = profile.drawer;

  std::cout << "drawer test " << endpoint.host << ":" << endpoint.port
            << "  profile=" << args.profile << "\n";
  printDrawerElectrical(drawer);

  if (!drawer.electricalKnown()) {
    unknownStandardWarning(args.profile);
    return kExitFailed;
  }
  if (!drawer.kickable()) {
    std::cout << "\n  refused: this profile's kick method is "
              << pd::to_string(drawer.kick.method)
              << ", which this ESC/POS engine does not drive.\n"
              << "  The port is classified and the command path is somebody else's "
                 "API;\n"
              << "  driving it from here would mean guessing at a protocol.\n";
    return kExitFailed;
  }

  warningBanner("ESC p " + std::to_string(args.channel == 2 ? 1 : 0) + " (1B 70 ...)",
                "energises the drawer solenoid: the cash drawer WILL open");

  pd::PrinterDriver driver(pd::StorageConfig::at(args.store));
  pd::PrinterConfig config;
  config.id = endpoint.host + ":" + std::to_string(endpoint.port);
  config.transport = pd::tcp(endpoint.host, endpoint.port, 3000);
  config.profile = profile;
  std::shared_ptr<pd::Printer> printer = driver.addPrinter(config);
  if (!printer) {
    std::cout << "could not attach printer\n";
    return kExitFailed;
  }
  // A polarity recorded by `drawer-probe` for this endpoint is picked up on attach.
  printDrawerSoftware(printer->drawerCapabilities());

  section("drawer test");
  pd::DrawerRequest request;
  request.channel = args.channel;
  request.pulse_ms = args.pulse_ms;
  const pd::DrawerOpenResult result = printer->openDrawer(request);

  row("before kick", pd::to_string(result.previous_state));
  row("channel", std::to_string(static_cast<int>(result.channel)));
  row("pulse",
      result.pulse_ms != 0 ? std::to_string(result.pulse_ms) + " ms"
                           : std::string("none sent"));
  switch (result.state) {
    case pd::DrawerState::OpenVerified:
      row("sensor", "changed " + std::to_string(result.elapsed_ms) + " ms after kick");
      std::cout << "\n  ok  OPEN_VERIFIED - the microswitch was seen changing\n";
      return kExitDone;
    case pd::DrawerState::Open:
      std::cout << "\n  ok  already OPEN before the kick - nothing was pulsed\n";
      return kExitDone;
    case pd::DrawerState::FailedToOpen:
      row("sensor", "unchanged after " + std::to_string(result.elapsed_ms) + " ms");
      std::cout << "\n  x   FAILED_TO_OPEN (locked / wrong channel / cable / jam)\n";
      return kExitFailed;
    case pd::DrawerState::KickSentUnverified:
      row("sensor", result.previous_state == pd::DrawerState::NoSensor
                        ? "no switch on this port"
                        : "no usable answer on this path");
      std::cout << "\n  ?   KICK_SENT_UNVERIFIED - the pulse went out and nothing "
                   "here\n"
                << "      can confirm the drawer moved. That is the honest answer, "
                   "not a\n"
                << "      weaker success: through a print server the kick travels "
                   "forward\n"
                << "      while the sensor response never comes back.\n";
      return kExitUnknown;
    case pd::DrawerState::Closed:
    case pd::DrawerState::Opening:
    case pd::DrawerState::NoSensor:
    case pd::DrawerState::Unknown:
      break;
  }
  std::cout << "\n  ?   " << pd::to_string(result.state)
            << " - nothing was established; see the report above\n";
  return kExitUnknown;
}

// ================================ end M14 ==========================================

// --- render (no printer involved) -----------------------------------------------------

void printCutAndMargins(const pd::dsl::RenderOutput& output) {
  // docs/receipt-dsl.md: the document declares cut and margins, the caller applies them.
  // The renderer returns them; it never emits a trailing cut of its own.
  row("document cut", output.requested_cut.has_value()
                          ? pd::dsl::to_string(*output.requested_cut)
                          : "not stated (caller and profile decide)");
  row("top margin", output.requested_margins.top_dots.has_value()
                        ? std::to_string(*output.requested_margins.top_dots) + " dots"
                        : "not stated");
  row("bottom margin", output.requested_margins.bottom_dots.has_value()
                           ? std::to_string(*output.requested_margins.bottom_dots) + " dots"
                           : "not stated");
}

int runRender(const RenderArgs& args) {
  const pd::CapabilityProfile profile = profileByName(args.profile);

  pd::dsl::DocumentPipelineOptions options;
  options.render.profile = pd::dsl::RenderProfile::from(profile, args.width);
  pd::dsl::DocumentPipelineOutcome outcome;
  if (!loadAndRender(args.template_path, args.model_path, options, &outcome)) {
    return kExitFailed;
  }
  const pd::dsl::RenderOutput& output = outcome.output;
  // The preview runs the same layout the bytes came from — a preview with its own layout
  // would eventually disagree with the paper, which is the one thing it must never do.
  const pd::dsl::TextPreview preview =
      pd::dsl::renderText(outcome.document, options.render);

  const pd::dsl::ResolvedStyle plain;
  const uint32_t columns = options.render.profile.charsPerLine(plain);

  std::cout << "Receipt render - " << args.template_path << "\n";
  section("Media");
  row("profile", profile.name);
  row("printable width", std::to_string(options.render.profile.width_dots) + " dots");
  row("characters per line", std::to_string(columns) + " (font A)");
  row("code page", std::to_string(static_cast<int>(options.render.profile.code_page)));
  row("escpos bytes", std::to_string(output.bytes().size()));
  printCutAndMargins(output);

  section("Render report");
  printDslReport(outcome.report);

  section("Text approximation");
  // A line in font B or at a width multiplier is laid out against its own column count,
  // so the box is drawn at the widest line rather than clipping anything away.
  size_t box = columns;
  for (const std::string& line : preview.lines) {
    box = std::max(box, pd::dsl::text::width(line));
  }
  std::cout << "  +" << std::string(box, '-') << "+\n";
  for (const std::string& line : preview.lines) {
    std::cout << "  |" << pd::dsl::text::pad(line, box, pd::dsl::Align::Left) << "|\n";
  }
  std::cout << "  +" << std::string(box, '-') << "+\n";
  std::cout << "\nthis is an approximation: character cells only. Scaled and font-B lines\n"
               "carry their own column count (" << columns
            << " is font A at this width), and QR,\n"
               "images and barcodes appear as markers. The byte count above is what a\n"
               "printer would actually receive.\n";
  return kExitDone;
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

  pd::JobOptions options;
  options.key = args.key;
  options.cut = args.cut ? pd::CutSetting::Profile : pd::CutSetting::None;
  options.preflight = args.preflight ? pd::PreflightMode::Strict : pd::PreflightMode::Skip;
  options.timeout_ms = args.timeout_ms;

  pd::Payload payload = pd::Payload::raw({});
  if (!args.template_path.empty()) {
    pd::dsl::DocumentPipelineOptions render_options;
    render_options.render.profile = pd::dsl::RenderProfile::from(profile, args.width);
    pd::dsl::DocumentPipelineOutcome outcome;
    if (!loadAndRender(args.template_path, args.model_path, render_options, &outcome)) {
      return kExitFailed;
    }

    section("Render report");
    printDslReport(outcome.report);
    section("Document meta");
    printCutAndMargins(outcome.output);

    // The three-level precedence of docs/receipt-dsl.md, applied once in the shared
    // pipeline: --no-cut has already put CutSetting::None in `options` and is left alone,
    // and a cut still at Profile — "the caller said nothing" — is filled by the document.
    // The margins ride on JobOptions, so the engine feeds
    // max(blade clearance, bottom margin) and no document can clip its own trailer.
    pd::dsl::applyDocumentMeta(outcome.output, &options);
    payload = pd::Payload::document(outcome.output.ops);
    std::cout << "\n";
  } else {
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
    payload = pd::Payload::document(receipt);
  }

  auto job = printer->print(std::move(payload), options);
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

// --- verify -------------------------------------------------------------------------

std::string wallClock(uint64_t unix_ms) {
  if (unix_ms == 0) {
    return "-";
  }
  const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000u);
  std::tm parts{};
  if (::localtime_r(&seconds, &parts) == nullptr) {
    return std::to_string(unix_ms);
  }
  char buffer[32];
  const size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  if (written == 0) {
    return std::to_string(unix_ms);
  }
  std::string out(buffer, written);
  out += ".";
  const std::string millis = std::to_string(unix_ms % 1000u);
  out += std::string(3 - millis.size(), '0') + millis;
  return out;
}

// Paper -> job (docs/api.md §14). Deliberately reads the journal rather than opening a
// JobStore: opening one compacts it to a single state line per job, which would destroy
// the very history this command exists to print.
int runVerify(const std::string& token, const std::string& store) {
  const std::string path = store + "/jobs.journal";
  const std::vector<pd::JournalEntry> entries = pd::readJournal(path);
  if (entries.empty()) {
    std::cout << "no journal at " << path << "\n";
    return kExitFailed;
  }

  // Most recent first: a sequence wraps, and the newest holder of a token is the one
  // the receipt in somebody's hand belongs to.
  std::string job_id;
  pd::JobRecord created;
  std::unordered_map<std::string, pd::JobRecord> creation;
  std::unordered_map<std::string, std::pair<std::string, std::string>> tokens;
  for (const pd::JournalEntry& entry : entries) {
    if (entry.kind == pd::JournalEntry::Kind::Job) {
      creation[entry.record.id] = entry.record;
      tokens[entry.record.id] = {entry.record.print_token, entry.record.cut_token};
    } else if (entry.kind == pd::JournalEntry::Kind::Tokens) {
      tokens[entry.record.id] = {entry.record.print_token, entry.record.cut_token};
    }
  }
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->kind != pd::JournalEntry::Kind::Job) {
      continue;
    }
    const auto found = tokens.find(it->record.id);
    if (found == tokens.end()) {
      continue;
    }
    if (found->second.first == token || found->second.second == token) {
      job_id = it->record.id;
      created = it->record;
      created.print_token = found->second.first;
      created.cut_token = found->second.second;
      break;
    }
  }
  if (job_id.empty()) {
    std::cout << "no job in " << path << " carried the verification token \"" << token
              << "\"\n\n"
                 "a token is four printable characters: two naming the driver instance\n"
                 "that printed the receipt, two naming the job. A token whose first two\n"
                 "characters are not this store's instance nonce belongs to another\n"
                 "instance's journal.\n";
    return kExitFailed;
  }

  std::cout << "Receipt verification - " << token << "\n";
  section("Job");
  row("token queried", token);
  row("print token", created.print_token.empty() ? "none" : created.print_token);
  row("cut token", created.cut_token.empty() ? "none" : created.cut_token);
  row("job id", created.id);
  row("idempotency key", created.key);
  row("printer", created.printer_id);
  row("attempt", std::to_string(created.attempt));
  row("payload", std::string(pd::to_string(created.payload_kind)) + ", " +
                     std::to_string(created.payload_bytes) + " bytes in");
  row("created", wallClock(created.created_unix_ms));

  section("History");
  std::cout << "  " << std::left << std::setw(24) << "when" << std::setw(22) << "state"
            << std::setw(20) << "confidence" << "evidence\n";
  size_t transitions = 0;
  for (const pd::JournalEntry& entry : entries) {
    if (entry.kind != pd::JournalEntry::Kind::State || entry.record.id != job_id) {
      continue;
    }
    ++transitions;
    std::cout << "  " << std::left << std::setw(24)
              << wallClock(entry.record.updated_unix_ms) << std::setw(22)
              << pd::to_string(entry.record.state) << std::setw(20)
              << pd::to_string(entry.record.confidence)
              << pd::gradeLetter(entry.record.grade) << " ("
              << pd::to_string(entry.record.grade) << ") / "
              << pd::to_string(entry.record.authority) << " / " << entry.record.method;
    if (entry.record.reason != pd::FailureReason::None) {
      std::cout << " / " << pd::to_string(entry.record.reason);
    }
    std::cout << "\n";
  }
  if (transitions == 0) {
    std::cout << "  (none: the journal was compacted after this job's last transition)\n";
    return kExitUnknown;
  }

  // The question an operator holding two receipts is actually asking. Each attempt is
  // its own journal record under the same key, so they are listed rather than summed.
  std::vector<pd::JobRecord> siblings;
  for (const pd::JournalEntry& entry : entries) {
    if (entry.kind == pd::JournalEntry::Kind::Job && entry.record.key == created.key &&
        entry.record.id != job_id) {
      pd::JobRecord sibling = entry.record;
      const auto found = tokens.find(sibling.id);
      if (found != tokens.end()) {
        sibling.print_token = found->second.first;
      }
      siblings.push_back(sibling);
    }
  }
  if (!siblings.empty()) {
    section("Other attempts of this key");
    for (const pd::JobRecord& sibling : siblings) {
      std::cout << "  attempt " << sibling.attempt << "  "
                << wallClock(sibling.created_unix_ms) << "  "
                << (sibling.print_token.empty() ? "no token" : sibling.print_token)
                << "  " << sibling.id << "\n";
    }
    std::cout << "\neach attempt is its own record: this key reached paper more than\n"
                 "once, so the receipt in hand is the one whose V: code is above\n";
  }
  std::cout << "\nholding a receipt whose V: code matches a journaled PrintConfirmed\n"
               "token is end-to-end evidence: this paper is the output of that job,\n"
               "and the printer acknowledged finishing it\n";
  return kExitDone;
}

// --- discover ---------------------------------------------------------------------

struct DiscoverArgs {
  std::string cidr;  // empty → the local /24
  pd::DiscoveryOptions options;
  bool quiet = false;  // suppress the per-address progress line
};

int runDiscover(const DiscoverArgs& args) {
  pd::Subnet subnet;
  if (args.cidr.empty()) {
    const std::optional<pd::Subnet> local = pd::localSubnet();
    if (!local.has_value()) {
      std::cout << "could not determine a local IPv4 subnet; pass a CIDR, e.g.\n"
                   "  pdctl discover 192.168.1.0/24\n";
      return kExitFailed;
    }
    subnet = *local;
  } else if (!pd::parseCidr(args.cidr, &subnet)) {
    std::cout << "not a CIDR block: " << args.cidr << "\n";
    return kExitUsage;
  }

  std::cout << "scanning " << subnet.toString() << " port " << args.options.port << " ("
            << subnet.hostCount() << " addresses, " << args.options.concurrency
            << " at a time)\n"
               "probe: DLE EOT 1 (10 04 01) and nothing else - no printable byte is\n"
               "ever written, so this scan cannot make a printer eject paper\n\n";

  std::mutex output;
  pd::DiscoveryCallbacks callbacks;
  if (!args.quiet) {
    callbacks.on_progress = [&](const pd::DiscoveryProgress& progress) {
      if (!progress.device.port9100_open) {
        return;
      }
      std::lock_guard<std::mutex> lock(output);
      std::cout << "  found " << progress.device.ip << " (" << progress.completed << "/"
                << progress.total << ")\n";
    };
  }

  const std::vector<pd::DiscoveredDevice> devices =
      pd::discover(subnet, args.options, callbacks);

  std::cout << "\n"
            << std::left << std::setw(18) << "ADDRESS" << std::setw(8) << "PORT"
            << std::setw(10) << "STATE" << std::setw(14) << "BACKCHANNEL"
            << "DLE EOT 1\n"
            << "----------------------------------------------------------------------\n";
  for (const pd::DiscoveredDevice& device : devices) {
    std::cout << std::left << std::setw(18) << device.ip << std::setw(8) << device.port
              << std::setw(10) << "open" << std::setw(14)
              << (device.answered() ? "answers" : "silent")
              << (device.answered() ? device.responseHex() : std::string("-")) << "\n";
  }
  if (devices.empty()) {
    std::cout << "  (nothing listening on port " << args.options.port << ")\n";
  }
  std::cout << "\n" << devices.size() << " open, " << subnet.hostCount()
            << " scanned\n\n"
               "an open port is a device, not a printer, and a silent one may still\n"
               "print perfectly - its LAN module just does not forward status bytes\n"
               "(docs/techspec.md §4). Run `pdctl probe <address>` to find out what a\n"
               "listener actually is; that one costs paper, so point it deliberately.\n";
  return devices.empty() ? kExitFailed : kExitDone;
}

// ====================================================================================
// M15 — SELF-TEST AND AUTO-DETECTION (docs/api.md §15)
// ====================================================================================

struct SelfTestArgs {
  std::string profile = "xp-s260m";
  std::string store;
  uint32_t width = pd::escpos::kWidth80mm;
  bool refresh_identity = false;
  bool barcode = true;
};

int runSelfTest(const Endpoint& endpoint, const SelfTestArgs& args) {
  const pd::CapabilityProfile profile = profileByName(args.profile);

  pd::PrinterDriver driver(pd::StorageConfig::at(args.store));
  pd::PrinterConfig config;
  config.id = endpoint.host + ":" + std::to_string(endpoint.port);
  config.transport = pd::tcp(endpoint.host, endpoint.port, 3000);
  config.width_dots = args.width;
  config.profile = profile;
  auto printer = driver.addPrinter(config);

  std::cout << "self-test " << config.id << "  profile=" << profile.name << "\n"
            << "one diagnostic ticket, through the ordinary fenced engine, under an\n"
               "ordinary idempotency key. THIS USES PAPER: the paper is the report.\n";

  pd::SelfTestOptions options;
  options.refresh_identity = args.refresh_identity;
  options.barcode = args.barcode;
  const pd::SelfTestResult result = printer->selfTest(options);

  section("Ticket (as laid out)");
  for (const std::string& line : result.ticket_lines) {
    std::cout << "  |" << line << "|\n";
  }

  const pd::DetectionSummary& detection = result.detection;
  section("Detection");
  row("endpoint", detection.endpoint);
  row("vendor", detection.identity.vendor);
  row("model", detection.identity.model.empty() ? "not reported"
                                                : detection.identity.model);
  row("identity trusted", yesNo(detection.identity.trusted));
  row("confidence",
      std::to_string(static_cast<int>(detection.identity.confidence_percent)) + "%");
  row("identity from this run", yesNo(detection.identity_fresh));
  row("profile", detection.profile_id);
  row("selected by", pd::to_string(detection.selection));
  row("media", std::to_string(detection.printable_width_dots) + " dots, " +
                   std::to_string(detection.chars_per_line) + " cols, " +
                   std::to_string(detection.dpi) + " dpi");
  row("completion", std::string(pd::to_string(detection.completion)) + " (" +
                        detection.method + ")");
  row("grade ceiling", std::string(pd::gradeLetter(detection.grade_ceiling)) + " (" +
                           pd::to_string(detection.grade_ceiling) + ")");
  row("completion provenance", pd::to_string(detection.completion_provenance));
  row("drawer", detection.drawer_present
                    ? (std::string(pd::to_string(detection.drawer_standard)) +
                       (detection.drawer_kickable ? ", kickable" : ", NOT kickable"))
                    : std::string("no drawer port on this profile"));
  row("provenance", detection.provenanceSummary());

  if (!detection.degradations.empty()) {
    section("Declared degradations (printed on the ticket)");
    for (const std::string& line : detection.degradations) {
      bullet(line);
    }
  }

  section("Result");
  row("key", result.key);
  row("verification id", result.print_token.empty()
                             ? "none - this profile has no GS ( H wire token"
                             : result.print_token);
  row("outcome", pd::to_string(result.result.outcome));
  row("evidence", pd::to_string(result.result.confidence));
  row("grade", std::string(pd::gradeLetter(result.result.grade)) + " (" +
                   pd::to_string(result.result.grade) + ")");
  row("authority", pd::to_string(result.result.authority));
  row("method", result.result.method);
  if (result.result.reason != pd::FailureReason::None) {
    row("reason", pd::to_string(result.result.reason));
  }

  std::cout << "\n";
  switch (result.result.outcome) {
    case pd::JobOutcome::Done:
      paragraph("Done. The ticket in your hand is the output of the ordinary path on "
                "this unit, and the grade above is what the hardware proved, not what "
                "the profile hoped.");
      return kExitDone;
    case pd::JobOutcome::Failed:
      return kExitFailed;
    case pd::JobOutcome::Unknown:
      paragraph("UNKNOWN, not failed: bytes were sent and the ticket may have printed. "
                "Look at the paper.");
      return kExitUnknown;
  }
  return kExitUnknown;
}

struct AutoDetectArgs {
  pd::AutoDetectOptions options;
  std::string store;
  bool quiet = false;
};

int runAutoDetect(const AutoDetectArgs& args) {
  pd::PrinterDriver driver(args.store.empty() ? pd::StorageConfig::inMemory()
                                              : pd::StorageConfig::at(args.store));

  std::cout << "auto-detect "
            << (args.options.subnet_cidr.empty() ? std::string("(local /24)")
                                                 : args.options.subnet_cidr)
            << " port " << args.options.port << "\n"
               "discovery (DLE EOT 1) -> identify -> the PRINTLESS probe subset.\n"
               "Nothing prints and nothing fires: the full probe's test lines are\n"
               "skipped, so a fence found here proves the command exists and NOT that\n"
               "its answer waits for paper. Run `pdctl probe <address>`, which costs\n"
               "paper, or a real job to promote it.\n\n";

  std::mutex output;
  std::vector<pd::DetectedPrinter> found;
  try {
    found = driver.autoDetect(
        args.options, [&](const pd::DetectedPrinter& one, uint64_t done, uint64_t total) {
          if (args.quiet) {
            return;
          }
          std::lock_guard<std::mutex> lock(output);
          std::cout << "  " << std::left << std::setw(24) << one.endpoint
                    << pd::to_string(one.status) << "  (" << done << "/" << total
                    << ")\n";
        });
  } catch (const pd::DiscoveryError& error) {
    std::cout << error.what() << "\n";
    return kExitUsage;
  }

  std::cout << "\n"
            << std::left << std::setw(22) << "IP" << std::setw(12) << "VENDOR"
            << std::setw(14) << "MODEL" << std::setw(9) << "TRUSTED" << std::setw(20)
            << "PROFILE" << std::setw(16) << "COMPLETION" << std::setw(8) << "CEILING"
            << "PROVENANCE\n"
            << "-------------------------------------------------------------------"
               "-----------------------------------------------\n";
  size_t reachable = 0;
  for (const pd::DetectedPrinter& one : found) {
    const pd::DetectionSummary& summary = one.summary;
    const bool live = one.status != pd::DetectionStatus::Unreachable;
    reachable += live ? 1 : 0;
    std::cout << std::left << std::setw(22) << one.endpoint << std::setw(12)
              << (live ? summary.identity.vendor : std::string("-")) << std::setw(14)
              << (summary.identity.model.empty() ? std::string("-")
                                                 : summary.identity.model)
              << std::setw(9)
              << (live ? yesNo(summary.identity.trusted) : std::string("-"))
              << std::setw(20)
              << (summary.profile_id.empty() ? std::string("-") : summary.profile_id)
              << std::setw(16)
              << (live ? pd::to_string(summary.completion) : "-") << std::setw(8)
              << (live ? pd::gradeLetter(summary.grade_ceiling) : "-")
              << (live ? summary.provenanceSummary() : pd::to_string(one.status)) << "\n";
  }
  if (found.empty()) {
    std::cout << "  (nothing listening)\n";
  }

  for (const pd::DetectedPrinter& one : found) {
    if (one.summary.degradations.empty()) {
      continue;
    }
    std::cout << "\n" << one.endpoint << ":\n";
    for (const std::string& line : one.summary.degradations) {
      bullet(line);
    }
  }

  std::cout << "\n" << found.size() << " candidate(s), " << reachable << " reachable\n";
  return reachable == 0 ? kExitFailed : kExitDone;
}

// ============================== end M15 =============================================

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
  // M14. `drawer test <host>` puts the host in argv[3], so it cannot go through the
  // endpoint dispatch below, which reads argv[2]. Same shape as `print list`.
  if (argc >= 3 && std::string(argv[1]) == "drawer") {
    if (std::string(argv[2]) != "test" || argc < 4) {
      std::cout << "usage: pdctl drawer test <host[:port]> [options]\n\n";
      return usage();
    }
    Endpoint drawer_endpoint;
    if (!parseEndpoint(argv[3], &drawer_endpoint)) {
      std::cout << "invalid host: " << argv[3] << "\n\n";
      return usage();
    }
    DrawerArgs args;
    for (int i = 4; i < argc; ++i) {
      const std::string flag = argv[i];
      const bool has_value = i + 1 < argc;
      if (flag == "--channel" && has_value) {
        const unsigned long value = std::strtoul(argv[++i], nullptr, 10);
        if (value != 1 && value != 2) {
          std::cout << "--channel must be 1 (drive 1) or 2 (drive 2)\n\n";
          return usage();
        }
        args.channel = static_cast<uint8_t>(value);
      } else if (flag == "--pulse" && has_value) {
        const unsigned long value = std::strtoul(argv[++i], nullptr, 10);
        if (value == 0 || value > 5000) {
          std::cout << "--pulse must be 1..5000 ms\n\n";
          return usage();
        }
        args.pulse_ms = static_cast<uint16_t>(value);
      } else if (flag == "--profile" && has_value) {
        args.profile = argv[++i];
        if (!profileExists(args.profile)) {
          std::cout << "unknown profile: " << args.profile << "\n\n";
          return usage();
        }
      } else if (flag == "--store" && has_value) {
        args.store = argv[++i];
      } else {
        std::cout << "unknown option: " << flag << "\n\n";
        return usage();
      }
    }
    try {
      return runDrawerTest(drawer_endpoint, args);
    } catch (const std::exception& error) {
      std::cout << "error: " << error.what() << "\n";
      return kExitFailed;
    }
  }
  if (argc >= 3 && std::string(argv[1]) == "verify") {
    // Not an endpoint command: it answers from the journal on disk, so it works with
    // the printer unplugged, which is exactly when somebody is holding the receipt.
    std::string store = defaultStore();
    for (int i = 3; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--store" && i + 1 < argc) {
        store = argv[++i];
      } else {
        std::cout << "unknown option: " << flag << "\n\n";
        return usage();
      }
    }
    return runVerify(argv[2], store);
  }

  // `discover` takes a subnet rather than an endpoint, and the subnet is optional, so
  // it is dispatched before argv[2] is read as a host.
  if (argc >= 2 && std::string(argv[1]) == "discover") {
    DiscoverArgs args;
    for (int i = 2; i < argc; ++i) {
      const std::string flag = argv[i];
      const bool has_value = i + 1 < argc;
      if (flag == "--port" && has_value) {
        const long value = std::strtol(argv[++i], nullptr, 10);
        if (value <= 0 || value > 65535) {
          std::cout << "invalid port\n\n";
          return kExitUsage;
        }
        args.options.port = static_cast<uint16_t>(value);
      } else if (flag == "--concurrency" && has_value) {
        args.options.concurrency =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (args.options.concurrency == 0) {
          std::cout << "invalid concurrency\n\n";
          return kExitUsage;
        }
      } else if (flag == "--connect-timeout" && has_value) {
        args.options.connect_timeout_ms =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (flag == "--response-timeout" && has_value) {
        args.options.response_timeout_ms =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (flag == "--no-probe") {
        args.options.probe_backchannel = false;
      } else if (flag == "--quiet") {
        args.quiet = true;
      } else if (!flag.empty() && flag[0] != '-' && args.cidr.empty()) {
        args.cidr = flag;
      } else {
        std::cout << "unknown option: " << flag << "\n\n";
        return usage();
      }
    }
    try {
      return runDiscover(args);
    } catch (const std::exception& error) {
      std::cout << "error: " << error.what() << "\n";
      return kExitFailed;
    }
  }

  // M15 — docs/api.md §15. Like `discover`, it takes a subnet rather than a printer, so
  // it is dispatched before argv[2] is read as a hostname.
  if (argc >= 2 && std::string(argv[1]) == "autodetect") {
    AutoDetectArgs args;
    for (int i = 2; i < argc; ++i) {
      const std::string flag = argv[i];
      const bool has_value = i + 1 < argc;
      if (flag == "--port" && has_value) {
        const long value = std::strtol(argv[++i], nullptr, 10);
        if (value <= 0 || value > 65535) {
          std::cout << "invalid port\n\n";
          return kExitUsage;
        }
        args.options.port = static_cast<uint16_t>(value);
      } else if (flag == "--concurrency" && has_value) {
        args.options.concurrency =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (args.options.concurrency == 0) {
          std::cout << "invalid concurrency\n\n";
          return kExitUsage;
        }
      } else if (flag == "--connect-timeout" && has_value) {
        args.options.connect_timeout_ms =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (flag == "--response-timeout" && has_value) {
        args.options.response_timeout_ms =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (flag == "--endpoint" && has_value) {
        args.options.endpoints.emplace_back(argv[++i]);
      } else if (flag == "--no-probe-unknown") {
        args.options.probe_unknown = false;
      } else if (flag == "--store" && has_value) {
        args.store = argv[++i];
      } else if (flag == "--quiet") {
        args.quiet = true;
      } else if (!flag.empty() && flag[0] != '-' && args.options.subnet_cidr.empty()) {
        args.options.subnet_cidr = flag;
      } else {
        std::cout << "unknown option: " << flag << "\n\n";
        return usage();
      }
    }
    try {
      return runAutoDetect(args);
    } catch (const std::exception& error) {
      std::cout << "error: " << error.what() << "\n";
      return kExitFailed;
    }
  }

  // `render` takes no printer, so it is dispatched before the endpoint is parsed.
  if (argc >= 2 && std::string(argv[1]) == "render") {
    RenderArgs args;
    for (int i = 2; i < argc; ++i) {
      const std::string flag = argv[i];
      const bool has_value = i + 1 < argc;
      if (flag == "--template" && has_value) {
        args.template_path = argv[++i];
      } else if (flag == "--model" && has_value) {
        args.model_path = argv[++i];
      } else if (flag == "--profile" && has_value) {
        args.profile = argv[++i];
        if (!profileExists(args.profile)) {
          std::cout << "unknown profile: " << args.profile << " (try --profile list)\n\n";
          return kExitUsage;
        }
      } else if (flag == "--width" && has_value) {
        args.width = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (args.width == 0) {
          std::cout << "invalid width\n\n";
          return kExitUsage;
        }
      } else {
        std::cout << "unknown option: " << flag << "\n\n";
        return usage();
      }
    }
    if (args.template_path.empty()) {
      std::cout << "render requires --template\n\n";
      return usage();
    }
    try {
      return runRender(args);
    } catch (const std::exception& error) {
      std::cout << "error: " << error.what() << "\n";
      return kExitFailed;
    }
  }
  // `probe-path` names its endpoint with --server rather than positionally, because the
  // thing being measured is the path and not the device at the end of it. That also keeps
  // it out of the endpoint dispatch below, where argv[2] would otherwise be read as a
  // hostname called "--server".
  if (argc >= 2 && std::string(argv[1]) == "probe-path") {
    PathArgs args;
    bool have_server = false;
    for (int i = 2; i < argc; ++i) {
      const std::string flag = argv[i];
      const bool has_value = i + 1 < argc;
      if (flag == "--server" && has_value) {
        if (!parseEndpoint(argv[++i], &args.server)) {
          std::cout << "invalid server: " << argv[i] << "\n\n";
          return kExitUsage;
        }
        have_server = true;
      } else if (flag == "--expect-printer-behind") {
        args.expect_printer_behind = true;
      } else if (flag == "--print-test-line") {
        args.print_test_line = true;
      } else if (flag == "--timeout" && has_value) {
        args.fence_timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        if (args.fence_timeout_ms == 0) {
          std::cout << "invalid timeout\n\n";
          return kExitUsage;
        }
      } else {
        std::cout << "unknown option: " << flag << "\n\n";
        return usage();
      }
    }
    if (!have_server) {
      std::cout << "probe-path requires --server <host[:port]>\n\n";
      return usage();
    }
    try {
      return runProbePath(args);
    } catch (const std::exception& error) {
      std::cout << "error: " << error.what() << "\n";
      return kExitFailed;
    }
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
    // M14 — docs/cash-drawer.md. Read-only: it energises nothing, whatever the port
    // turns out to be.
    if (command == "drawer-probe") {
      DrawerArgs args;
      for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--profile" && has_value) {
          args.profile = argv[++i];
          if (!profileExists(args.profile)) {
            std::cout << "unknown profile: " << args.profile << "\n\n";
            return usage();
          }
        } else if (flag == "--store" && has_value) {
          args.store = argv[++i];
        } else if (flag == "--no-calibrate") {
          args.calibrate = false;
        } else {
          std::cout << "unknown option: " << flag << "\n\n";
          return usage();
        }
      }
      return runDrawerProbe(endpoint, args);
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
    // M15 — docs/api.md §15. Consumes paper, so it sits with the operator commands
    // even though everything it does goes through the ordinary job path.
    if (command == "self-test") {
      SelfTestArgs args;
      args.store = defaultStore();
      for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--profile" && has_value) {
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
        } else if (flag == "--refresh-identity") {
          args.refresh_identity = true;
        } else if (flag == "--no-barcode") {
          args.barcode = false;
        } else {
          std::cout << "unknown option: " << flag << "\n\n";
          return usage();
        }
      }
      return runSelfTest(endpoint, args);
    }
    if (command == "print") {
      PrintArgs args;
      args.store = defaultStore();
      for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--text" && has_value) {
          args.text = argv[++i];
        } else if (flag == "--template" && has_value) {
          args.template_path = argv[++i];
        } else if (flag == "--model" && has_value) {
          args.model_path = argv[++i];
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
      if (args.text.empty() && args.template_path.empty()) {
        std::cout << "print requires --text or --template\n\n";
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
