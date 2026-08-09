#include "printerdriver/agent/http.hpp"

#include "printerdriver/net_platform.hpp"
#include "printerdriver/types.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pd::agent {
namespace {

net::Socket asSocket(std::uintptr_t handle) { return static_cast<net::Socket>(handle); }
std::uintptr_t asHandle(net::Socket socket) {
  return static_cast<std::uintptr_t>(socket);
}

std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string_view trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

int hexValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Reads until `until(buffer)` says the message is complete or the budget runs out.
bool readUntil(net::Socket socket, std::string* buffer, size_t limit,
               uint32_t timeout_ms,
               const std::function<bool(const std::string&)>& complete) {
  const MonotonicTime deadline =
      MonotonicClock::now() + std::chrono::milliseconds(timeout_ms);
  char chunk[4096];
  while (!complete(*buffer)) {
    if (buffer->size() > limit) {
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - MonotonicClock::now())
                               .count();
    if (remaining <= 0) {
      return false;
    }
    net::PollFd waiter;
    waiter.socket = socket;
    waiter.events = net::kPollIn;
    const int ready = net::poll(&waiter, 1, static_cast<int>(remaining));
    if (ready <= 0) {
      return false;
    }
    const int64_t got = net::recvSome(socket, chunk, sizeof(chunk));
    if (got <= 0) {
      return false;  // peer closed before the message was complete
    }
    buffer->append(chunk, static_cast<size_t>(got));
  }
  return true;
}

bool sendAll(net::Socket socket, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int64_t wrote = net::sendSome(socket, data.data() + sent, data.size() - sent);
    if (wrote <= 0) {
      if (wrote < 0 && net::wouldBlock(net::lastError())) {
        net::PollFd waiter;
        waiter.socket = socket;
        waiter.events = net::kPollOut;
        if (net::poll(&waiter, 1, 1000) > 0) {
          continue;
        }
      }
      return false;
    }
    sent += static_cast<size_t>(wrote);
  }
  return true;
}

size_t headerEnd(const std::string& buffer) {
  const size_t crlf = buffer.find("\r\n\r\n");
  if (crlf != std::string::npos) {
    return crlf + 4;
  }
  const size_t lf = buffer.find("\n\n");
  return lf == std::string::npos ? std::string::npos : lf + 2;
}

// Splits a header block into a request/status line plus name/value pairs.
bool parseHeaderBlock(std::string_view block, std::string* first_line,
                      std::vector<std::pair<std::string, std::string>>* headers) {
  size_t pos = block.find('\n');
  if (pos == std::string_view::npos) {
    return false;
  }
  *first_line = std::string(trim(block.substr(0, pos)));
  ++pos;
  while (pos < block.size()) {
    size_t line_end = block.find('\n', pos);
    if (line_end == std::string_view::npos) {
      line_end = block.size();
    }
    const std::string_view line = trim(block.substr(pos, line_end - pos));
    pos = line_end + 1;
    if (line.empty()) {
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      return false;
    }
    headers->emplace_back(lowercase(line.substr(0, colon)),
                          std::string(trim(line.substr(colon + 1))));
  }
  return true;
}

std::string serializeResponse(const HttpResponse& response) {
  std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                    httpReason(response.status) + "\r\n";
  out += "Content-Type: " + response.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  out += "Cache-Control: no-store\r\n";
  out += "Connection: close\r\n\r\n";
  out += response.body;
  return out;
}

}  // namespace

const char* httpReason(int status) noexcept {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    // M13b: a CloudPRNT job download answers 415 when the printer asks for a media type
    // the job cannot be served as (docs/wire-protocols.md §2). The reason phrase is
    // cosmetic to a client that reads the code, and wrong on the wire without this.
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "OK";
  }
}

