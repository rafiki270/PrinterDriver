#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// A hand-rolled HTTP/1.1 server, because the agent's whole reason to exist is that the
// core has no third-party dependencies and can therefore be compiled anywhere
// (docs/sdk-spec.md §2). Pulling in an HTTP library for a submission API with six routes
// would put a package manager back in the loop on the one build — the Raspberry Pi
// agent — that is supposed to prove the core is portable.
//
// Deliberately small, and deliberately not general:
//
//  * **One accept thread**, plus a fixed pool of connection workers. The accept loop
//    never blocks on a request, so a job waiting on a completion fence cannot stop the
//    agent from answering /healthz — and a printer's lane still runs concurrently with
//    every other printer's, which is the topology docs/sdk-spec.md §14 requires.
//  * **`Connection: close` always.** Keep-alive buys nothing here (a till submits one
//    receipt per socket) and costs a state machine.
//  * **Content-Length bodies only.** `Transfer-Encoding: chunked` is answered 501
//    rather than half-parsed.
//  * **No TLS, no auth.** This binds to loopback or to a trusted LAN segment, exactly
//    like the printers it fronts. Anything else belongs behind a reverse proxy.

namespace pd::agent {

struct HttpRequest {
  std::string method;
  std::string target;    // as received, path and query together
  std::string raw_path;  // still encoded — split this, then decode each segment
  std::string path;      // percent-decoded, for logging and error bodies only
  std::string query;     // raw, undecoded
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
  std::string body;

  const std::string* header(std::string_view name) const noexcept;
  // First value of `name` in the query string, percent-decoded. Absent → nullopt-ish
  // empty string with `found` false.
  std::string queryValue(std::string_view name, bool* found = nullptr) const;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;

  static HttpResponse json(int status, std::string body) {
    HttpResponse out;
    out.status = status;
    out.body = std::move(body);
    return out;
  }
  static HttpResponse text(int status, std::string body) {
    HttpResponse out;
    out.status = status;
    out.content_type = "text/plain; charset=utf-8";
    out.body = std::move(body);
    return out;
  }
};

const char* httpReason(int status) noexcept;

// RFC 3986 percent-decoding, used for path segments: an idempotency key is caller-chosen
// text that routinely contains '#', and a verification token is base-94 over printable
// ASCII, so both reach a path only percent-encoded. '+' is left alone here — it means a
// space in form-encoded query values, which is what formDecode is for.
std::string percentDecode(std::string_view text);
std::string formDecode(std::string_view text);

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

struct HttpServerConfig {
  std::string bind = "127.0.0.1";
  // 0 asks the OS for an ephemeral port; port() reports what was bound.
  uint16_t port = 8080;
  // Connection workers. Accept is always single-threaded; these drain what it accepts.
  uint32_t workers = 4;
  // How long a connection may take to deliver its request line, headers and body. A
  // client that opens a socket and says nothing must not occupy a worker forever.
  uint32_t read_timeout_ms = 5000;
  size_t max_body_bytes = 8u * 1024u * 1024u;
  // Accepted-but-unhandled connections. Beyond this, new connections are closed
  // immediately: refusing loudly beats accumulating sockets toward a stalled agent,
  // which is the printer's 128 KB buffer problem one layer up (docs/sdk-spec.md §12).
  size_t max_queued_connections = 64;
  int backlog = 64;
};

class HttpServer {
 public:
  explicit HttpServer(HttpServerConfig config);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Binds, listens and starts the accept thread. False with `error` filled in on any
  // failure; the server is left stopped.
  bool start(HttpHandler handler, std::string* error);
  void stop();

  bool running() const noexcept { return running_.load(); }
  // The bound port, which is the interesting number when config.port was 0.
  uint16_t port() const noexcept { return port_.load(); }
  const HttpServerConfig& config() const noexcept { return config_; }

 private:
  void acceptLoop();
  void workerLoop();
  void serve(std::uintptr_t socket);

  HttpServerConfig config_;
  HttpHandler handler_;

  std::uintptr_t listen_socket_ = 0;
  bool listening_ = false;
  std::atomic<uint16_t> port_{0};
  std::atomic<bool> running_{false};

  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::uintptr_t> pending_;
};

// --- Client side ---------------------------------------------------------------------
//
// One blocking request over a fresh socket. It exists so the agent's own test suite can
// drive the real server over a real loopback socket rather than calling the handler
// directly — an evidence document that never went through the wire proves nothing about
// the wire.

struct HttpClientResult {
  bool ok = false;
  std::string error;
  int status = 0;
  std::string body;
};

HttpClientResult httpRequest(const std::string& host, uint16_t port,
                             const std::string& method, const std::string& target,
                             const std::string& body = {},
                             const std::string& content_type = "application/json",
                             uint32_t timeout_ms = 20000);

}  // namespace pd::agent
