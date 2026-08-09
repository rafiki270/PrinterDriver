#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "printerdriver/agent/agent.hpp"
#include "printerdriver/agent/cloudprnt.hpp"
#include "printerdriver/dsl/json.hpp"
#include "printerdriver/net_platform.hpp"
#include "test_harness.hpp"

// The CloudPRNT suite drives the real HTTP server over a real loopback socket with a
// scripted printer client — one that polls, downloads and confirms exactly as
// docs/wire-protocols.md §2 says firmware does, because every property worth proving here
// is a property of the wire:
//
//   * a download is idempotent and never consumes the job (§2 server rules 1 and 3), so an
//     interrupted transfer is recoverable and only the DELETE retires anything;
//   * jobs are keyed by printer identity + token (§2 server rule 2), so a second device on
//     the segment cannot take or confirm somebody else's receipt;
//   * `code=200` is the only thing that produces Done, at grade A / PhysicalPrinter /
//     "CloudPRNT" (docs/compatibility-brief.md §24), and a documented failure code produces
//     an honest failure rather than a downgraded success.
//
// Nothing here goes through a scripted *device*: a CloudPRNT printer pulls, so there is no
// transport for a mock to sit behind. The HTTP conversation is the whole mechanism.

using pd::agent::Agent;
using pd::agent::AgentConfig;
using pd::agent::CloudPrntSpec;
using pd::agent::HttpClientResult;
using pd::dsl::Json;

namespace {

constexpr const char* kStarLine = "application/vnd.star.line";
constexpr const char* kCounterMac = "00:11:62:AA:BB:CC";
constexpr const char* kBarMac = "00:11:62:00:00:01";

Json parse(const std::string& text) {
  Json json;
  std::string error;
  if (!pd::dsl::tryParseJson(text, &json, &error)) {
    return Json::object({});
  }
  return json;
}

std::string field(const Json& json, const char* key) {
  const Json* value = json.find(key);
  return value != nullptr && value->isString() ? value->asString() : std::string();
}

bool flag(const Json& json, const char* key) {
  const Json* value = json.find(key);
  return value != nullptr && value->truthy();
}

long long number(const Json& json, const char* key) {
  const Json* value = json.find(key);
  return value != nullptr && value->isNumber() ? value->asInt() : -1;
}

std::string urlEncode(const std::string& text) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                            c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(digits[(c >> 4) & 0x0F]);
      out.push_back(digits[c & 0x0F]);
    }
  }
  return out;
}

std::string base64(const std::vector<uint8_t>& data) {
  static const char* alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
    const uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(alphabet[(triple >> 18) & 0x3F]);
    out.push_back(alphabet[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < data.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < data.size() ? alphabet[triple & 0x3F] : '=');
  }
  return out;
}

// A receipt as a printer would receive it: ESC @, text, a NUL and a high byte, so the
// download proves the response body is binary-clean rather than string-shaped.
std::vector<uint8_t> receiptBytes() {
  return {0x1B, 0x40, 'C', 'O', 'U', 'N', 'T', 'E', 'R', ' ', '1', 0x0A, 0x00, 0xFF, 0x0A};
}

std::string asString(const std::vector<uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// --- A raw client, because the download's Content-Type is part of the contract ----------
//
// pd::agent::httpRequest returns the status and the body, which is everything the other
// routes are judged on. The job download is judged on its header too — §2 has the printer
// pick a media type and the server answer in it — so this one reads the whole response.

struct RawResponse {
  bool ok = false;
  int status = 0;
  std::string headers;  // lowercased header block
  std::string body;
};

RawResponse rawRequest(uint16_t port, const std::string& method, const std::string& target,
                       const std::string& body = {},
                       const std::string& content_type = {}) {
  using pd::net::Socket;
  RawResponse out;
  if (!pd::net::startup()) {
    return out;
  }
  const Socket socket = pd::net::create(AF_INET, SOCK_STREAM, 0);
  if (!pd::net::valid(socket)) {
    return out;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = pd::net::fromNetwork16(port);
  address.sin_addr.s_addr = pd::net::loopbackAddress();
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address),
                static_cast<pd::net::SockLen>(sizeof(address))) != 0) {
    pd::net::closeSocket(socket);
    return out;
  }

  std::string request = method + " " + target + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
  request += "Connection: close\r\n";
  if (!content_type.empty()) {
    request += "Content-Type: " + content_type + "\r\n";
  }
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  request += body;
  size_t sent = 0;
  while (sent < request.size()) {
    const int64_t wrote =
        pd::net::sendSome(socket, request.data() + sent, request.size() - sent);
    if (wrote <= 0) {
      pd::net::closeSocket(socket);
      return out;
    }
    sent += static_cast<size_t>(wrote);
  }

  // The agent always answers `Connection: close`, so reading to EOF is the complete
  // response, headers included.
  std::string buffer;
  char chunk[4096];
  for (;;) {
    pd::net::PollFd waiter;
    waiter.socket = socket;
    waiter.events = pd::net::kPollIn;
    if (pd::net::poll(&waiter, 1, 5000) <= 0) {
      break;
    }
    const int64_t got = pd::net::recvSome(socket, chunk, sizeof(chunk));
    if (got <= 0) {
      break;
    }
    buffer.append(chunk, static_cast<size_t>(got));
  }
  pd::net::shutdownBoth(socket);
  pd::net::closeSocket(socket);

  const size_t split = buffer.find("\r\n\r\n");
  if (split == std::string::npos) {
    return out;
  }
  out.headers = buffer.substr(0, split);
  for (char& c : out.headers) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  out.body = buffer.substr(split + 4);
  const size_t space = buffer.find(' ');
  out.status = space == std::string::npos ? 0 : std::atoi(buffer.c_str() + space + 1);
  out.ok = true;
  return out;
}