// RFC 3986 percent-decoding, and nothing else. `+` stays a plus: it means a space only
// in form-encoded query strings, and a verification token is base-94 over printable
// ASCII — turning its '+' into a space would make the token on the paper unresolvable.
std::string percentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = hexValue(text[i + 1]);
      const int low = hexValue(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string formDecode(std::string_view text) {
  std::string plus_decoded;
  plus_decoded.reserve(text.size());
  for (const char c : text) {
    plus_decoded.push_back(c == '+' ? ' ' : c);
  }
  return percentDecode(plus_decoded);
}

const std::string* HttpRequest::header(std::string_view name) const noexcept {
  const std::string wanted = lowercase(name);
  for (const auto& entry : headers) {
    if (entry.first == wanted) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::string HttpRequest::queryValue(std::string_view name, bool* found) const {
  if (found != nullptr) {
    *found = false;
  }
  size_t pos = 0;
  while (pos < query.size()) {
    size_t end = query.find('&', pos);
    if (end == std::string::npos) {
      end = query.size();
    }
    const std::string_view pair(query.data() + pos, end - pos);
    const size_t equals = pair.find('=');
    const std::string_view key = equals == std::string_view::npos
                                     ? pair
                                     : pair.substr(0, equals);
    if (key == name) {
      if (found != nullptr) {
        *found = true;
      }
      return equals == std::string_view::npos
                 ? std::string()
                 : formDecode(pair.substr(equals + 1));
    }
    pos = end + 1;
  }
  return {};
}

// --- Server ---------------------------------------------------------------------------

HttpServer::HttpServer(HttpServerConfig config) : config_(std::move(config)) {
  listen_socket_ = asHandle(net::invalidSocket());
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(HttpHandler handler, std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    if (listening_) {
      net::closeSocket(asSocket(listen_socket_));
      listen_socket_ = asHandle(net::invalidSocket());
      listening_ = false;
    }
    return false;
  };

  if (running_.load()) {
    return fail("server already running");
  }
  if (!handler) {
    return fail("no handler");
  }
  if (!net::startup()) {
    return fail("socket layer unavailable");
  }

  const net::Socket socket = net::create(AF_INET, SOCK_STREAM, 0);
  if (!net::valid(socket)) {
    return fail("socket: " + net::errorText(net::lastError()));
  }
  listen_socket_ = asHandle(socket);
  listening_ = true;
#if !PD_PLATFORM_WINDOWS
  // POSIX-only on purpose: Winsock's SO_REUSEADDR lets a second socket steal a bound
  // address outright rather than reusing a TIME_WAIT one.
  net::setIntOption(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = net::fromNetwork16(config_.port);
  if (config_.bind.empty() || config_.bind == "0.0.0.0" || config_.bind == "*") {
    address.sin_addr.s_addr = net::toNetwork32(0);
  } else if (config_.bind == "127.0.0.1" || config_.bind == "localhost") {
    address.sin_addr.s_addr = net::loopbackAddress();
  } else {
    uint32_t parsed = 0;
    unsigned octets[4] = {0, 0, 0, 0};
    if (std::sscanf(config_.bind.c_str(), "%u.%u.%u.%u", &octets[0], &octets[1],
                    &octets[2], &octets[3]) != 4 ||
        octets[0] > 255 || octets[1] > 255 || octets[2] > 255 || octets[3] > 255) {
      return fail("not an IPv4 bind address: " + config_.bind);
    }
    parsed = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    address.sin_addr.s_addr = net::toNetwork32(parsed);
  }

  if (::bind(socket, reinterpret_cast<sockaddr*>(&address),
             static_cast<net::SockLen>(sizeof(address))) != 0) {
    return fail("bind " + config_.bind + ":" + std::to_string(config_.port) + ": " +
                net::errorText(net::lastError()));
  }
  if (::listen(socket, config_.backlog) != 0) {
    return fail("listen: " + net::errorText(net::lastError()));
  }
  net::SockLen length = static_cast<net::SockLen>(sizeof(address));
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return fail("getsockname: " + net::errorText(net::lastError()));
  }
  port_.store(net::fromNetwork16(address.sin_port));

  handler_ = std::move(handler);
  running_.store(true);
  const uint32_t worker_count = std::max<uint32_t>(1, config_.workers);
  workers_.reserve(worker_count);
  for (uint32_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this] { workerLoop(); });
  }
  acceptor_ = std::thread([this] { acceptLoop(); });
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (listening_) {
    net::shutdownBoth(asSocket(listen_socket_));
    net::closeSocket(asSocket(listen_socket_));
    listen_socket_ = asHandle(net::invalidSocket());
    listening_ = false;
  }
  if (acceptor_.joinable()) {
    acceptor_.join();
  }
  ready_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::uintptr_t socket : pending_) {
    net::closeSocket(asSocket(socket));
  }
  pending_.clear();
}

