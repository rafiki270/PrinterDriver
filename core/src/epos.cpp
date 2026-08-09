#include "printerdriver/epos.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

#include "printerdriver/net_platform.hpp"

namespace pd::epos {
namespace {

// The one namespace that identifies an ePOS-Print payload. Everything the parser accepts
// is matched against this URI plus a local name; nothing is matched against a prefix.
constexpr char kEposNamespace[] = "http://www.epson-pos.com/schemas/2011/03/epos-print";
constexpr char kSoapNamespace[] = "http://schemas.xmlsoap.org/soap/envelope/";

// The service caps the print timeout here (ePOS-Print XML User's Manual rev. AC).
constexpr uint32_t kMaxServiceTimeoutMs = 300000u;

bool isSpace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

// --- A very small namespace-aware XML scanner ----------------------------------------
//
// Enough to find one element and read its attributes, and no more. Writing it by hand
// rather than reaching for a parser is the same decision the rest of this core makes: the
// SDK ships with no dependencies so that it builds inside iOS, Android, Flutter and
// Raspberry Pi toolchains with no package manager anywhere in the loop.
//
// What it does implement is the part that actually matters here: **prefixes are resolved
// through xmlns declarations, scoped to the element that declared them**. `s:Envelope`,
// `soap:Envelope` and a default-namespaced `Envelope` are the same element, and only a
// resolver can see that.

struct Attribute {
  std::string prefix;
  std::string local;
  std::string value;
};

struct StartTag {
  std::string prefix;
  std::string local;
  std::vector<Attribute> attributes;
  bool self_closing = false;
};

std::string decodeEntities(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] != '&') {
      out += text[i++];
      continue;
    }
    const size_t end = text.find(';', i);
    if (end == std::string_view::npos) {
      out += text[i++];
      continue;
    }
    const std::string_view name = text.substr(i + 1, end - i - 1);
    if (name == "amp") {
      out += '&';
    } else if (name == "lt") {
      out += '<';
    } else if (name == "gt") {
      out += '>';
    } else if (name == "quot") {
      out += '"';
    } else if (name == "apos") {
      out += '\'';
    } else if (!name.empty() && name[0] == '#') {
      // Numeric character references are decoded only in the Latin-1 range: the values
      // that appear in these documents are punctuation, and a full UTF-8 encoder here
      // would be code with no caller.
      const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
      const std::string digits(name.substr(hex ? 2 : 1));
      const unsigned long value = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
      if (value > 0 && value < 0x80) {
        out += static_cast<char>(value);
      } else {
        out += '?';
      }
    } else {
      out.append(text.substr(i, end - i + 1));
    }
    i = end + 1;
  }
  return out;
}

void splitQName(std::string_view qname, std::string* prefix, std::string* local) {
  const size_t colon = qname.find(':');
  if (colon == std::string_view::npos) {
    prefix->clear();
    local->assign(qname);
    return;
  }
  prefix->assign(qname.substr(0, colon));
  local->assign(qname.substr(colon + 1));
}

// Parses the inside of a `<...>`, i.e. `name attr="value" ...` with an optional trailing
// `/`. Returns false for anything that is not a start or empty element.
bool parseStartTag(std::string_view inside, StartTag* out) {
  size_t i = 0;
  while (i < inside.size() && isSpace(inside[i])) {
    ++i;
  }
  const size_t name_begin = i;
  while (i < inside.size() && !isSpace(inside[i]) && inside[i] != '/' && inside[i] != '>') {
    ++i;
  }
  if (i == name_begin) {
    return false;
  }
  splitQName(inside.substr(name_begin, i - name_begin), &out->prefix, &out->local);

  for (;;) {
    while (i < inside.size() && isSpace(inside[i])) {
      ++i;
    }
    if (i >= inside.size()) {
      break;
    }
    if (inside[i] == '/') {
      out->self_closing = true;
      break;
    }
    const size_t attr_begin = i;
    while (i < inside.size() && inside[i] != '=' && !isSpace(inside[i]) &&
           inside[i] != '/') {
      ++i;
    }
    if (i >= inside.size() || inside[i] != '=') {
      // An attribute with no value is not valid XML here; skip it rather than abandoning
      // a document that is otherwise readable.
      continue;
    }
    Attribute attribute;
    splitQName(inside.substr(attr_begin, i - attr_begin), &attribute.prefix,
               &attribute.local);
    ++i;  // '='
    while (i < inside.size() && isSpace(inside[i])) {
      ++i;
    }
    if (i >= inside.size() || (inside[i] != '"' && inside[i] != '\'')) {
      continue;
    }
    const char quote = inside[i++];
    const size_t value_begin = i;
    while (i < inside.size() && inside[i] != quote) {
      ++i;
    }
    attribute.value = decodeEntities(inside.substr(value_begin, i - value_begin));
    if (i < inside.size()) {
      ++i;  // closing quote
    }
    out->attributes.push_back(std::move(attribute));
  }
  return true;
}