bool headerSays(const RawResponse& response, const std::string& lowercase_fragment) {
  return response.headers.find(lowercase_fragment) != std::string::npos;
}

// --- The scripted printer ----------------------------------------------------------------
//
// Every call is one real HTTP request with the parameters §2 documents. The printer keeps
// the token it was offered, exactly as firmware does, so the walk reads like the
// conversation it is.

struct PollAnswer {
  int status = 0;
  bool job_ready = false;
  std::string token;
  std::string delete_method;
  std::vector<std::string> media_types;
};

struct ScriptedPrinter {
  uint16_t port = 0;
  std::string route = "/cloudprnt/counter";
  std::string mac = kCounterMac;
  std::string uid = "star-tsp143-01";
  std::string asb = "23060000000000";
  bool printing = false;
  std::string held;

  PollAnswer poll(const std::string& status_code = "200%20OK") {
    Json body = Json::object({});
    body.set("status", Json::string(asb));
    // The documented spelling: the percent-encoded "200 OK" form firmware sends.
    body.set("statusCode", Json::string(status_code));
    body.set("printerMAC", Json::string(mac));
    body.set("uniqueID", Json::string(uid));
    body.set("jobToken", Json::string(held));
    body.set("printingInProgress", Json::boolean(printing));
    const HttpClientResult response = pd::agent::httpRequest(
        "127.0.0.1", port, "POST", route, pd::dsl::toJson(body));
    PollAnswer out;
    out.status = response.status;
    const Json json = parse(response.body);
    out.job_ready = flag(json, "jobReady");
    out.token = field(json, "jobToken");
    out.delete_method = field(json, "deleteMethod");
    if (const Json* types = json.find("mediaTypes"); types != nullptr && types->isArray()) {
      for (const Json& entry : types->asArray()) {
        out.media_types.push_back(entry.asString());
      }
    }
    if (out.job_ready) {
      held = out.token;
    }
    return out;
  }

  RawResponse download(const std::string& type = kStarLine,
                       const std::string& token = {}) const {
    std::string target = route + "?uid=" + urlEncode(uid) + "&type=" + urlEncode(type) +
                         "&mac=" + urlEncode(mac);
    const std::string wanted = token.empty() ? held : token;
    if (!wanted.empty()) {
      target += "&token=" + urlEncode(wanted);
    }
    return rawRequest(port, "GET", target);
  }

  HttpClientResult confirm(const std::string& code = "200%20OK",
                           const std::string& token = {},
                           const std::string& retry = {}) const {
    std::string target = route + "?uid=" + urlEncode(uid) + "&mac=" + urlEncode(mac) +
                         "&code=" + code;
    const std::string wanted = token.empty() ? held : token;
    if (!wanted.empty()) {
      target += "&token=" + urlEncode(wanted);
    }
    if (!retry.empty()) {
      target += "&retry=" + retry;
    }
    return pd::agent::httpRequest("127.0.0.1", port, "DELETE", target);
  }
};

// One agent on an ephemeral loopback port serving two CloudPRNT printers and no pushed
// ones: the polling path shares nothing with the engine, so nothing here needs a socket to
// a device.
struct Fixture {
  std::unique_ptr<Agent> agent;
  std::string error;
  mutable std::mutex events_mutex;
  std::vector<std::pair<std::string, pd::DeviceEvent>> events;

  bool start() {
    AgentConfig config;
    config.bind = "127.0.0.1";
    config.port = 0;  // ephemeral
    config.workers = 3;

    CloudPrntSpec counter;
    counter.id = "counter";
    counter.mac = kCounterMac;
    counter.media_types = {kStarLine, "text/plain"};
    config.cloudprnt.push_back(counter);

    CloudPrntSpec bar;
    bar.id = "bar";
    bar.mac = kBarMac;
    config.cloudprnt.push_back(bar);

    agent = std::make_unique<Agent>(std::move(config));
    agent->cloudprnt().subscribeDevices(
        [this](const std::string& printer_id, pd::DeviceEvent event) {
          std::lock_guard<std::mutex> lock(events_mutex);
          events.emplace_back(printer_id, event);
        });
    return agent->start(&error);
  }

