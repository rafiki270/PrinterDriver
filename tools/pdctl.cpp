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

#include "printerdriver/driver.hpp"
#include "printerdriver/response_parser.hpp"
#include "printerdriver/transport.hpp"

// Diagnostic front end for the core. `print` deliberately goes through the whole
// engine — preflight, fences, job store, tri-state result — because a CLI that
// bypassed the engine would prove nothing about the engine.

namespace {

using namespace std::chrono_literals;

constexpr int kExitDone = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUnknown = 2;
constexpr int kExitUsage = 64;

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

  void expectQueued() {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.expectQueued();
  }

  bool waitForEvents(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] {
      return disconnected_ || events_.size() >= count;
    }) && events_.size() >= count;
  }

  bool waitForToken(const std::string& token, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, &token] {
      if (disconnected_) {
        return true;
      }
      for (const pd::escpos::ParsedEvent& event : events_) {
        if (event.kind == pd::escpos::ParsedEventKind::GsHAck && event.token == token) {
          return true;
        }
      }
      return false;
    }) && hasTokenLocked(token);
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
  bool hasTokenLocked(const std::string& token) const {
    for (const pd::escpos::ParsedEvent& event : events_) {
      if (event.kind == pd::escpos::ParsedEventKind::GsHAck && event.token == token) {
        return true;
      }
    }
    return false;
  }

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
      "usage:\n"
      "  pdctl status <host[:port]>\n"
      "      DLE EOT 1-4 decoded, plus the raw response bytes.\n"
      "\n"
      "  pdctl probe <host[:port]>\n"
      "      Capability probe: DLE EOT backchannel, GS ( H process-ID echo,\n"
      "      GS r 1 queued completion. Prints two short test sections, no cut.\n"
      "\n"
      "  pdctl print <host[:port]> --text \"...\" [options]\n"
      "      Print a small receipt through the full engine and stream job events.\n"
      "      --key <k>                   idempotency key (default: generated)\n"
      "      --profile xp-s260m|generic  capability profile (default: xp-s260m)\n"
      "      --no-cut                    do not cut after printing\n"
      "      --width <dots>              384 | 504 | 576 (default: 576)\n"
      "      --store <dir>               job store directory\n"
      "                                  (default: $HOME/.printerdriver)\n"
      "      --skip-preflight            do not refuse on cover/paper/error\n"
      "      --timeout <ms>              completion-wait budget per phase\n"
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

// --- probe ------------------------------------------------------------------------

int runProbe(const Endpoint& endpoint) {
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
  std::cout << "connected to " << transport.describe() << "\n"
            << "this prints two short test sections and does not cut\n\n";

  pd::escpos::Encoder reset;
  reset.initialize().asb(false);
  transport.write(reset.bytes());
  std::this_thread::sleep_for(300ms);
  listener.clear();

  // 1. Is there a backchannel at all?
  listener.expectRealtime(pd::escpos::DleEotKind::PrinterStatus);
  transport.write(pd::escpos::dleEot(pd::escpos::DleEotKind::PrinterStatus));
  listener.waitForEvents(1, 2000ms);
  const std::vector<uint8_t> realtime_raw = listener.raw();
  std::cout << "DLE EOT response: "
            << (realtime_raw.empty() ? "NO RESPONSE" : hex(realtime_raw)) << "\n";
  const bool backchannel = !realtime_raw.empty();
  listener.clear();

  // 2. GS ( H function 48 process-ID echo — the strong per-receipt fence.
  const std::string token = "P001";
  pd::escpos::Encoder marker_probe;
  marker_probe.line("GS(H) completion probe P001").feed().processId(token);
  transport.write(marker_probe.bytes());
  const bool gs_h = listener.waitForToken(token, 10000ms);
  const std::vector<uint8_t> gs_h_raw = listener.raw();
  std::cout << "GS(H): " << (gs_h ? "SUPPORTED" : "NO MATCH") << "  "
            << (gs_h_raw.empty() ? "NO RESPONSE" : hex(gs_h_raw)) << "\n";
  listener.clear();

  // 3. GS r 1 queued completion — the documented fallback.
  listener.expectQueued();
  pd::escpos::Encoder queued_probe;
  queued_probe.line("GS r completion probe").feed().queuedPaperStatus();
  transport.write(queued_probe.bytes());
  listener.waitForEvents(1, 10000ms);
  const std::vector<uint8_t> gs_r_raw = listener.raw();
  const bool gs_r = !gs_r_raw.empty();
  std::cout << "GS r response: " << (gs_r_raw.empty() ? "NO RESPONSE" : hex(gs_r_raw))
            << "\n\n";

  std::cout << "conclusion: ";
  if (gs_h) {
    std::cout << "use profile xp-s260m (GS ( H fence, confidence up to CutFaultFree)\n";
  } else if (gs_r) {
    std::cout << "use profile generic (GS r 1 fence, confidence capped at CutProcessed)\n";
  } else if (backchannel) {
    std::cout << "realtime status only: no ordered fence, every job ends Unknown or\n"
                 "            TransportAccepted - retry the probe over serial or USB\n";
  } else {
    std::cout << "write-only interface: no completion evidence is available here\n";
  }
  transport.close();
  return backchannel || gs_h || gs_r ? kExitDone : kExitFailed;
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

int runPrint(const Endpoint& endpoint, const PrintArgs& args) {
  pd::CapabilityProfile profile =
      args.profile == "generic" ? pd::generic_escpos() : pd::xp_s260m();

  pd::PrinterDriver driver(pd::StorageConfig::at(args.store));
  pd::PrinterConfig config;
  config.id = endpoint.host + ":" + std::to_string(endpoint.port);
  config.transport = pd::tcp(endpoint.host, endpoint.port, 3000);
  config.width_dots = args.width;
  config.profile = profile;
  auto printer = driver.addPrinter(config);

  std::cout << "printer  " << config.id << "\n"
            << "profile  " << profile.name << " (" << to_string(profile.completion)
            << ", ceiling " << pd::to_string(profile.maxConfidence()) << ")\n"
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
            << pd::to_string(result.confidence) << "\n";
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

}  // namespace

int main(int argc, char** argv) {
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
    if (command == "probe") {
      return runProbe(endpoint);
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
          if (args.profile != "xp-s260m" && args.profile != "generic") {
            std::cout << "unknown profile: " << args.profile << "\n\n";
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