struct NamespaceScope {
  std::string element;                                       // for popping
  std::vector<std::pair<std::string, std::string>> bindings;  // prefix ("" = default) -> uri
};

std::string resolve(const std::vector<NamespaceScope>& scopes, const std::string& prefix) {
  for (size_t i = scopes.size(); i > 0; --i) {
    for (const auto& binding : scopes[i - 1].bindings) {
      if (binding.first == prefix) {
        return binding.second;
      }
    }
  }
  return std::string();
}

uint32_t parseUnsigned32(const std::string& text) {
  // strtoull, then a narrowing cast that is intentional: the field is documented as a
  // 32-bit unsigned mask and 0x80000000 is a value it actually takes. Anything that
  // routes this through a signed type reports spooler-stopped as a negative number.
  const unsigned long long value = std::strtoull(text.c_str(), nullptr, 10);
  return static_cast<uint32_t>(value & 0xFFFFFFFFull);
}

const char* alignmentName(escpos::Alignment alignment) noexcept {
  switch (alignment) {
    case escpos::Alignment::Left: return "left";
    case escpos::Alignment::Center: return "center";
    case escpos::Alignment::Right: return "right";
  }
  return "left";
}

// --- Evidence ---------------------------------------------------------------------------

JobEvidence spooledEvidence() noexcept {
  // The retrieved result of a durable, queryable printer-side job: the top of
  // docs/compatibility-brief.md §24. The authority is the vendor's spooler, not the
  // mechanism that moved the paper, and saying so is the point of carrying the two
  // separately — this is a very strong claim made by something that is still not the
  // print head.
  return JobEvidence{ConfidenceGrade::APlus_DurableQueryableJob,
                     CompletionAuthority::VendorSpooler, "ePOS JobID"};
}

JobEvidence directEvidence() noexcept {
  // No spooler: the HTTP response did not come back until the data had printed. That is
  // explicit device completion, but it is not durable and not queryable — lose the
  // connection and the answer is gone — so it is grade A, not A+.
  return JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                     CompletionAuthority::PhysicalPrinter, "ePOS-Print response"};
}

JobEvidence faultEvidence() noexcept {
  return JobEvidence{ConfidenceGrade::C_DeviceStatusAround,
                     CompletionAuthority::VendorSpooler, "ePOS status"};
}

JobEvidence noEvidence() noexcept {
  return JobEvidence{ConfidenceGrade::E_TransportOnly, CompletionAuthority::TransportOnly,
                     "transport-only"};
}

FailureReason reasonFor(ResponseCode code) noexcept {
  switch (code) {
    case ResponseCode::EPTR_COVER_OPEN:
      return FailureReason::PreflightCoverOpen;
    case ResponseCode::EPTR_REC_EMPTY:
      return FailureReason::PreflightPaperOut;
    case ResponseCode::EPTR_CUTTER:
      return FailureReason::CutterFault;
    case ResponseCode::EPTR_AUTOMATICAL:
    case ResponseCode::EPTR_BATTERY_LOW:
    case ResponseCode::EPTR_MECHANICAL:
    case ResponseCode::EPTR_UNRECOVERABLE:
    case ResponseCode::ERROR_WAIT_EJECT:
      return FailureReason::PreflightHardwareError;
    case ResponseCode::EX_SPOOLER:
    case ResponseCode::TooManyRequests:
      // The queue is full or the service is shedding load. Nothing was printed and
      // nothing is pending, which is exactly the addon's QueueOverflow (docs/sdk-spec.md
      // §12) rather than a device fault.
      return FailureReason::QueueOverflow;
    case ResponseCode::SchemaError:
    case ResponseCode::RequestEntityTooLarge:
      // We sent something this service will not accept. Retrying it unchanged cannot
      // help, which is what Unsupported means everywhere else in this SDK.
      return FailureReason::Unsupported;
    case ResponseCode::DeviceNotFound:
    case ResponseCode::EX_BADPORT:
      return FailureReason::TransportUnreachable;
    case ResponseCode::EX_TIMEOUT:
      return FailureReason::TimeoutAwaitingCompletion;
    case ResponseCode::PrintSystemError:
    case ResponseCode::JobNotFound:
    case ResponseCode::Printing:
    case ResponseCode::JobSpooling:
    case ResponseCode::Unrecognised:
    case ResponseCode::None:
      return FailureReason::Unknown;
  }
  return FailureReason::Unknown;
}

}  // namespace