  // stop() joins the server threads, so no subscriber can run against a half-destroyed
  // fixture.
  ~Fixture() {
    if (agent) {
      agent->stop();
    }
  }

  uint16_t port() const { return agent->port(); }

  ScriptedPrinter printer(const std::string& route = "/cloudprnt/counter",
                          const std::string& mac = kCounterMac) const {
    ScriptedPrinter out;
    out.port = port();
    out.route = route;
    out.mac = mac;
    return out;
  }

  HttpClientResult get(const std::string& target) const {
    return pd::agent::httpRequest("127.0.0.1", port(), "GET", target);
  }
  HttpClientResult post(const std::string& target, const std::string& body) const {
    return pd::agent::httpRequest("127.0.0.1", port(), "POST", target, body);
  }

  // The application side: hand bytes to a printer and get the token back.
  std::string submit(const std::vector<uint8_t>& bytes,
                     const std::string& media_type = kStarLine,
                     const std::string& id = "counter") const {
    Json body = Json::object({});
    body.set("mediaType", Json::string(media_type));
    body.set("base64", Json::string(base64(bytes)));
    const HttpClientResult response =
        post("/cloudprnt/" + id + "/jobs", pd::dsl::toJson(body));
    CHECK_EQ(response.status, 201);
    return field(parse(response.body), "token");
  }

  Json jobDoc(const std::string& token, const std::string& id = "counter") const {
    return parse(get("/cloudprnt/" + id + "/jobs/" + urlEncode(token)).body);
  }

  std::vector<pd::DeviceEvent> eventsFor(const std::string& id) const {
    std::lock_guard<std::mutex> lock(events_mutex);
    std::vector<pd::DeviceEvent> out;
    for (const auto& entry : events) {
      if (entry.first == id) {
        out.push_back(entry.second);
      }
    }
    return out;
  }
};

}  // namespace

PD_TEST(a_cloudprnt_printer_walks_poll_download_and_confirmation) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::vector<uint8_t> bytes = receiptBytes();
  const std::string token = fixture.submit(bytes);
  CHECK(!token.empty());

  ScriptedPrinter printer = fixture.printer();

  // 1. The poll. docs/wire-protocols.md §2: jobReady, the types this job can be served
  //    as, the token, and the delete method — which is DELETE, always.
  const PollAnswer offered = printer.poll();
  CHECK_EQ(offered.status, 200);
  CHECK(offered.job_ready);
  CHECK_EQ(offered.token, token);
  CHECK_EQ(offered.delete_method, std::string("DELETE"));
  CHECK_EQ(offered.media_types.size(), static_cast<size_t>(1));
  if (!offered.media_types.empty()) {
    CHECK_EQ(offered.media_types[0], std::string(kStarLine));
  }

  // 2. The download, in the type the printer picked, byte for byte.
  const RawResponse downloaded = printer.download();
  CHECK(downloaded.ok);
  CHECK_EQ(downloaded.status, 200);
  CHECK_EQ(downloaded.body, asString(bytes));
  CHECK(headerSays(downloaded, "content-type: application/vnd.star.line"));

  // A downloaded job is not a printed job. Until the confirmation lands there is no
  // outcome at all — this is the claim the whole file exists to keep honest.
  const Json in_flight = fixture.jobDoc(token);
  CHECK_EQ(field(in_flight, "state"), std::string("delivered"));
  CHECK(!flag(in_flight, "terminal"));
  CHECK(in_flight.find("outcome") == nullptr);
  CHECK_EQ(field(in_flight, "grade"), std::string("E"));

  // 3. The confirmation, in the documented percent-encoded spelling.
  const HttpClientResult confirmed = printer.confirm("200%20OK");
  CHECK_EQ(confirmed.status, 200);
  const Json receipt = parse(confirmed.body);
  CHECK(flag(receipt, "ok"));
  CHECK_EQ(number(receipt, "code"), 200LL);
  CHECK_EQ(field(receipt, "outcome"), std::string("Done"));

  // docs/compatibility-brief.md §24: a job-level confirmation from the mechanism that
  // moved the paper is grade A, and the method names it.
  const Json evidence = fixture.jobDoc(token);
  CHECK_EQ(field(evidence, "state"), std::string("confirmed"));
  CHECK_EQ(field(evidence, "outcome"), std::string("Done"));
  CHECK_EQ(field(evidence, "grade"), std::string("A"));
  CHECK_EQ(field(evidence, "authority"), std::string("PhysicalPrinter"));
  CHECK_EQ(field(evidence, "method"), std::string("CloudPRNT"));
  CHECK_EQ(field(evidence, "confidence"), std::string("PrintConfirmed"));
  CHECK(flag(evidence, "terminal"));
  CHECK_EQ(number(evidence, "deliveries"), 1LL);

  // And the queue is empty: the DELETE is what retires a job.
  const PollAnswer idle = printer.poll();
  CHECK(!idle.job_ready);
  CHECK_EQ(idle.token, std::string());
}

