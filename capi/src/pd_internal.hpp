#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "printerdriver/driver.hpp"
#include "printerdriver/pd.h"
#include "printerdriver/transport.hpp"

// The C++ side of the handles declared opaque in pd.h. Kept out of the public header
// on purpose: a wrapper that can see these layouts will eventually depend on one.
//
// It is also the seam a test binary uses to attach a printer whose transport cannot be
// described in C (a scripted in-process device). attachPrinter() is the same call
// pd_add_printer_tcp() makes once it has turned a pd_tcp_config into a PrinterConfig,
// so a scripted printer is not a second code path — it is the same one with a
// different transport factory.

struct pd_printer {
  pd_driver* owner = nullptr;
  std::shared_ptr<pd::Printer> printer;
  std::string id;
  // Non-null only for a printer added through pd_add_printer_custom. The link, not the
  // transport instance, is what the embedder feeds: the core creates a fresh transport
  // after every reconnect, and a caller holding a pd_printer must not have to notice.
  std::shared_ptr<pd::CustomTransportLink> link;
};

struct pd_job {
  pd_driver* owner = nullptr;
  std::shared_ptr<pd::PrintJob> job;
  std::string id;
  std::string key;
  // Storage for the strings pd.h promises are owned by the handle: the evidence method
  // handed back through pd_job_result, and the two verification identifiers. The
  // tokens are read lazily, because a job is interned the moment it is submitted and
  // may not have reached its worker yet. Guarded by pd_driver::mutex.
  std::string method = "none";
  std::string print_token;
  std::string cut_token;
};

// M15. Backing storage for the strings pd_detection_summary hands back as `const char*`
// (docs/api.md §15). Used two ways: owned by the driver for pd_self_test, whose result
// pd.h promises stays valid until the next self-test on that driver, and built on the
// stack inside pd_auto_detect's per-candidate callback, where pd.h promises only the
// duration of the call.
struct pd_summary_storage {
  std::string endpoint;
  std::string vendor;
  std::string model;
  std::string firmware;
  std::string serial;
  std::string profile_id;
  std::string method;
  std::string provenance_summary;
  std::vector<std::string> degradations;
  // Rebuilt whenever `degradations` changes: the array of pointers pd.h exposes has to
  // outlive the call that fills it, and a vector<string> does not hand one out.
  std::vector<const char*> degradation_ptrs;
};

struct pd_self_test_storage {
  pd_summary_storage summary;
  std::string key;
  std::string print_token;
  std::string ticket_text;
  // The JobResult's own method string, which is not the summary's: a job refused before
  // any byte reached the link reports "none" while the profile still names a mechanism.
  std::string result_method = "none";
};

struct pd_driver {
  std::unique_ptr<pd::PrinterDriver> driver;
  std::mutex mutex;
  std::vector<std::unique_ptr<pd_printer>> printers;
  std::unordered_map<const pd::PrintJob*, std::unique_ptr<pd_job>> jobs;
  std::string last_error;
  pd_log_cb log = nullptr;
  void* log_ctx = nullptr;
  // M15. The last self-test's strings, and the last answer pd_local_subnet gave.
  pd_self_test_storage self_test;
  std::string local_subnet;
  // M15. The last sweep's results, kept so a wrapper can read them by index instead of
  // through a callback. Dart is why this exists: a Dart isolate blocked inside an FFI
  // call cannot service a native callback until that call returns, so a callback-only
  // ABI would be unusable from the one wrapper that binds this library by dlopen.
  std::vector<pd::DetectedPrinter> detected;
  std::vector<pd::DiscoveredDevice> discovered;
  // One reusable slot for the strings pd_detected_at / pd_discovered_at hand out.
  pd_summary_storage detected_view;
  std::string detected_endpoint;
  std::string detected_host;
  std::string detected_hex;
  std::string discovered_ip;
  std::string discovered_hex;
};

namespace pd {
namespace capi {

pd_printer* attachPrinter(pd_driver* driver, PrinterConfig config);

// Interns a core job as a stable handle: the same PrintJob always yields the same
// pd_job pointer, which is what makes idempotency-key dedupe visible to C as pointer
// equality.
pd_job* internJob(pd_driver* driver, const std::shared_ptr<PrintJob>& job);

}  // namespace capi
}  // namespace pd