// --- Response codes ---------------------------------------------------------------------

const char* to_string(ResponseCode code) noexcept {
  switch (code) {
    case ResponseCode::None: return "";
    case ResponseCode::EPTR_AUTOMATICAL: return "EPTR_AUTOMATICAL";
    case ResponseCode::EPTR_BATTERY_LOW: return "EPTR_BATTERY_LOW";
    case ResponseCode::EPTR_COVER_OPEN: return "EPTR_COVER_OPEN";
    case ResponseCode::EPTR_CUTTER: return "EPTR_CUTTER";
    case ResponseCode::EPTR_MECHANICAL: return "EPTR_MECHANICAL";
    case ResponseCode::EPTR_REC_EMPTY: return "EPTR_REC_EMPTY";
    case ResponseCode::EPTR_UNRECOVERABLE: return "EPTR_UNRECOVERABLE";
    case ResponseCode::ERROR_WAIT_EJECT: return "ERROR_WAIT_EJECT";
    case ResponseCode::SchemaError: return "SchemaError";
    case ResponseCode::DeviceNotFound: return "DeviceNotFound";
    case ResponseCode::PrintSystemError: return "PrintSystemError";
    case ResponseCode::EX_BADPORT: return "EX_BADPORT";
    case ResponseCode::EX_TIMEOUT: return "EX_TIMEOUT";
    case ResponseCode::EX_SPOOLER: return "EX_SPOOLER";
    case ResponseCode::TooManyRequests: return "TooManyRequests";
    case ResponseCode::RequestEntityTooLarge: return "RequestEntityTooLarge";
    case ResponseCode::JobNotFound: return "JobNotFound";
    case ResponseCode::Printing: return "Printing";
    case ResponseCode::JobSpooling: return "JobSpooling";
    case ResponseCode::Unrecognised: return "Unrecognised";
  }
  return "Unrecognised";
}

ResponseCode responseCodeFrom(std::string_view text) noexcept {
  if (text.empty()) {
    return ResponseCode::None;
  }
  static const ResponseCode kCodes[] = {
      ResponseCode::EPTR_AUTOMATICAL,      ResponseCode::EPTR_BATTERY_LOW,
      ResponseCode::EPTR_COVER_OPEN,       ResponseCode::EPTR_CUTTER,
      ResponseCode::EPTR_MECHANICAL,       ResponseCode::EPTR_REC_EMPTY,
      ResponseCode::EPTR_UNRECOVERABLE,    ResponseCode::ERROR_WAIT_EJECT,
      ResponseCode::SchemaError,           ResponseCode::DeviceNotFound,
      ResponseCode::PrintSystemError,      ResponseCode::EX_BADPORT,
      ResponseCode::EX_TIMEOUT,            ResponseCode::EX_SPOOLER,
      ResponseCode::TooManyRequests,       ResponseCode::RequestEntityTooLarge,
      ResponseCode::JobNotFound,           ResponseCode::Printing,
      ResponseCode::JobSpooling,
  };
  for (const ResponseCode code : kCodes) {
    if (text == to_string(code)) {
      return code;
    }
  }
  return ResponseCode::Unrecognised;
}

bool isNonTerminal(ResponseCode code) noexcept {
  return code == ResponseCode::Printing || code == ResponseCode::JobSpooling;
}

// --- Status mask ---------------------------------------------------------------------