PD_TEST(an_interrupted_transfer_downloads_the_same_bytes_again) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::vector<uint8_t> bytes = receiptBytes();
  const std::string token = fixture.submit(bytes);

  ScriptedPrinter printer = fixture.printer();
  CHECK(printer.poll().job_ready);

  // §2: the download "must be idempotent — re-download after interruption". A printer
  // whose transfer was cut off asks again with the same token and must get the same
  // document; anything else prints a truncated receipt or nothing at all.
  const RawResponse first = printer.download();
  const RawResponse second = printer.download();
  CHECK_EQ(first.status, 200);
  CHECK_EQ(second.status, 200);
  CHECK_EQ(second.body, first.body);
  CHECK_EQ(second.body, asString(bytes));
  CHECK(headerSays(second, "content-type: application/vnd.star.line"));

  CHECK_EQ(number(fixture.jobDoc(token), "deliveries"), 2LL);
  CHECK(!flag(fixture.jobDoc(token), "terminal"));
}

PD_TEST(a_download_never_consumes_the_job) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::string token = fixture.submit(receiptBytes());
  ScriptedPrinter printer = fixture.printer();

  CHECK_EQ(printer.poll().token, token);
  CHECK_EQ(printer.download().status, 200);

  // §2 server rule: never consume on GET alone. The job is still offered, still
  // downloadable, and still not terminal.
  const PollAnswer again = printer.poll();
  CHECK(again.job_ready);
  CHECK_EQ(again.token, token);
  CHECK_EQ(printer.download().status, 200);
  CHECK_EQ(field(fixture.jobDoc(token), "state"), std::string("delivered"));
  CHECK_EQ(number(fixture.jobDoc(token), "deliveries"), 2LL);

  // Only the DELETE deletes — and once it has, the same download is gone.
  CHECK_EQ(printer.confirm("200%20OK", token).status, 200);
  CHECK_EQ(printer.download(kStarLine, token).status, 404);
  CHECK(!printer.poll().job_ready);
}

PD_TEST(a_failure_confirmation_code_is_never_reported_as_done) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::string token = fixture.submit(receiptBytes());
  ScriptedPrinter printer = fixture.printer();
  CHECK(printer.poll().job_ready);
  CHECK_EQ(printer.download().status, 200);

  // 410 is "paper out" in §2's table. The printer had the bytes and no receipt exists.
  const HttpClientResult answered = printer.confirm("410", token);
  CHECK_EQ(answered.status, 200);  // the confirmation was accepted; the job was not printed
  CHECK_EQ(field(parse(answered.body), "outcome"), std::string("Failed"));

  const Json evidence = fixture.jobDoc(token);
  CHECK_EQ(field(evidence, "outcome"), std::string("Failed"));
  CHECK(field(evidence, "outcome") != std::string("Done"));
  CHECK_EQ(field(evidence, "reason"), std::string("PreflightPaperOut"));
  CHECK_EQ(field(evidence, "detail"), std::string("paper out"));
  CHECK_EQ(number(evidence, "code"), 410LL);
  // The failure is as well attested as a success would have been: the printer itself said
  // it, at job level. Grade describes the evidence, not the verdict.
  CHECK_EQ(field(evidence, "grade"), std::string("A"));
  CHECK_EQ(field(evidence, "authority"), std::string("PhysicalPrinter"));
  CHECK_EQ(field(evidence, "method"), std::string("CloudPRNT"));
  CHECK(flag(evidence, "terminal"));
}

