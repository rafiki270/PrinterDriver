#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// LAN discovery (docs/sdk-spec.md §11.3: "leaning core for LAN discovery"). The web POS
// ships a scanner that returns fake devices and the native suite has an Android-only /24
// sweep with no iOS equivalent; both are the same missing piece, so it lives here once.
//
// -- The one safety rule that shapes this whole file -----------------------------------
//
// A port-9100 device prints what it receives. Anything printable this scanner sent would
// come out of every printer on the subnet as a strip of garbage paper, and a scan that
// costs a venue a roll of paper per run is a scan nobody will ever run. So the probe is
// exactly three bytes, `10 04 01` — DLE EOT 1, the real-time status query of
// docs/techspec.md §3 — and no other byte is ever written to a discovered socket.
//
// DLE EOT is real-time: it is answered out of order with buffered print data, which
// makes it useless as a completion fence (docs/techspec.md §3.3) and ideal here, where
// the question is only "is something ESC/POS-shaped listening, and does its backchannel
// reach me?". A silent answer is a real finding, not a failure: it is the LAN module
// that does not forward status bytes (docs/techspec.md §4), and the reason discovery
// reports the raw response instead of a verdict.
//
// The answer is raw bytes and never a model name. Deciding what a device *is* belongs to
// the identity and capability probes (printerdriver/capability_probe.hpp), which cost
// paper and are pointed at one address deliberately.

namespace pd {

class DiscoveryError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The whole write side of discovery, spelled out so a test can assert on it: DLE EOT 1,
// and nothing else, ever. Every byte is below 0x20, so none of it can print.
inline constexpr uint8_t kDiscoveryProbe[3] = {0x10, 0x04, 0x01};
inline constexpr size_t kDiscoveryProbeSize = sizeof(kDiscoveryProbe);

// An IPv4 subnet in host byte order. `prefix` is the CIDR length.
struct Subnet {
  uint32_t network = 0;
  uint8_t prefix = 32;

  uint32_t mask() const noexcept;
  // First and last address of the block, inclusive.
  uint32_t first() const noexcept;
  uint32_t last() const noexcept;
  // Addresses worth connecting to: network and broadcast are excluded for /30 and
  // wider, kept for /31 (RFC 3021 point-to-point) and /32 (a single host).
  uint64_t hostCount() const noexcept;
  std::string toString() const;
};

// Refusing to sweep the internet is a feature: /16 is 65 534 connects already, and
// anything wider is a mistyped CIDR rather than a venue.
inline constexpr uint8_t kMinDiscoveryPrefix = 16;

std::string formatIpv4(uint32_t address);
bool parseIpv4(std::string_view text, uint32_t* out) noexcept;
// "192.168.1.0/24", or a bare address, which is read as /32.
bool parseCidr(std::string_view text, Subnet* out) noexcept;

// The /24 around this host's primary address, or nullopt when it cannot be determined.
// Found by asking the routing table which local address would be used to reach a remote
// one — a connected UDP socket plus getsockname(). No packet is ever transmitted.
std::optional<Subnet> localSubnet() noexcept;

struct DiscoveryOptions {
  uint16_t port = 9100;
  // Sockets in flight at once. Each worker owns one connect at a time, so this is also
  // the thread count.
  uint32_t concurrency = 32;
  uint32_t connect_timeout_ms = 300;
  // How long to wait for the DLE EOT 1 answer once the port is open. Silence here is a
  // finding, so this budget is short on purpose.
  uint32_t response_timeout_ms = 400;
  // Off turns discovery into a pure port sweep: the port state is still reported, and
  // not one byte is written.
  bool probe_backchannel = true;
};

struct DiscoveredDevice {
  std::string ip;
  uint16_t port = 9100;
  bool port9100_open = false;
  // Whatever came back from `10 04 01`, verbatim. Empty means the port accepted the
  // connection and said nothing — a LAN module that does not forward status bytes.
  std::vector<uint8_t> dle_eot_response;

  std::string responseHex() const;
  bool answered() const noexcept { return !dle_eot_response.empty(); }
};

// Every address the sweep finishes, open or not, in completion order — the progress
// channel a UI needs. Called from worker threads: it must not block.
struct DiscoveryProgress {
  DiscoveredDevice device;
  uint64_t completed = 0;
  uint64_t total = 0;
};

using DiscoveryFoundCallback = std::function<void(const DiscoveredDevice&)>;
using DiscoveryProgressCallback = std::function<void(const DiscoveryProgress&)>;

struct DiscoveryCallbacks {
  // Open ports only, as they are found.
  DiscoveryFoundCallback on_found;
  // Every address, open or closed.
  DiscoveryProgressCallback on_progress;
};

// One address. Reports a closed or unreachable port as `port9100_open = false` rather
// than as an error: on a subnet sweep that is the ordinary case.
DiscoveredDevice probeHost(const std::string& ip, const DiscoveryOptions& options = {});

// Sweeps a subnet and returns the open ports, sorted by address. Throws DiscoveryError
// on a malformed CIDR or one wider than kMinDiscoveryPrefix.
std::vector<DiscoveredDevice> discover(const Subnet& subnet,
                                       const DiscoveryOptions& options = {},
                                       const DiscoveryCallbacks& callbacks = {});
std::vector<DiscoveredDevice> discover(std::string_view cidr,
                                       const DiscoveryOptions& options = {},
                                       const DiscoveryCallbacks& callbacks = {});
// The local /24 (localSubnet()). Throws DiscoveryError when it cannot be determined.
std::vector<DiscoveredDevice> discover(const DiscoveryOptions& options = {},
                                       const DiscoveryCallbacks& callbacks = {});

}  // namespace pd
