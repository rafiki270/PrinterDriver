#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "printerdriver/agent/http.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/dsl/json.hpp"
// M13b: the embedded CloudPRNT server (docs/wire-protocols.md §2). A polling printer is
// not a lane the engine can write to, so it lives beside the driver rather than inside it.
#include "printerdriver/agent/cloudprnt.hpp"

// The local printer agent of docs/techspec.md §5 and docs/sdk-spec.md §12/§14: the
// shared-agent deployment shape, compiled from the same core every wrapper binds.
//
// -- What the agent is for --------------------------------------------------------------
//
// One invariant generates the whole design: **each printer has exactly one connection
// owner at a time** (docs/sdk-spec.md §14). Completion-fence attribution needs it —
// a GS ( H echo is only attributable on the socket that sent the marker — and so does
// fleet-wide duplicate prevention, because idempotency keys only dedupe within one
// journal. So the agent holds exactly **one PrinterDriver**: one job store, one key
// index, one instance nonce. Two tills submitting the same order key print once,
// which is the entire kitchen-duplicate problem solved one layer up from the socket.
//
// -- What it is not ----------------------------------------------------------------------
//
// It is not a queue. Store-and-forward, TTLs and priority are the addon's policy
// (docs/sdk-spec.md §12); the agent submits through the same core path a direct caller
// would and reports what came back. It never retries out of `Unknown`, and it never
// upgrades a claim: every response carries the grade, the authority and the command
// that produced it, so "printed" from a fence and "printed" from a socket write are
// different documents (docs/compatibility-brief.md §24).

namespace pd::agent {

// One printer the agent owns. `profile` is a name from the device database
// (printerdriver/device_profiles.hpp), or "xp-s260m" / "generic" for the two built-in
// engine profiles.
struct PrinterSpec {
  std::string id;
  std::string host;
  uint16_t port = 9100;
  uint32_t width_dots = 0;  // 0 → the profile's printable width
  std::string profile = "generic";
  uint32_t connect_timeout_ms = 3000;

  std::string endpoint() const;
};

struct AgentConfig {
  // Journal directory. Empty means in-memory: no crash recovery, which is never
  // appropriate for kitchen tickets and is why --store has no default here.
  std::string store;
  std::string bind = "127.0.0.1";
  uint16_t port = 8080;
  uint32_t workers = 4;
  // How long POST /jobs waits for a job to reach a terminal state before answering 202
  // with the state so far. The job is not cancelled — it is still the agent's, and the
  // caller can poll GET /jobs/<key>.
  uint32_t wait_ms = 20000;
  std::vector<PrinterSpec> printers;

  // --- M13b: CloudPRNT ------------------------------------------------------------------
  // A separate list because these are a different topology, not a different transport:
  // nothing here has a host or a port, because the printer dials the agent
  // (docs/wire-protocols.md §2). Their ids share one namespace with `printers` above, so a
  // reader of GET /printers never has to wonder which "kitchen" was meant.
  std::vector<CloudPrntSpec> cloudprnt;
  // --- end M13b -------------------------------------------------------------------------
};

// Config file shape:
//   { "bind": "0.0.0.0", "port": 8080, "store": "/var/lib/pd-agent",
//     "printers": [ { "id": "kitchen", "tcp": { "host": "192.168.1.101", "port": 9100 },
//                     "widthDots": 576, "profile": "xprinter_s_series" } ] }
// Command-line flags win over the file, so a unit file can pin the bind address while
// the printer list stays data.
bool parseAgentConfig(const dsl::Json& json, AgentConfig* out, std::string* error);
bool loadAgentConfigFile(const std::string& path, AgentConfig* out, std::string* error);

class Agent {
 public:
  explicit Agent(AgentConfig config);
  ~Agent();

  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;

  // Adjusts the PrinterConfig the agent built from a spec, immediately before it is
  // added. The suite points a printer at a scripted device and shortens the production
  // timeouts with it; an embedder can install a profile the device database does not
  // carry. Nothing on the HTTP surface can reach it — a caller must never be able to
  // choose its own transport or downgrade a fence.
  using PrinterConfigHook = std::function<void(const PrinterSpec&, PrinterConfig*)>;
  void setPrinterConfigHook(PrinterConfigHook hook);

  // Adds the configured printers and starts serving. False with `error` filled in.
  bool start(std::string* error);
  void stop();
  uint16_t port() const noexcept { return server_.port(); }