PD_TEST(the_documented_status_codes_map_to_honest_outcomes) {
  using pd::agent::cloudPrntDisposition;
  using pd::agent::cloudPrntDeviceEvent;
  using pd::agent::cloudPrntStatusCode;

  // Every spelling §2 shows, plus the bare integer some firmware sends.
  int code = 0;
  CHECK(cloudPrntStatusCode("200 OK", &code));
  CHECK_EQ(code, 200);
  CHECK(cloudPrntStatusCode(pd::agent::percentDecode("200%20OK"), &code));
  CHECK_EQ(code, 200);
  CHECK(cloudPrntStatusCode("200", &code));
  CHECK_EQ(code, 200);
  CHECK(cloudPrntStatusCode("410 paper out", &code));
  CHECK_EQ(code, 410);
  CHECK(!cloudPrntStatusCode("", &code));
  CHECK(!cloudPrntStatusCode("OK", &code));

  // 200 is the only code that may produce Done (docs/compatibility-brief.md §24).
  CHECK_EQ(cloudPrntDisposition(200).outcome, pd::JobOutcome::Done);
  CHECK_EQ(cloudPrntDisposition(200).reason, pd::FailureReason::None);
  for (const int other : {201, 211, 220, 221, 230, 231, 410, 411, 412, 420, 510, 511, 512,
                          520, 521, 1000, 1001, 499, 12}) {
    CHECK(cloudPrntDisposition(other).outcome != pd::JobOutcome::Done);
  }

  // The documented failures, each with the reason a support engineer can act on.
  CHECK_EQ(cloudPrntDisposition(410).reason, pd::FailureReason::PreflightPaperOut);
  CHECK_EQ(cloudPrntDisposition(411).reason, pd::FailureReason::PreflightHardwareError);
  CHECK_EQ(cloudPrntDisposition(412).reason, pd::FailureReason::PreflightHardwareError);
  CHECK_EQ(cloudPrntDisposition(420).reason, pd::FailureReason::PreflightCoverOpen);
  CHECK_EQ(cloudPrntDisposition(510).reason, pd::FailureReason::Unsupported);
  CHECK_EQ(cloudPrntDisposition(511).reason, pd::FailureReason::Unsupported);
  CHECK_EQ(cloudPrntDisposition(512).reason, pd::FailureReason::Unsupported);
  CHECK_EQ(cloudPrntDisposition(520).reason,
           pd::FailureReason::TimeoutAwaitingCompletion);
  CHECK_EQ(cloudPrntDisposition(521).reason, pd::FailureReason::Unsupported);
  CHECK_EQ(cloudPrntDisposition(1000).outcome, pd::JobOutcome::Failed);
  CHECK_EQ(cloudPrntDisposition(1001).outcome, pd::JobOutcome::Failed);
  for (const int failure : {410, 411, 412, 420, 510, 511, 512, 520, 521, 1000, 1001}) {
    CHECK_EQ(cloudPrntDisposition(failure).outcome, pd::JobOutcome::Failed);
    CHECK(cloudPrntDisposition(failure).documented);
  }
  // Device conditions are not job verdicts: as a confirmation they resolve to Unknown,
  // never to either bucket.
  for (const int condition : {201, 211, 220, 221, 230, 231}) {
    CHECK_EQ(cloudPrntDisposition(condition).outcome, pd::JobOutcome::Unknown);
    CHECK(cloudPrntDisposition(condition).documented);
  }
  // A code the table does not list says only which half of the wire it came from.
  CHECK_EQ(cloudPrntDisposition(499).outcome, pd::JobOutcome::Failed);
  CHECK(!cloudPrntDisposition(499).documented);
  CHECK_EQ(cloudPrntDisposition(12).outcome, pd::JobOutcome::Unknown);

  // The closed pd::DeviceEvent enum is used where a member exists, and nowhere else.
  pd::DeviceEvent event = pd::DeviceEvent::Online;
  CHECK(cloudPrntDeviceEvent(211, &event));
  CHECK_EQ(event, pd::DeviceEvent::PaperNearEnd);
  CHECK(cloudPrntDeviceEvent(410, &event));
  CHECK_EQ(event, pd::DeviceEvent::PaperOut);
  CHECK(cloudPrntDeviceEvent(420, &event));
  CHECK_EQ(event, pd::DeviceEvent::CoverOpen);
  CHECK(cloudPrntDeviceEvent(411, &event));
  CHECK_EQ(event, pd::DeviceEvent::RecoverableError);
  CHECK(cloudPrntDeviceEvent(412, &event));
  CHECK_EQ(event, pd::DeviceEvent::RecoverableError);
  for (const int unmapped : {201, 220, 221, 230, 231, 510, 520, 1000}) {
    CHECK(!cloudPrntDeviceEvent(unmapped, nullptr));
  }
}

PD_TEST(another_printer_can_neither_download_nor_confirm_the_job) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::string token = fixture.submit(receiptBytes());
  ScriptedPrinter counter = fixture.printer();
  CHECK(counter.poll().job_ready);

  // §2 server rule 2: keyed by printer identity + token. A second device on the segment
  // that learned the token — or simply guessed the route — must get nothing.
  ScriptedPrinter impostor = fixture.printer("/cloudprnt/counter", "00:11:62:DE:AD:00");
  impostor.held = token;
  CHECK_EQ(impostor.poll().status, 404);
  CHECK_EQ(impostor.download(kStarLine, token).status, 404);
  CHECK_EQ(impostor.confirm("200%20OK", token).status, 404);

  // Nor may the agent's *other* CloudPRNT printer take it, with or without a MAC that
  // belongs to somebody else.
  ScriptedPrinter other = fixture.printer("/cloudprnt/bar", kBarMac);
  CHECK_EQ(other.download(kStarLine, token).status, 404);
  CHECK_EQ(other.confirm("200%20OK", token).status, 404);
  ScriptedPrinter borrowed = fixture.printer("/cloudprnt/bar", kCounterMac);
  CHECK_EQ(borrowed.download(kStarLine, token).status, 404);

  // Nothing above touched the job: the rightful printer still has it, intact.
  CHECK_EQ(counter.download(kStarLine, token).status, 200);
  CHECK_EQ(counter.download(kStarLine, token).body, asString(receiptBytes()));
  CHECK(!flag(fixture.jobDoc(token), "terminal"));

  // The refusals are counted rather than swallowed — the CloudPRNT shape of the
  // single-owner violation (docs/sdk-spec.md §14).
  const Json listed = parse(fixture.get("/cloudprnt").body);
  const Json* printers = listed.find("cloudprnt");
  CHECK(printers != nullptr && printers->isArray());
  if (printers != nullptr && printers->size() == 2) {
    CHECK(number(printers->asArray()[0], "identityRefusals") >= 3LL);
  }
}

