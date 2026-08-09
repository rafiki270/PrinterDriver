#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "printerdriver/agent/agent.hpp"

// pd-agent — the local printer agent of docs/techspec.md §5, exposed over HTTP.
//
// One process, one PrinterDriver, one journal, one instance nonce. Every printer it is
// configured with belongs to it exclusively for as long as it runs; see agent/README.md
// for what that invariant buys and what breaking it costs.

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

int usage() {
  std::cout <<
      "pd-agent - semantic print server for ESC/POS receipt printers\n"
      "\n"
      "  pd-agent [--config <file.json>] [--store <dir>] [--bind <addr>]\n"
      "           [--port <n>] [--workers <n>] [--wait <ms>]\n"
      "           [--printer <id>=<host>[:port][,width=<dots>][,profile=<name>]] ...\n"
      "\n"
      "  --config <file>   JSON config: bind, port, store, printers[]\n"
      "  --store <dir>     job journal directory. Without it the agent runs in\n"
      "                    memory: no crash recovery, never right for kitchen\n"
      "                    tickets\n"
      "  --bind <addr>     IPv4 address to listen on (default 127.0.0.1;\n"
      "                    0.0.0.0 for the LAN)\n"
      "  --port <n>        listen port (default 8080; 0 picks a free one)\n"
      "  --workers <n>     connection workers (default 4). Accept is always one\n"
      "                    thread; these keep a job waiting on a fence from\n"
      "                    blocking /healthz\n"
      "  --wait <ms>       how long POST /jobs waits for a terminal state before\n"
      "                    answering 202 (default 20000)\n"
      "  --printer ...     add a printer without a config file, e.g.\n"
      "                    --printer kitchen=192.168.1.101,profile=xprinter_s_series\n"
      "\n"
      "routes:\n"
      "  POST /jobs                      submit a receipt, get an evidence document\n"
      "  GET  /jobs/<id|key|token>       the same document for a known job\n"
      "  GET  /printers                  every printer this agent owns\n"
      "  POST /printers                  add one at runtime\n"
      "  GET  /printers/<id>/status      device status (?refresh=1 to query it)\n"
      "  GET  /healthz                   liveness, journal depth, instance nonce\n"
      // M13b: the CloudPRNT surface. The first three are the printer's, not yours —
      // a polling printer is configured with /cloudprnt/<id> and varies the method.
      "  POST/GET/DELETE /cloudprnt/<id> the CloudPRNT printer's own poll, job\n"
      "                                  download and confirmation\n"
      "  POST /cloudprnt/<id>/jobs       hand bytes to a CloudPRNT printer\n"
      "  GET  /cloudprnt/<id>/jobs[/<token>]  what is queued, and how it ended\n"
      "\n"
      "CloudPRNT printers are configured with the \"cloudprnt\" list in --config;\n"
      "they have no host and no port, because they dial the agent.\n"
      "\n"
      "The agent owns each printer connection exclusively. Do not point a second\n"
      "agent, a till, CUPS or a raw port-9100 client at a printer it owns: echoes\n"
      "become unattributable and two journals cannot dedupe each other.\n";
  return 64;
}