std::vector<DeviceEvent> toDeviceEvents(uint32_t value) {
  std::vector<DeviceEvent> events;
  const auto add = [&events](DeviceEvent event) {
    if (std::find(events.begin(), events.end(), event) == events.end()) {
      events.push_back(event);
    }
  };
  if ((value & status::kOffline) != 0) {
    add(DeviceEvent::Offline);
  } else if ((value & status::kNoResponse) == 0) {
    add(DeviceEvent::Online);
  }
  if ((value & status::kCoverOpen) != 0) {
    add(DeviceEvent::CoverOpen);
  }
  if ((value & status::kPaperEnd) != 0) {
    add(DeviceEvent::PaperOut);
  } else if ((value & status::kRollNearEnd) != 0) {
    add(DeviceEvent::PaperNearEnd);
  }
  if ((value & status::kCutterError) != 0) {
    add(DeviceEvent::CutterError);
  }
  if ((value & status::kUnrecoverableError) != 0 ||
      (value & status::kMechanicalError) != 0) {
    add(DeviceEvent::UnrecoverableError);
  }
  if ((value & status::kAutoRecoveryError) != 0 ||
      (value & status::kWaitingOnlineRecovery) != 0) {
    add(DeviceEvent::RecoverableError);
  }
  return events;
}

// --- Parsing -----------------------------------------------------------------------------

bool parseResponse(std::string_view xml, Response* out) {
  if (out == nullptr) {
    return false;
  }
  *out = Response{};

  std::vector<NamespaceScope> scopes;
  size_t i = 0;
  while (i < xml.size()) {
    const size_t open = xml.find('<', i);
    if (open == std::string_view::npos) {
      break;
    }
    if (xml.compare(open, 4, "<!--") == 0) {
      const size_t end = xml.find("-->", open + 4);
      i = end == std::string_view::npos ? xml.size() : end + 3;
      continue;
    }
    if (xml.compare(open, 2, "<?") == 0 || xml.compare(open, 2, "<!") == 0) {
      const size_t end = xml.find('>', open + 2);
      i = end == std::string_view::npos ? xml.size() : end + 1;
      continue;
    }
    const size_t close = xml.find('>', open + 1);
    if (close == std::string_view::npos) {
      break;
    }
    const std::string_view inside = xml.substr(open + 1, close - open - 1);
    i = close + 1;

    if (!inside.empty() && inside[0] == '/') {
      std::string prefix;
      std::string local;
      splitQName(inside.substr(1), &prefix, &local);
      if (!scopes.empty() && scopes.back().element == local) {
        scopes.pop_back();
      }
      continue;
    }

    StartTag tag;
    if (!parseStartTag(inside, &tag)) {
      continue;
    }

    NamespaceScope scope;
    scope.element = tag.local;
    for (const Attribute& attribute : tag.attributes) {
      if (attribute.prefix.empty() && attribute.local == "xmlns") {
        scope.bindings.emplace_back(std::string(), attribute.value);
      } else if (attribute.prefix == "xmlns") {
        scope.bindings.emplace_back(attribute.local, attribute.value);
      }
    }
    scopes.push_back(scope);

    // The whole point: match on the resolved namespace URI and the local name. The
    // prefix that appears in the document is the sender's private choice and carries no
    // meaning of its own.
    const std::string uri = resolve(scopes, tag.prefix);
    if (tag.local == "response" && uri == kEposNamespace) {
      out->parsed = true;
      for (const Attribute& attribute : tag.attributes) {
        if (!attribute.prefix.empty()) {
          continue;  // namespaced attributes are not part of this contract
        }
        const std::string value = attribute.value;
        if (attribute.local == "success") {
          const std::string normalised = lower(value);
          out->success = normalised == "true" || normalised == "1";
        } else if (attribute.local == "code") {
          out->code_text = value;
          out->code = responseCodeFrom(value);
        } else if (attribute.local == "status") {
          out->status = parseUnsigned32(value);
        } else if (attribute.local == "battery") {
          out->battery = parseUnsigned32(value);
        } else if (attribute.local == "printjobid") {
          out->printjobid = value;
        }
      }
      return true;
    }
    if (tag.self_closing) {
      scopes.pop_back();
    }
  }
  return out->parsed;
}