PD_TEST(a_repeated_confirmation_is_idempotent) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::string token = fixture.submit(receiptBytes());
  ScriptedPrinter printer = fixture.printer();
  CHECK(printer.poll().job_ready);
  CHECK_EQ(printer.download().status, 200);

  const HttpClientResult first = printer.confirm("200%20OK", token);
  CHECK_EQ(first.status, 200);
  CHECK(!flag(parse(first.body), "idempotent"));

  // §2 documents the confirmation as retried, `retry=x` and all. A printer that repeats a
  // DELETE it already delivered is behaving correctly; 404 here would make it retry
  // forever, and a second code must not overwrite the one that was evidence.
  const HttpClientResult again = printer.confirm("410", token, "1");
  CHECK_EQ(again.status, 200);
  const Json repeated = parse(again.body);
  CHECK(flag(repeated, "ok"));
  CHECK(flag(repeated, "idempotent"));
  CHECK_EQ(number(repeated, "code"), 200LL);
  CHECK_EQ(field(repeated, "outcome"), std::string("Done"));

  const Json evidence = fixture.jobDoc(token);
  CHECK_EQ(field(evidence, "outcome"), std::string("Done"));
  CHECK_EQ(number(evidence, "code"), 200LL);
  CHECK_EQ(field(evidence, "grade"), std::string("A"));
}

PD_TEST(an_unknown_token_is_404_and_an_unsatisfiable_media_type_is_415) {
  Fixture fixture;
  CHECK(fixture.start());
  ScriptedPrinter printer = fixture.printer();

  // Nothing queued at all.
  CHECK_EQ(printer.download(kStarLine, "cp-never-issued").status, 404);
  CHECK_EQ(printer.download().status, 404);

  const std::string token = fixture.submit(receiptBytes());
  CHECK(printer.poll().job_ready);
  CHECK_EQ(printer.download(kStarLine, "cp-never-issued").status, 404);
  // §2 lists 415 for the download: the printer asked for a type this job cannot be served
  // as. The job survives — asking for the wrong type consumes nothing.
  CHECK_EQ(printer.download("text/plain", token).status, 415);
  CHECK_EQ(printer.download("image/png", token).status, 415);
  CHECK_EQ(printer.download(kStarLine, token).status, 200);

  // A job submitted as text/plain is served as text/plain, and 415s for the other one:
  // the negotiated type is per job, not per printer.
  const std::string plain = fixture.submit({'H', 'I', '\n'}, "text/plain");
  CHECK_EQ(printer.confirm("200%20OK", token).status, 200);
  const PollAnswer next = printer.poll();
  CHECK(next.job_ready);
  CHECK_EQ(next.token, plain);
  CHECK_EQ(next.media_types.size(), static_cast<size_t>(1));
  if (!next.media_types.empty()) {
    CHECK_EQ(next.media_types[0], std::string("text/plain"));
  }
  const RawResponse served = printer.download("text/plain", plain);
  CHECK_EQ(served.status, 200);
  CHECK(headerSays(served, "content-type: text/plain"));
  CHECK_EQ(printer.download(kStarLine, plain).status, 415);

  // A media type the printer never advertised cannot be queued at all: the poll answer is
  // a contract, and a job outside it would 415 forever.
  const HttpClientResult refused =
      fixture.post("/cloudprnt/counter/jobs",
                   R"({"mediaType":"image/png","text":"x"})");
  CHECK_EQ(refused.status, 400);
  CHECK_EQ(fixture.get("/cloudprnt/counter/jobs/cp-never-issued").status, 404);
  CHECK_EQ(fixture.post("/cloudprnt/nope/jobs", R"({"text":"x"})").status, 404);
}