void HttpServer::acceptLoop() {
  while (running_.load()) {
    net::PollFd waiter;
    waiter.socket = asSocket(listen_socket_);
    waiter.events = net::kPollIn;
    if (!listening_ || net::poll(&waiter, 1, 100) <= 0) {
      continue;
    }
    const net::Socket client = static_cast<net::Socket>(
        ::accept(asSocket(listen_socket_), nullptr, nullptr));
    if (!net::valid(client)) {
      continue;
    }
    net::setIntOption(client, IPPROTO_TCP, TCP_NODELAY, 1);
    bool overflow = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_.size() >= config_.max_queued_connections) {
        overflow = true;
      } else {
        pending_.push_back(asHandle(client));
      }
    }
    if (overflow) {
      // Loud refusal beats a growing backlog: the caller sees 503 and can hold its own
      // ticket instead of assuming the agent took it.
      sendAll(client, serializeResponse(HttpResponse::json(
                          503, "{\"error\":\"agent busy\"}")));
      net::shutdownBoth(client);
      net::closeSocket(client);
      continue;
    }
    ready_.notify_one();
  }
}

void HttpServer::workerLoop() {
  for (;;) {
    std::uintptr_t socket = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return !pending_.empty() || !running_.load(); });
      if (pending_.empty()) {
        if (!running_.load()) {
          return;
        }
        continue;
      }
      socket = pending_.front();
      pending_.pop_front();
    }
    serve(socket);
  }
}

void HttpServer::serve(std::uintptr_t handle) {
  const net::Socket socket = asSocket(handle);
  std::string buffer;
  HttpResponse response;

  const bool got_head =
      readUntil(socket, &buffer, config_.max_body_bytes + 65536, config_.read_timeout_ms,
                [](const std::string& text) {
                  return headerEnd(text) != std::string::npos;
                });
  if (!got_head) {
    net::shutdownBoth(socket);
    net::closeSocket(socket);
    return;
  }

  const size_t body_start = headerEnd(buffer);
  HttpRequest request;
  std::string request_line;
  if (!parseHeaderBlock(std::string_view(buffer).substr(0, body_start), &request_line,
                        &request.headers)) {
    response = HttpResponse::json(400, "{\"error\":\"malformed request headers\"}");
  } else {
    const size_t first_space = request_line.find(' ');
    const size_t second_space = first_space == std::string::npos
                                    ? std::string::npos
                                    : request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
      response = HttpResponse::json(400, "{\"error\":\"malformed request line\"}");
    } else {
      request.method = request_line.substr(0, first_space);
      request.target =
          request_line.substr(first_space + 1, second_space - first_space - 1);
      const size_t question = request.target.find('?');
      request.raw_path = question == std::string::npos
                             ? request.target
                             : request.target.substr(0, question);
      // Decoded whole-path form, for logs and error bodies. Routing must never split
      // this one: a percent-encoded '/' inside a segment — and a verification token is
      // base 94, so it happens — would become a segment boundary that was never there.
      request.path = percentDecode(request.raw_path);
      request.query = question == std::string::npos
                          ? std::string()
                          : request.target.substr(question + 1);

      const std::string* encoding = request.header("transfer-encoding");
      const std::string* length = request.header("content-length");
      if (encoding != nullptr && !encoding->empty()) {
        response = HttpResponse::json(
            501, "{\"error\":\"transfer-encoding is not supported; send content-length\"}");
      } else {
        size_t expected = 0;
        bool bad_length = false;
        if (length != nullptr) {
          char* end = nullptr;
          const unsigned long long value = std::strtoull(length->c_str(), &end, 10);
          bad_length = end == nullptr || *end != '\0';
          expected = static_cast<size_t>(value);
        }
        if (bad_length) {
          response = HttpResponse::json(400, "{\"error\":\"malformed content-length\"}");
        } else if (expected > config_.max_body_bytes) {
          response = HttpResponse::json(413, "{\"error\":\"body too large\"}");
        } else {
          const size_t needed = body_start + expected;
          const bool got_body =
              buffer.size() >= needed ||
              readUntil(socket, &buffer, needed + 1024, config_.read_timeout_ms,
                        [needed](const std::string& text) {
                          return text.size() >= needed;
                        });
          if (!got_body) {
            response = HttpResponse::json(400, "{\"error\":\"incomplete request body\"}");
          } else {
            request.body = buffer.substr(body_start, expected);
            try {
              response = handler_(request);
            } catch (const std::exception& error) {
              response = HttpResponse::json(
                  500, std::string("{\"error\":\"handler failed\",\"detail\":\"") +
                           error.what() + "\"}");
            }
          }
        }
      }
    }
  }

  sendAll(socket, serializeResponse(response));
  net::shutdownBoth(socket);
  net::closeSocket(socket);
}