// --- Requests -----------------------------------------------------------------------------

bool isValidJobId(std::string_view job_id) noexcept {
  if (job_id.empty() || job_id.size() > 30) {
    return false;
  }
  for (const char c : job_id) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                       (c >= 'A' && c <= 'Z');
    if (!alnum && c != '_' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

std::string escapeXml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      case '\n': out += "&#10;"; break;
      default: out += c; break;
    }
  }
  return out;
}

Document& Document::text(std::string_view utf8) {
  body_ += "<text>" + escapeXml(utf8) + "</text>";
  return *this;
}

Document& Document::line(std::string_view utf8) {
  body_ += "<text>" + escapeXml(utf8) + "&#10;</text>";
  return *this;
}

Document& Document::align(escpos::Alignment alignment) {
  body_ += std::string("<text align=\"") + alignmentName(alignment) + "\"/>";
  return *this;
}

Document& Document::bold(bool enabled) {
  body_ += std::string("<text em=\"") + (enabled ? "true" : "false") + "\"/>";
  return *this;
}

Document& Document::feed(uint32_t lines) {
  if (lines == 0) {
    return *this;
  }
  body_ += "<feed line=\"" + std::to_string(lines) + "\"/>";
  return *this;
}

Document& Document::cut(bool full) {
  // `feed` is the cut-with-feed variant, which is what a receipt wants: the head is ahead
  // of the blade, so a cut without feed slices the last printed line.
  body_ += std::string("<cut type=\"") + (full ? "feed" : "feed") + "\"/>";
  return *this;
}

Document& Document::raw(std::string_view xml) {
  body_.append(xml);
  return *this;
}

std::string buildPrintEnvelope(const std::string& device_id, uint32_t timeout_ms,
                               const std::string& job_id, const std::string& document) {
  const uint32_t timeout = std::min(timeout_ms, kMaxServiceTimeoutMs);
  std::string out;
  out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
  out += "<s:Envelope xmlns:s=\"";
  out += kSoapNamespace;
  out += "\"><s:Header><parameter xmlns=\"";
  out += kEposNamespace;
  out += "\">";
  out += "<devid>" + escapeXml(device_id) + "</devid>";
  out += "<timeout>" + std::to_string(timeout) + "</timeout>";
  if (!job_id.empty()) {
    out += "<printjobid>" + escapeXml(job_id) + "</printjobid>";
  }
  out += "</parameter></s:Header><s:Body><epos-print xmlns=\"";
  out += kEposNamespace;
  out += "\">";
  out += document;
  out += "</epos-print></s:Body></s:Envelope>";
  return out;
}

std::string buildPollEnvelope(const std::string& job_id) {
  // Header carries **only** printjobid, body carries an **empty** epos-print. That
  // emptiness is the whole retrieval verb: there is no separate query operation, and
  // sending a document here would print the receipt a second time.
  std::string out;
  out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
  out += "<s:Envelope xmlns:s=\"";
  out += kSoapNamespace;
  out += "\"><s:Header><parameter xmlns=\"";
  out += kEposNamespace;
  out += "\"><printjobid>" + escapeXml(job_id) + "</printjobid></parameter></s:Header>";
  out += "<s:Body><epos-print xmlns=\"";
  out += kEposNamespace;
  out += "\"/></s:Body></s:Envelope>";
  return out;
}

// --- HTTP ---------------------------------------------------------------------------------

namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool waitFor(net::Socket socket, short events, int64_t deadline_ms) {
  const int64_t remaining = deadline_ms - nowMs();
  if (remaining <= 0) {
    return false;
  }
  net::PollFd waiter;
  waiter.socket = socket;
  waiter.events = events;
  return net::poll(&waiter, 1, static_cast<int>(remaining)) > 0;
}

