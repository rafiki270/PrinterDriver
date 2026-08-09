#include "printerdriver/probe_path.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>

#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/response_parser.hpp"

namespace pd {
namespace {

// Kept small on purpose. The interesting bytes on a print-server path are the first few;
// a box that floods the backchannel is a finding the raw prefix already carries, and an
// unbounded buffer here would let it become a memory problem instead.
constexpr size_t kMaxUnclassifiedKept = 32;

uint32_t elapsedMsSince(MonotonicTime start) noexcept {
  const auto delta =
      std::chrono::duration_cast<std::chrono::milliseconds>(MonotonicClock::now() - start);
  return delta.count() <= 0 ? 0u : static_cast<uint32_t>(delta.count());
}

std::string transportMessage(const TransportResult& result) {
  return result.message.empty() ? std::string(to_string(result.error)) : result.message;
}

// Collects the far side's stream twice over: raw, because on an unfamiliar path the bytes
// nobody could classify are the interesting part, and parsed, because only the parser can
// tell a correlated GS ( H echo from a status byte that happens to look like one. There is
// no second parser here for the same reason there is no second encoder: the interleaving
// rules live in pd::escpos::ResponseParser or they live nowhere.
class PathListener {
 public:
  void attach(Transport& transport) {
    transport.onBytes([this](const uint8_t* data, size_t size) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        raw_.insert(raw_.end(), data, data + size);
        phase_raw_.insert(phase_raw_.end(), data, data + size);
        for (escpos::ParsedEvent& event : parser_.feed(data, size)) {
          record(event);
        }
      }
      cv_.notify_all();
    });
    transport.onDisconnected([this](TransportError, const std::string& message) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped_ = true;
        drop_message_ = message;
      }
      cv_.notify_all();
    });
  }

  // The transport outlives this listener at every call site (the caller owns it), so the
  // callbacks capturing `this` must go before the listener does. Safe here and only here:
  // Transport::close() has already joined the reader thread, so nothing can be inside a
  // callback while it is being replaced.
  void detach(Transport& transport) {
    transport.onBytes(Transport::BytesCallback{});
    transport.onDisconnected(Transport::DisconnectCallback{});
  }

  void expectRealtime(escpos::DleEotKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.expectRealtime(kind);
  }

  // Any byte ends this wait, not just a well-formed status answer. A path that returns a
  // byte the parser cannot classify has still demonstrably carried printer -> host data,
  // and waiting out the rest of the budget for a prettier byte would only slow the probe
  // down without changing what it may conclude.
  void waitForAnyBytes(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return dropped_ || !phase_raw_.empty(); });
  }

  bool waitForToken(const std::string& token, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this, &token] { return dropped_ || tokens_.count(token) != 0; });
    return tokens_.count(token) != 0;
  }

  // The read side has gone idle, so a lone byte still held as a possible incomplete frame
  // is never going to be completed. Force-classifying it here is what keeps a path that
  // answered with exactly one unrecognisable byte from being reported as silent.
  void flushHeldBytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (escpos::ParsedEvent& event : parser_.flush()) {
      record(event);
    }
  }

  std::vector<uint8_t> takePhaseBytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> out;
    out.swap(phase_raw_);
    return out;
  }

  std::vector<uint8_t> raw() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raw_;
  }
  std::vector<uint8_t> unclassified() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return unclassified_;
  }
  bool realtimeAnswered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return realtime_answered_;
  }
  bool dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }
  std::string dropMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drop_message_;
  }
  // Every distinct GS ( H token seen, in arrival order. The caller subtracts its own.
  std::vector<std::string> tokens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_order_;
  }

 private:
  void record(const escpos::ParsedEvent& event) {
    switch (event.kind) {
      case escpos::ParsedEventKind::GsHAck:
        if (tokens_.insert(event.token).second) {
          token_order_.push_back(event.token);
        }
        break;
      case escpos::ParsedEventKind::RealtimeStatus:
      case escpos::ParsedEventKind::AsbStatus:
        // ASB counts too: this probe never enables it, so a device volunteering a status
        // frame is still the printer's own answer reaching this host.
        realtime_answered_ = true;
        break;
      case escpos::ParsedEventKind::UnknownByte:
        if (unclassified_.size() < kMaxUnclassifiedKept) {
          unclassified_.push_back(event.byte);
        }
        break;
      case escpos::ParsedEventKind::QueuedStatus:
        break;
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  escpos::ResponseParser parser_;
  std::vector<uint8_t> raw_;
  std::vector<uint8_t> phase_raw_;
  std::vector<uint8_t> unclassified_;
  std::set<std::string> tokens_;
  std::vector<std::string> token_order_;
  bool realtime_answered_ = false;
  bool dropped_ = false;
  std::string drop_message_;
};

std::string joinTokens(const std::vector<std::string>& tokens) {
  std::string out;
  for (const std::string& token : tokens) {
    out += out.empty() ? "" : ", ";
    out += token;
  }
  return out;
}

// Everything after the last phase: classify, explain, and hand the transport back in the
// state it was given in. Shared by every early return so a probe that gave up at connect
// still comes back with a filled-in verdict rather than a default-constructed one.
PathProbeFindings& finish(PathProbeFindings& findings, Transport& transport,
                          PathListener& listener) {
  // Read before closing: close() is deliberate and never notifies, so anything the
  // listener holds at this point is the far side hanging up rather than this probe
  // finishing.
  findings.link_dropped = listener.dropped();
  findings.drop_message = listener.dropMessage();
  transport.close();
  listener.detach(transport);
  findings.authority = classifyPath(findings);
  findings.rationale = explainPath(findings);
  return findings;
}

}  // namespace