PD_TEST(bytes_are_handed_over_raw_or_as_json_and_reach_the_printer_unchanged) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::vector<uint8_t> bytes = receiptBytes();

  // The raw shape: an already-encoded document named by its Content-Type. Nothing in this
  // process renders for a puller, so this is the natural way to hand one over.
  const HttpClientResult raw =
      pd::agent::httpRequest("127.0.0.1", fixture.port(), "POST",
                             "/cloudprnt/counter/jobs", asString(bytes), kStarLine);
  CHECK_EQ(raw.status, 201);
  const Json queued = parse(raw.body);
  CHECK_EQ(field(queued, "mediaType"), std::string(kStarLine));
  CHECK_EQ(number(queued, "bytes"), static_cast<long long>(bytes.size()));

  ScriptedPrinter printer = fixture.printer();
  CHECK_EQ(printer.poll().token, field(queued, "token"));
  const RawResponse downloaded = printer.download();
  CHECK_EQ(downloaded.status, 200);
  CHECK_EQ(downloaded.body, asString(bytes));
  CHECK_EQ(printer.confirm("200%20OK").status, 200);

  // The JSON shape, with text rather than base64, and the queue is strictly FIFO: one job
  // at a time, and never the next one while a confirmation is still owed.
  CHECK_EQ(fixture.post("/cloudprnt/counter/jobs",
                        R"({"mediaType":"text/plain","text":"HELLO\n"})")
               .status,
           201);
  const std::string second = fixture.submit(bytes);
  const PollAnswer offered = printer.poll();
  CHECK(offered.job_ready);
  CHECK(offered.token != second);  // the text job was queued first
  CHECK_EQ(printer.download("text/plain", offered.token).body, std::string("HELLO\n"));

  CHECK_EQ(fixture.post("/cloudprnt/counter/jobs", R"({"mediaType":"text/plain"})").status,
           400);
  CHECK_EQ(fixture.post("/cloudprnt/counter/jobs", "{not json").status, 400);
  CHECK_EQ(fixture.post("/cloudprnt/counter/jobs",
                        R"({"mediaType":"text/plain","base64":"not base64!!"})")
               .status,
           400);
}

PD_TEST(poll_status_codes_surface_as_device_events_and_recorded_conditions) {
  Fixture fixture;
  CHECK(fixture.start());
  ScriptedPrinter printer = fixture.printer();

  printer.poll("200%20OK");   // first contact: it is talking to us
  printer.poll("211");        // paper low
  printer.poll("211");        // unchanged: not an event
  printer.poll("420");        // cover open
  printer.poll("200%20OK");   // recovered
  printer.poll("230");        // cleaning: no member in the closed enum

  const std::vector<pd::DeviceEvent> events = fixture.eventsFor("counter");
  const std::vector<pd::DeviceEvent> expected{
      pd::DeviceEvent::Online, pd::DeviceEvent::PaperNearEnd, pd::DeviceEvent::CoverOpen,
      pd::DeviceEvent::CoverClosed};
  CHECK_EQ(events.size(), expected.size());
  for (size_t i = 0; i < events.size() && i < expected.size(); ++i) {
    CHECK_EQ(events[i], expected[i]);
  }

  // 230 has no pd::DeviceEvent member — the enum is closed and mirrored into four
  // wrappers — so it is recorded verbatim instead of being bent onto a neighbour.
  const Json entry = parse(fixture.get("/printers/counter").body);
  const Json* conditions = entry.find("conditions");
  CHECK(conditions != nullptr && conditions->isArray());
  bool recorded = false;
  if (conditions != nullptr) {
    for (const Json& condition : conditions->asArray()) {
      recorded = recorded || condition.asString() == "230 cleaning";
    }
  }
  CHECK(recorded);
  CHECK_EQ(field(entry, "statusMeaning"), std::string("cleaning"));
  CHECK(!flag(entry, "refreshSupported"));

  // The device snapshot is the same three-state shape the pushed printers report, and a
  // "cleaning" code asserts nothing about paper — so paper stays unknown, not healthy.
  const Json* status = entry.find("status");
  CHECK(status != nullptr && status->isObject());
  if (status != nullptr) {
    CHECK(flag(*status, "connected"));
    CHECK(flag(*status, "observed"));
    CHECK(status->find("paperOut") != nullptr && status->find("paperOut")->isNull());
  }

  // A fault code puts the flag up, in the same field name the ASB path uses.
  printer.poll("410");
  const Json out_of_paper = parse(fixture.get("/printers/counter").body);
  const Json* paper = out_of_paper.find("status");
  CHECK(paper != nullptr);
  if (paper != nullptr) {
    CHECK(flag(*paper, "paperOut"));
  }
  const std::vector<pd::DeviceEvent> after = fixture.eventsFor("counter");
  CHECK_EQ(after.size(), expected.size() + 1);
  if (!after.empty()) {
    CHECK_EQ(after.back(), pd::DeviceEvent::PaperOut);
  }
}