HttpReply socketPost(const ClientConfig& config, const std::string& path,
                     const std::string& body, uint32_t timeout_ms) {
  HttpReply reply;
  if (!net::startup()) {
    reply.error = "socket layer unavailable";
    return reply;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(config.port);
  if (::getaddrinfo(config.host.c_str(), port_text.c_str(), &hints, &resolved) != 0 ||
      resolved == nullptr) {
    reply.error = "cannot resolve " + config.host;
    return reply;
  }

  net::Socket sock = net::invalidSocket();
  const int64_t connect_deadline = nowMs() + static_cast<int64_t>(config.connect_timeout_ms);
  for (addrinfo* candidate = resolved; candidate != nullptr;
       candidate = candidate->ai_next) {
    sock = net::create(candidate->ai_family, candidate->ai_socktype,
                       candidate->ai_protocol);
    if (!net::valid(sock)) {
      continue;
    }
    net::setNonBlocking(sock);
    const int result = ::connect(sock, candidate->ai_addr,
                                 static_cast<net::SockLen>(candidate->ai_addrlen));
    if (result == 0) {
      break;
    }
    if (net::inProgress(net::lastError()) &&
        waitFor(sock, net::kPollOut, connect_deadline)) {
      int pending = 0;
      if (net::pendingError(sock, &pending) && pending == 0) {
        break;
      }
    }
    net::closeSocket(sock);
    sock = net::invalidSocket();
  }
  ::freeaddrinfo(resolved);
  if (!net::valid(sock)) {
    reply.error = "cannot connect to " + config.host + ":" + port_text;
    return reply;
  }

  // The documented request shape (docs/wire-protocols.md §1). `SOAPAction: ""` and the
  // epoch `If-Modified-Since` are both required by the service; the second exists to stop
  // an intermediate cache from answering a print request out of its store, which is a
  // failure mode with paper consequences. No Authorization header, ever — see the note at
  // the top of epos.hpp.
  std::string request;
  request += "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + config.host + ":" + port_text + "\r\n";
  request += "Content-Type: text/xml; charset=utf-8\r\n";
  request += "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n";
  request += "SOAPAction: \"\"\r\n";
  request += "Connection: close\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  request += body;

  const int64_t deadline = nowMs() + static_cast<int64_t>(timeout_ms);
  size_t sent = 0;
  while (sent < request.size()) {
    if (!waitFor(sock, net::kPollOut, deadline)) {
      net::closeSocket(sock);
      reply.error = "timed out sending the ePOS request";
      return reply;
    }
    const int64_t wrote =
        net::sendSome(sock, request.data() + sent, request.size() - sent);
    if (wrote > 0) {
      sent += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && net::wouldBlock(net::lastError())) {
      continue;
    }
    net::closeSocket(sock);
    reply.error = "the ePOS connection closed while sending";
    return reply;
  }

  std::string raw;
  char buffer[4096];
  for (;;) {
    if (!waitFor(sock, net::kPollIn, deadline)) {
      net::closeSocket(sock);
      reply.error = "timed out waiting for the ePOS response";
      return reply;
    }
    const int64_t got = net::recvSome(sock, buffer, sizeof(buffer));
    if (got > 0) {
      raw.append(buffer, static_cast<size_t>(got));
      // Stop as soon as a Content-Length body is complete; otherwise read to close.
      const size_t header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const std::string headers = lower(raw.substr(0, header_end));
        const size_t marker = headers.find("content-length:");
        if (marker != std::string::npos) {
          const size_t length =
              static_cast<size_t>(std::strtoull(headers.c_str() + marker + 15, nullptr, 10));
          if (raw.size() >= header_end + 4 + length) {
            break;
          }
        }
      }
      continue;
    }
    if (got == 0) {
      break;  // orderly close: the whole body has arrived
    }
    if (net::wouldBlock(net::lastError())) {
      continue;
    }
    break;
  }
  net::closeSocket(sock);

  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    reply.error = "malformed HTTP response from the ePOS service";
    return reply;
  }
  const size_t status_end = raw.find("\r\n");
  const std::string status_line = raw.substr(0, status_end);
  const size_t first_space = status_line.find(' ');
  if (first_space != std::string::npos) {
    reply.status = std::atoi(status_line.c_str() + first_space + 1);
  }
  reply.body = raw.substr(header_end + 4);
  reply.ok = true;
  return reply;
}

}  // namespace

// --- Client -------------------------------------------------------------------------------

Client::Client(ClientConfig config) : config_(std::move(config)) {}

void Client::setTransport(HttpPost post) { post_ = std::move(post); }

HttpReply Client::post(const std::string& body) {
  if (post_) {
    return post_(config_.path, body, config_.http_timeout_ms);
  }
  return socketPost(config_, config_.path, body, config_.http_timeout_ms);
}