const char* pathAuthorityLabel(CompletionAuthority value) noexcept {
  switch (value) {
    case CompletionAuthority::PhysicalPrinter: return "PHYSICAL_PRINTER";
    case CompletionAuthority::VendorSpooler: return "VENDOR_SPOOLER";
    case CompletionAuthority::PdAgent: return "PD_AGENT";
    case CompletionAuthority::PrintServer: return "PRINT_SERVER_ONLY";
    case CompletionAuthority::TransportOnly: return "TRANSPORT_ONLY";
  }
  return "TRANSPORT_ONLY";
}

CompletionAuthority classifyPath(const PathProbeFindings& observation) noexcept {
  // A path that never opened, or that stopped taking bytes half-way through, was not
  // asked the question. Nothing about the far side follows from that, so the claim falls
  // all the way back to the transport (docs/compatibility-brief.md §24, grade E).
  if (!observation.connected || !observation.wrote_status_query ||
      !observation.wrote_fence) {
    return CompletionAuthority::TransportOnly;
  }
  // The correlated echo came back carrying the token this host issued. That is the one
  // observation on this path which supports a per-receipt completion claim, and it does so
  // regardless of what else did or did not answer (§24, grade A).
  if (observation.fence_echoed) {
    return CompletionAuthority::PhysicalPrinter;
  }
  // §21, exactly: every byte was accepted and not one came back. The path takes jobs and
  // returns nothing, so anything downstream of it can only ever be claimed by the box in
  // the middle.
  if (observation.raw.empty()) {
    return CompletionAuthority::PrintServer;
  }
  // Something came back — a real-time status byte, an unrecognised byte, somebody else's
  // echo — but not this host's correlated answer. The backchannel is not the thing that
  // failed, so this is not the print-server verdict; it is simply a path over which no
  // receipt can be individually acknowledged.
  return CompletionAuthority::TransportOnly;
}

std::string explainPath(const PathProbeFindings& observation) {
  std::string text;
  if (!observation.connected) {
    text = "the path did not open (" +
           (observation.connect_error.empty() ? std::string("connect failed")
                                              : observation.connect_error) +
           "), so nothing was established about anything behind it";
  } else if (!observation.wrote_status_query || !observation.wrote_fence) {
    text = "the path opened and then stopped accepting bytes (" +
           (observation.write_error.empty() ? std::string("write failed")
                                            : observation.write_error) +
           "): a probe that could not finish asking proves nothing about what would "
           "have answered";
  } else if (observation.fence_echoed) {
    text = "responses survive this path: the GS ( H fn 48 echo came back carrying the "
           "token this probe issued (" +
           observation.token +
           "), so a completion crossing it can still be attributed to one receipt";
    if (!observation.dle_eot_answered) {
      text +=
          ". DLE EOT did not answer, which is a fact about the device rather than about "
          "the path — the correlated echo has already proved the backchannel";
    }
  } else if (observation.raw.empty()) {
    text =
        "the path accepted every byte of the probe and returned none: DLE EOT 1 was not "
        "answered and the GS ( H fn 48 echo never arrived, so the printer's answers do "
        "not reach this host. TCP acceptance here means the print server took the bytes, "
        "never that the paper moved";
  } else if (observation.dle_eot_answered) {
    text = "partial: DLE EOT 1 answered, so printer -> host bytes do traverse this path, "
           "but the GS ( H fn 48 echo carrying token " +
           observation.token +
           " never came back inside the budget. Two readings stay open and no probe "
           "standing at this end can separate them — the device does not implement fn 48, "
           "or something on the path drops the correlated echo. Either way no receipt can "
           "be individually acknowledged over it";
  } else {
    text = "the path returned " + std::to_string(observation.raw.size()) +
           " byte(s) that answered neither question: no DLE EOT 1 status and no GS ( H "
           "fn 48 echo. Bytes do come back, so this is not a silent path, but nothing "
           "that came back is an answer this host asked for";
  }
  if (observation.link_dropped) {
    text += ". The far side also closed the link before the probe finished (" +
            (observation.drop_message.empty() ? std::string("no reason given")
                                              : observation.drop_message) +
            "), which is how a forwarder with no idea what its printer is doing usually "
            "ends a session";
  }
  if (observation.foreign_token_echoed) {
    text += ". A GS ( H echo carrying a token this probe never issued (" +
            joinTokens(observation.foreign_tokens) +
            ") also came back: something else is writing to the same printer, so an "
            "acknowledgement seen here cannot be assumed to answer this host "
            "(docs/sdk-spec.md §14)";
  }
  text += ". completionAuthority = ";
  text += pathAuthorityLabel(classifyPath(observation));
  return text;
}