PD_TEST(a_poll_alone_never_completes_a_job) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::string token = fixture.submit(receiptBytes());
  ScriptedPrinter printer = fixture.printer();

  // Six polls and a download, all reporting "200 OK", and still nothing is Done: the
  // printer saying it is healthy is not the printer saying a receipt exists.
  for (int i = 0; i < 6; ++i) {
    CHECK(printer.poll().job_ready);
  }
  const Json queued = fixture.jobDoc(token);
  CHECK_EQ(field(queued, "state"), std::string("queued"));
  CHECK(!flag(queued, "terminal"));
  CHECK(queued.find("outcome") == nullptr);
  CHECK_EQ(field(queued, "grade"), std::string("E"));
  CHECK_EQ(field(queued, "authority"), std::string("TransportOnly"));
  CHECK_EQ(field(queued, "method"), std::string("none"));

  CHECK_EQ(printer.download().status, 200);
  const Json delivered = fixture.jobDoc(token);
  CHECK_EQ(field(delivered, "state"), std::string("delivered"));
  CHECK(delivered.find("outcome") == nullptr);

  // A confirmation with no readable code retires the job and claims nothing.
  const HttpClientResult bare = pd::agent::httpRequest(
      "127.0.0.1", fixture.port(), "DELETE",
      "/cloudprnt/counter?uid=star-tsp143-01&mac=" + urlEncode(kCounterMac) +
          "&token=" + urlEncode(token));
  CHECK_EQ(bare.status, 200);
  const Json unresolved = fixture.jobDoc(token);
  CHECK_EQ(field(unresolved, "outcome"), std::string("Unknown"));
  CHECK_EQ(field(unresolved, "grade"), std::string("E"));
  CHECK(flag(unresolved, "terminal"));
}

PD_TEST(cloudprnt_printers_are_configured_from_json_and_reported) {
  AgentConfig config;
  std::string error;
  const char* source =
      R"({"bind":"127.0.0.1","port":0,)"
      R"("cloudprnt":[{"id":"counter","mac":"00:11:62:aa:bb:cc",)"
      R"("mediaTypes":["application/vnd.star.line"],"maxPendingJobs":4},)"
      R"({"id":"bar"}]})";
  CHECK(pd::agent::parseAgentConfig(pd::dsl::parseJson(source), &config, &error));
  CHECK_EQ(config.cloudprnt.size(), static_cast<size_t>(2));
  if (config.cloudprnt.size() == 2) {
    CHECK_EQ(config.cloudprnt[0].id, std::string("counter"));
    CHECK_EQ(config.cloudprnt[0].mac, std::string("00:11:62:aa:bb:cc"));
    CHECK_EQ(config.cloudprnt[0].max_pending, static_cast<size_t>(4));
    CHECK_EQ(config.cloudprnt[1].media_types.size(), static_cast<size_t>(0));
  }
  AgentConfig rejected;
  // The id is the URL segment the printer polls; there is nothing to derive it from.
  CHECK(!pd::agent::parseAgentConfig(pd::dsl::parseJson(R"({"cloudprnt":[{"mac":"x"}]})"),
                                     &rejected, &error));
  CHECK(!pd::agent::parseAgentConfig(pd::dsl::parseJson(R"({"cloudprnt":{"id":"a"}})"),
                                     &rejected, &error));

  Fixture fixture;
  CHECK(fixture.start());
  fixture.submit(receiptBytes());

  // They belong in the printer list an operator reads, marked for what they are: no
  // endpoint to dial, and a grade that only applies once a confirmation has landed.
  const Json listed = parse(fixture.get("/printers").body);
  const Json* printers = listed.find("printers");
  CHECK(printers != nullptr && printers->isArray());
  CHECK_EQ(printers->size(), static_cast<size_t>(2));
  if (printers != nullptr && printers->size() == 2) {
    const Json& entry = printers->asArray()[0];
    CHECK_EQ(field(entry, "id"), std::string("counter"));
    CHECK_EQ(field(entry, "kind"), std::string("cloudprnt"));
    CHECK_EQ(field(entry, "completion"), std::string("CloudPRNT"));
    CHECK_EQ(field(entry, "grade"), std::string("A"));
    CHECK_EQ(field(entry, "gradeAppliesWhen"), std::string("confirmed"));
    CHECK_EQ(number(entry, "pending"), 1LL);
    CHECK(entry.find("status") != nullptr);
  }

  const Json health = parse(fixture.get("/healthz").body);
  CHECK_EQ(number(health, "printers"), 0LL);  // no pushed lane: the printers dial us
  CHECK_EQ(number(health, "cloudprntPrinters"), 2LL);
  CHECK_EQ(number(health, "cloudprntAwaitingConfirmation"), 1LL);

  // Routing: one URL and three verbs for the printer, and nothing else answered.
  CHECK_EQ(fixture.get("/cloudprnt").status, 200);
  CHECK_EQ(fixture.get("/cloudprnt/nope").status, 404);
  CHECK_EQ(fixture.post("/cloudprnt", "{}").status, 405);
  CHECK_EQ(fixture.post("/cloudprnt/counter", "not json").status, 400);
  CHECK_EQ(fixture.get("/cloudprnt/counter/jobs").status, 200);
  CHECK_EQ(fixture.post("/cloudprnt/counter/jobs/x", "{}").status, 405);
}