Response Client::submit(const std::string& document, const std::string& job_id,
                        std::string* error) {
  Response parsed;
  if (!job_id.empty() && !isValidJobId(job_id)) {
    if (error != nullptr) {
      *error = "ePOS JobID must be 1-30 characters of alphanumerics, '_', '-' or '.'";
    }
    return parsed;
  }
  const HttpReply reply = post(
      buildPrintEnvelope(config_.device_id, config_.timeout_ms, job_id, document));
  if (!reply.ok) {
    if (error != nullptr) {
      *error = reply.error;
    }
    return parsed;
  }
  if (!parseResponse(reply.body, &parsed) && error != nullptr) {
    *error = "no ePOS-Print <response> in the reply (HTTP " +
             std::to_string(reply.status) + ")";
  }
  return parsed;
}

Response Client::retrieve(const std::string& job_id, std::string* error) {
  Response parsed;
  const HttpReply reply = post(buildPollEnvelope(job_id));
  if (!reply.ok) {
    if (error != nullptr) {
      *error = reply.error;
    }
    return parsed;
  }
  if (!parseResponse(reply.body, &parsed) && error != nullptr) {
    *error = "no ePOS-Print <response> in the reply (HTTP " +
             std::to_string(reply.status) + ")";
  }
  return parsed;
}

Outcome Client::classify(const Response& response, bool spooler, bool first) {
  Outcome out;
  out.submission = response;
  out.last = response;
  out.events = toDeviceEvents(response.status);

  if (!response.parsed) {
    // Something answered on the socket but it was not this service. Bytes may or may not
    // have reached the printer, so the honest verdict is Unknown rather than Failed.
    out.state = JobState::Unknown;
    out.result =
        JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                  FailureReason::Unknown}
            .with(noEvidence());
    return out;
  }

  if (isNonTerminal(response.code)) {
    // Printing / JobSpooling. The service is telling us to ask again, and there is no
    // information here about the paper beyond "not yet".
    out.terminal = false;
    out.accepted_not_printed = true;
    out.state = JobState::BytesSent;
    out.result = JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                           FailureReason::None};
    return out;
  }

  if (response.code == ResponseCode::JobNotFound) {
    // The spooler has no record of this JobID. Either it never took it, or the result
    // has aged out of the spooler's table. Both leave the fate of the paper genuinely
    // unknown, and the durability this transport is chosen for is exactly what has
    // failed, so nothing here may be reported as either printed or not printed.
    out.state = JobState::Unknown;
    out.result = JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                           FailureReason::Unknown}
                     .with(noEvidence());
    return out;
  }

  if (!response.success || response.code != ResponseCode::None) {
    out.state = JobState::FailedKnown;
    out.result = JobResult::failed(reasonFor(response.code),
                                   ConfidenceLevel::TransportAccepted)
                     .with(faultEvidence());
    return out;
  }

  // success = true, no code. What that means depends entirely on the spooler flag.
  if (spooler && first) {
    // THE DISCIPLINE. This is an enqueue acknowledgement. Its status field commonly reads
    // 0x00000002 ("printing completed") or 0x80000000 ("spooler stopped") *before the
    // paper has moved*, so neither bit is consulted here and nothing terminal is
    // reported. The job is accepted, not printed, and the only way forward is to retrieve
    // the result under its JobID.
    out.terminal = false;
    out.accepted_not_printed = true;
    out.state = JobState::BytesSent;
    out.result = JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                           FailureReason::None};
    return out;
  }

  if ((response.status & status::kCutterError) != 0) {
    out.state = JobState::FailedKnown;
    out.result = JobResult::failed(FailureReason::CutterFault,
                                   ConfidenceLevel::PrintConfirmed)
                     .with(faultEvidence());
    return out;
  }

  if (spooler) {
    if ((response.status & status::kPrintingCompleted) != 0) {
      out.state = JobState::DoneSoftware;
      out.result = JobResult::done(ConfidenceLevel::CutProcessed).with(spooledEvidence());
      return out;
    }
    if ((response.status & status::kSpoolerStopped) != 0) {
      // On a *retrieval* this bit is not the harmless initial value it is on the ack: the
      // queue this job is sitting in has been stopped, so it is not going to print until
      // an operator restarts it.
      out.state = JobState::FailedKnown;
      out.result = JobResult::failed(FailureReason::PreflightHardwareError,
                                     ConfidenceLevel::TransportAccepted)
                       .with(faultEvidence());
      return out;
    }
    // success with neither bit: still working. Keep retrieving.
    out.terminal = false;
    out.accepted_not_printed = true;
    out.state = JobState::BytesSent;
    out.result = JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                           FailureReason::None};
    return out;
  }

  // No spooler: the response did not come back until the data had printed.
  out.state = JobState::DoneSoftware;
  out.result = JobResult::done(ConfidenceLevel::CutProcessed).with(directEvidence());
  return out;
}