PathProbeFindings probePath(Transport& transport, const PathProbeOptions& options) {
  PathProbeFindings findings;
  findings.endpoint = options.endpoint.empty() ? transport.describe() : options.endpoint;
  findings.token = escpos::isValidProcessIdToken(options.token) ? options.token
                                                                : std::string(kPathProbeToken);

  PathListener listener;
  listener.attach(transport);

  const MonotonicTime connect_started = MonotonicClock::now();
  const TransportResult connected = transport.connect();
  findings.connect_ms = elapsedMsSince(connect_started);
  if (!connected.ok) {
    findings.connect_error = transportMessage(connected);
    return finish(findings, transport, listener);
  }
  findings.connected = true;

  // 1. Does anything at all come back? DLE EOT 1 is answered by the printer the moment it
  //    is scanned, so this measures the path and not the print buffer. It is also the
  //    exact three bytes the subnet sweep writes (printerdriver/discovery.hpp), which
  //    keeps "the port answered a sweep" and "the path carries responses" the same
  //    question asked twice rather than two subtly different ones.
  listener.expectRealtime(escpos::DleEotKind::PrinterStatus);
  const MonotonicTime status_started = MonotonicClock::now();
  const TransportResult status_written =
      transport.write(escpos::dleEot(escpos::DleEotKind::PrinterStatus));
  if (!status_written.ok) {
    findings.write_error = transportMessage(status_written);
    return finish(findings, transport, listener);
  }
  findings.wrote_status_query = true;
  listener.waitForAnyBytes(options.status_timeout_ms);
  findings.status_ms = elapsedMsSince(status_started);
  findings.dle_eot_response = listener.takePhaseBytes();

  // 2. Does a CORRELATED answer come back? One marker, one token, and the echo either
  //    names that token or it is not an answer to this question. The optional test line
  //    exists so the fence has data ahead of it to finish, which is what makes the echo
  //    an ordered fence rather than a bare capability check (docs/techspec.md §3.1).
  escpos::Bytes fence;
  if (options.print_test_line) {
    escpos::Encoder encoder;
    encoder.line("pd path probe: GS ( H fn 48").feed();
    const escpos::Bytes lines = encoder.take();
    fence.insert(fence.end(), lines.begin(), lines.end());
  }
  const escpos::Bytes marker = escpos::processIdMarker(findings.token);
  fence.insert(fence.end(), marker.begin(), marker.end());

  const MonotonicTime fence_started = MonotonicClock::now();
  const TransportResult fence_written = transport.write(fence);
  if (!fence_written.ok) {
    findings.write_error = transportMessage(fence_written);
    return finish(findings, transport, listener);
  }
  findings.wrote_fence = true;
  findings.fence_echoed = listener.waitForToken(findings.token, options.fence_timeout_ms);
  findings.fence_ms = elapsedMsSince(fence_started);

  listener.flushHeldBytes();
  for (const std::string& token : listener.tokens()) {
    if (token != findings.token) {
      findings.foreign_token_echoed = true;
      findings.foreign_tokens.push_back(token);
    }
  }
  findings.dle_eot_answered = listener.realtimeAnswered();
  findings.unclassified = listener.unclassified();
  findings.raw = listener.raw();
  return finish(findings, transport, listener);
}

PathProbeFindings probePath(const PathProbeOptions& options) {
  TcpConfig config;
  config.host = options.host;
  config.port = options.port;
  config.connect_timeout_ms = options.connect_timeout_ms;
  TcpTransport transport(config);
  return probePath(transport, options);
}

}  // namespace pd