// "kitchen=192.168.1.101:9100,width=576,profile=xprinter_s_series"
bool parsePrinterFlag(const std::string& text, pd::agent::PrinterSpec* spec,
                      std::string* error) {
  const size_t equals = text.find('=');
  if (equals == std::string::npos || equals == 0) {
    *error = "expected <id>=<host>[:port][,key=value]...";
    return false;
  }
  spec->id = text.substr(0, equals);
  std::string rest = text.substr(equals + 1);

  size_t pos = 0;
  bool first = true;
  while (pos <= rest.size()) {
    size_t end = rest.find(',', pos);
    if (end == std::string::npos) {
      end = rest.size();
    }
    const std::string field = rest.substr(pos, end - pos);
    if (first) {
      const size_t colon = field.rfind(':');
      if (colon == std::string::npos) {
        spec->host = field;
      } else {
        spec->host = field.substr(0, colon);
        const long port = std::strtol(field.c_str() + colon + 1, nullptr, 10);
        if (port <= 0 || port > 65535) {
          *error = "port out of range in " + field;
          return false;
        }
        spec->port = static_cast<uint16_t>(port);
      }
      if (spec->host.empty()) {
        *error = "no host in " + text;
        return false;
      }
      first = false;
    } else if (!field.empty()) {
      const size_t split = field.find('=');
      if (split == std::string::npos) {
        *error = "expected key=value, got " + field;
        return false;
      }
      const std::string key = field.substr(0, split);
      const std::string value = field.substr(split + 1);
      if (key == "width") {
        spec->width_dots = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
      } else if (key == "profile") {
        spec->profile = value;
      } else if (key == "connectTimeoutMs") {
        spec->connect_timeout_ms =
            static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
      } else {
        *error = "unknown printer option: " + key;
        return false;
      }
    }
    if (end == rest.size()) {
      break;
    }
    pos = end + 1;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  pd::agent::AgentConfig config;
  std::string config_path;
  std::vector<pd::agent::PrinterSpec> extra;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if ((flag == "--help" || flag == "-h")) {
      return usage();
    } else if (flag == "--config" && has_value) {
      config_path = argv[++i];
    } else if (flag == "--store" && has_value) {
      config.store = argv[++i];
    } else if (flag == "--bind" && has_value) {
      config.bind = argv[++i];
    } else if (flag == "--port" && has_value) {
      const long port = std::strtol(argv[++i], nullptr, 10);
      if (port < 0 || port > 65535) {
        std::cout << "invalid port\n\n";
        return usage();
      }
      config.port = static_cast<uint16_t>(port);
    } else if (flag == "--workers" && has_value) {
      config.workers = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (flag == "--wait" && has_value) {
      config.wait_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (flag == "--printer" && has_value) {
      pd::agent::PrinterSpec spec;
      std::string error;
      if (!parsePrinterFlag(argv[++i], &spec, &error)) {
        std::cout << "--printer: " << error << "\n\n";
        return usage();
      }
      extra.push_back(std::move(spec));
    } else {
      std::cout << "unknown option: " << flag << "\n\n";
      return usage();
    }
  }

  if (!config_path.empty()) {
    // The file supplies the printer list; flags given on the command line win over it,
    // so a unit file can pin the bind address while the fleet stays data.
    pd::agent::AgentConfig from_file;
    std::string error;
    if (!pd::agent::loadAgentConfigFile(config_path, &from_file, &error)) {
      std::cout << "config: " << error << "\n";
      return 1;
    }
    for (int i = 1; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--store") from_file.store = config.store;
      if (flag == "--bind") from_file.bind = config.bind;
      if (flag == "--port") from_file.port = config.port;
      if (flag == "--workers") from_file.workers = config.workers;
      if (flag == "--wait") from_file.wait_ms = config.wait_ms;
    }
    config = std::move(from_file);
  }
  for (pd::agent::PrinterSpec& spec : extra) {
    config.printers.push_back(std::move(spec));
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  pd::agent::Agent agent(std::move(config));
  std::string error;
  if (!agent.start(&error)) {
    std::cout << "pd-agent: " << error << "\n";
    return 1;
  }

  const pd::agent::AgentConfig& effective = agent.config();
  std::cout << "pd-agent listening on http://" << effective.bind << ":" << agent.port()
            << "\n"
            << "  journal:  "
            << (effective.store.empty() ? std::string("in memory (no crash recovery)")
                                        : effective.store)
            << "\n"
            << "  printers: " << effective.printers.size() << "\n"
            << "  nonce:    " << agent.driver().instanceNonce() << "\n"
            << "\nthis process owns every printer above exclusively; nothing else may\n"
               "write to them while it runs\n";
  std::cout.flush();

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "\npd-agent stopping\n";
  agent.stop();
  return 0;
}