Outcome Client::print(const std::string& document, const std::string& job_id) {
  Outcome out;
  out.job_id = job_id;

  std::string error;
  const Response submission = submit(document, job_id, &error);
  if (!error.empty() && !submission.parsed) {
    out.error = error;
    out.state = JobState::FailedKnown;
    // Nothing was accepted anywhere: the request never reached a service that answered
    // it, which is the one ePOS failure that is provably not on paper.
    out.result = JobResult::failed(FailureReason::TransportUnreachable,
                                   ConfidenceLevel::TransportAccepted)
                     .with(noEvidence());
    return out;
  }

  out = classify(submission, config_.spooler, true);
  out.job_id = job_id;
  out.submission = submission;
  if (out.terminal) {
    return out;
  }

  // Retrieval. A spooled job with no JobID cannot be asked about, so the honest answer is
  // Unknown immediately rather than a poll loop against nothing.
  if (job_id.empty()) {
    out.terminal = true;
    out.state = JobState::Unknown;
    out.result = JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                           FailureReason::Unknown}
                     .with(noEvidence());
    out.error = "the spooler accepted the job without a JobID, so its result cannot be "
                "retrieved";
    return out;
  }

  const int64_t deadline = nowMs() + static_cast<int64_t>(config_.poll_budget_ms);
  for (;;) {
    if (nowMs() >= deadline) {
      out.terminal = true;
      out.state = JobState::Unknown;
      // Out of *our* patience, not the printer's. The job may well still print, so this
      // is Unknown with a timeout reason and never Failed.
      out.result = JobResult{JobOutcome::Unknown, ConfidenceLevel::TransportAccepted,
                             FailureReason::TimeoutAwaitingCompletion}
                       .with(noEvidence());
      return out;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.poll_interval_ms));

    std::string poll_error;
    const Response answer = retrieve(job_id, &poll_error);
    ++out.polls;
    if (!answer.parsed) {
      // A retrieval that did not answer is not a verdict. Keep asking until the budget
      // runs out — this is precisely the situation the durable JobID exists for.
      out.error = poll_error;
      continue;
    }
    Outcome step = classify(answer, config_.spooler, false);
    const uint32_t polls = out.polls;
    const Response submission_copy = out.submission;
    const bool accepted = out.accepted_not_printed;
    std::vector<DeviceEvent> events = std::move(out.events);
    events.insert(events.end(), step.events.begin(), step.events.end());
    out = std::move(step);
    out.polls = polls;
    out.submission = submission_copy;
    out.accepted_not_printed = accepted || out.accepted_not_printed;
    out.events = std::move(events);
    out.job_id = job_id;
    if (out.terminal) {
      return out;
    }
  }
}

const std::vector<std::string>& declaredDegradations() {
  static const std::vector<std::string> kDegradations = {
      "the document builder covers text, alignment, emphasis, feed and cut only; "
      "barcodes, symbols, images, logos, pages and drawer commands are not built here",
      "no authentication is attempted, because the print service documents none; "
      "WebConfig administrative credentials are never sent",
      "HTTP only: TLS is not implemented, so the service must be reached over a trusted "
      "segment exactly like the raw 9100 port it sits beside",
      "chunked transfer encoding in a response is not decoded; the service answers with "
      "Content-Length or closes the connection",
      "the older pre-WSDL query-string form (?devid=...&timeout=...) is not emitted; the "
      "SOAP header form is used, and the endpoint path itself stays configurable",
  };
  return kDegradations;
}

}  // namespace pd::epos