// --- Client ---------------------------------------------------------------------------

HttpClientResult httpRequest(const std::string& host, uint16_t port,
                             const std::string& method, const std::string& target,
                             const std::string& body, const std::string& content_type,
                             uint32_t timeout_ms) {
  HttpClientResult out;
  if (!net::startup()) {
    out.error = "socket layer unavailable";
    return out;
  }
  const net::Socket socket = net::create(AF_INET, SOCK_STREAM, 0);
  if (!net::valid(socket)) {
    out.error = "socket: " + net::errorText(net::lastError());
    return out;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = net::fromNetwork16(port);
  unsigned octets[4] = {127, 0, 0, 1};
  if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &octets[0], &octets[1], &octets[2],
                  &octets[3]) != 4) {
    net::closeSocket(socket);
    out.error = "not an IPv4 host: " + host;
    return out;
  }
  address.sin_addr.s_addr = net::toNetwork32((octets[0] << 24) | (octets[1] << 16) |
                                             (octets[2] << 8) | octets[3]);
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address),
                static_cast<net::SockLen>(sizeof(address))) != 0) {
    out.error = "connect: " + net::errorText(net::lastError());
    net::closeSocket(socket);
    return out;
  }

  std::string request = method + " " + target + " HTTP/1.1\r\n";
  request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  request += "Connection: close\r\n";
  if (!body.empty()) {
    request += "Content-Type: " + content_type + "\r\n";
  }
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  request += body;
  if (!sendAll(socket, request)) {
    out.error = "write failed";
    net::closeSocket(socket);
    return out;
  }

  std::string buffer;
  const bool got_head = readUntil(socket, &buffer, 1u << 24, timeout_ms,
                                  [](const std::string& text) {
                                    return headerEnd(text) != std::string::npos;
                                  });
  if (!got_head) {
    out.error = "no response headers";
    net::closeSocket(socket);
    return out;
  }
  const size_t body_start = headerEnd(buffer);
  std::string status_line;
  std::vector<std::pair<std::string, std::string>> headers;
  if (!parseHeaderBlock(std::string_view(buffer).substr(0, body_start), &status_line,
                        &headers)) {
    out.error = "malformed response headers";
    net::closeSocket(socket);
    return out;
  }
  const size_t first_space = status_line.find(' ');
  if (first_space == std::string::npos) {
    out.error = "malformed status line";
    net::closeSocket(socket);
    return out;
  }
  out.status = std::atoi(status_line.c_str() + first_space + 1);

  size_t expected = 0;
  for (const auto& entry : headers) {
    if (entry.first == "content-length") {
      expected = static_cast<size_t>(std::strtoull(entry.second.c_str(), nullptr, 10));
    }
  }
  const size_t needed = body_start + expected;
  if (buffer.size() < needed) {
    readUntil(socket, &buffer, needed + 1024, timeout_ms,
              [needed](const std::string& text) { return text.size() >= needed; });
  }
  out.body = buffer.size() >= needed ? buffer.substr(body_start, expected)
                                     : buffer.substr(body_start);
  out.ok = true;
  net::shutdownBoth(socket);
  net::closeSocket(socket);
  return out;
}

}  // namespace pd::agent