  // The routing table, exposed so a test can exercise a route without a socket. The
  // agent's own suite deliberately goes over the wire instead.
  HttpResponse handle(const HttpRequest& request);

  // `assigned_id` reports the id the printer actually got, which matters when the spec
  // left it empty and it was derived from the endpoint.
  bool addPrinter(const PrinterSpec& spec, std::string* error,
                  std::string* assigned_id = nullptr);

  PrinterDriver& driver() noexcept { return *driver_; }
  const AgentConfig& config() const noexcept { return config_; }

  // --- M13b: CloudPRNT ------------------------------------------------------------------
  // The polling printers, exposed so an embedder can hand bytes in without going through
  // HTTP and can subscribe to the device events their polls carry.
  CloudPrntServer& cloudprnt() noexcept { return cloudprnt_; }
  const CloudPrntServer& cloudprnt() const noexcept { return cloudprnt_; }
  // --- end M13b -------------------------------------------------------------------------

 private:
  struct Owned {
    PrinterSpec spec;
    std::shared_ptr<Printer> printer;
  };

  // docs/sdk-spec.md §14: a GS ( H echo carrying a token this instance never issued
  // means something else is writing to a printer the agent believes it owns. Sticky
  // once seen, because a violated topology does not heal itself. Its own lock, held
  // only for the flag: a device-event callback must never wait on the agent's lock.
  struct ForeignWriters {
    mutable std::mutex mutex;
    std::unordered_map<std::string, bool> seen;

    void mark(const std::string& printer_id) {
      std::lock_guard<std::mutex> lock(mutex);
      seen[printer_id] = true;
    }
    bool detected(const std::string& printer_id) const {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = seen.find(printer_id);
      return it != seen.end() && it->second;
    }
    bool any() const {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& entry : seen) {
        if (entry.second) {
          return true;
        }
      }
      return false;
    }
  };

  HttpResponse postJobs(const HttpRequest& request);
  HttpResponse getJob(const std::string& reference);
  HttpResponse getPrinters() const;
  HttpResponse postPrinters(const HttpRequest& request);
  HttpResponse getPrinterStatus(const std::string& id, const HttpRequest& request);
  // M14 — POST /printers/<id>/drawer {channel?, pulseMs?} (docs/cash-drawer.md).
  HttpResponse postPrinterDrawer(const std::string& id, const HttpRequest& request);
  // --- M15 (docs/api.md §15) ------------------------------------------------------
  // POST /printers/<id>/self-test  — one diagnostic ticket, the whole detection report
  //                                  and the ordinary tri-state result. Uses paper.
  // POST /autodetect {cidr?}       — discovery + identify + the printless probe subset.
  //                                  Nothing prints and nothing fires.
  HttpResponse postPrinterSelfTest(const std::string& id, const HttpRequest& request);
  HttpResponse postAutoDetect(const HttpRequest& request);
  // --- end M15 ---------------------------------------------------------------------
  HttpResponse getHealth() const;

  // --- M13b: CloudPRNT ------------------------------------------------------------------
  // The printer-facing triple plus the application-facing submission route, split out so
  // Agent::handle stays a routing table and nothing about the polling contract leaks into
  // the pushed path.
  HttpResponse cloudPrntRoute(const std::vector<std::string>& path,
                              const HttpRequest& request);
  // --- end M13b -------------------------------------------------------------------------

  std::shared_ptr<Printer> lookup(const std::string& id) const;
  const Owned* owned(const std::string& id) const;
  dsl::Json printerJson(const Owned& entry) const;
  // The evidence document of docs/techspec.md §5, built from the journal record so a
  // job that outlived the process still answers.
  dsl::Json evidenceJson(const JobRecord& record, const std::shared_ptr<PrintJob>& job,
                         bool deduped) const;

  AgentConfig config_;
  std::unique_ptr<PrinterDriver> driver_;
  // M13b: its own repository, its own lock. A CloudPRNT job never enters the driver's
  // journal because it never enters the driver: the printer pulls, and there is no send
  // for the journal to record (docs/wire-protocols.md §2).
  CloudPrntServer cloudprnt_;
  HttpServer server_;
  std::shared_ptr<ForeignWriters> foreign_;
  PrinterConfigHook hook_;

  mutable std::mutex mutex_;
  std::vector<Owned> printers_;
  std::unordered_map<std::string, size_t> by_id_;
  MonotonicTime started_{};
  bool serving_ = false;
};

}  // namespace pd::agent
