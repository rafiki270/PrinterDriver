#include "printerdriver/discovery.hpp"

#include "printerdriver/net_platform.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <thread>

namespace pd {
namespace {

// One worker's whole job: connect, optionally ask DLE EOT 1, read whatever comes back,
// close. Written against pd::net so the POSIX and Winsock spellings stay in one place.
DiscoveredDevice probeAddress(uint32_t address, const DiscoveryOptions& options) {
  DiscoveredDevice out;
  out.ip = formatIpv4(address);
  out.port = options.port;

  const net::Socket socket = net::create(AF_INET, SOCK_STREAM, 0);
  if (!net::valid(socket)) {
    return out;
  }
  // Non-blocking throughout: a closed port answers instantly, a firewalled one never
  // answers at all, and the difference must cost a timeout rather than a hung thread.
  if (!net::setNonBlocking(socket)) {
    net::closeSocket(socket);
    return out;
  }

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = net::toNetwork32(address);
  // fromNetwork16 is a byte swap on a little-endian host and the identity on a
  // big-endian one, which is exactly htons in both directions.
  target.sin_port = net::fromNetwork16(options.port);

  const int started = ::connect(socket, reinterpret_cast<sockaddr*>(&target),
                                static_cast<net::SockLen>(sizeof(target)));
  if (started != 0) {
    const int error = net::lastError();
    if (!net::inProgress(error)) {
      net::closeSocket(socket);
      return out;
    }
    net::PollFd waiter;
    waiter.socket = socket;
    waiter.events = net::kPollOut;
    const int ready =
        net::poll(&waiter, 1, static_cast<int>(options.connect_timeout_ms));
    if (ready <= 0 || (waiter.revents & (net::kPollError | net::kPollInvalid)) != 0) {
      net::closeSocket(socket);
      return out;
    }
    int pending = 0;
    if (!net::pendingError(socket, &pending) || pending != 0) {
      net::closeSocket(socket);
      return out;
    }
  }

  out.port9100_open = true;

  if (options.probe_backchannel) {
    // The only write discovery ever performs. Three bytes, none of them printable.
    const int64_t sent =
        net::sendSome(socket, kDiscoveryProbe, kDiscoveryProbeSize);
    if (sent == static_cast<int64_t>(kDiscoveryProbeSize)) {
      net::PollFd reader;
      reader.socket = socket;
      reader.events = net::kPollIn;
      if (net::poll(&reader, 1, static_cast<int>(options.response_timeout_ms)) > 0 &&
          (reader.revents & net::kPollIn) != 0) {
        uint8_t buffer[64];
        const int64_t got = net::recvSome(socket, buffer, sizeof(buffer));
        if (got > 0) {
          out.dle_eot_response.assign(buffer, buffer + got);
        }
      }
    }
  }

  net::shutdownBoth(socket);
  net::closeSocket(socket);
  return out;
}

}  // namespace

// --- Subnet arithmetic --------------------------------------------------------------

uint32_t Subnet::mask() const noexcept {
  if (prefix == 0) {
    return 0;
  }
  return prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32u - prefix)) - 1u);
}

uint32_t Subnet::first() const noexcept { return network & mask(); }

uint32_t Subnet::last() const noexcept { return first() | ~mask(); }

uint64_t Subnet::hostCount() const noexcept {
  const uint64_t size = static_cast<uint64_t>(last() - first()) + 1u;
  // /31 is a point-to-point pair and /32 is one host: both addresses are real there.
  return prefix >= 31 ? size : size - 2u;
}

std::string Subnet::toString() const {
  return formatIpv4(first()) + "/" + std::to_string(static_cast<int>(prefix));
}

std::string formatIpv4(uint32_t address) {
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (address >> 24) & 0xFFu,
                (address >> 16) & 0xFFu, (address >> 8) & 0xFFu, address & 0xFFu);
  return std::string(text);
}

bool parseIpv4(std::string_view text, uint32_t* out) noexcept {
  if (out == nullptr) {
    return false;
  }
  uint32_t value = 0;
  size_t index = 0;
  for (int octet = 0; octet < 4; ++octet) {
    if (index >= text.size() || std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
      return false;
    }
    unsigned number = 0;
    size_t digits = 0;
    while (index < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
      number = number * 10u + static_cast<unsigned>(text[index] - '0');
      ++index;
      ++digits;
      if (number > 255u || digits > 3) {
        return false;
      }
    }
    value = (value << 8) | number;
    if (octet < 3) {
      if (index >= text.size() || text[index] != '.') {
        return false;
      }
      ++index;
    }
  }
  if (index != text.size()) {
    return false;
  }
  *out = value;
  return true;
}

bool parseCidr(std::string_view text, Subnet* out) noexcept {
  if (out == nullptr || text.empty()) {
    return false;
  }
  const size_t slash = text.find('/');
  const std::string_view address = text.substr(0, slash);
  uint32_t value = 0;
  if (!parseIpv4(address, &value)) {
    return false;
  }
  uint8_t prefix = 32;
  if (slash != std::string_view::npos) {
    const std::string_view suffix = text.substr(slash + 1);
    if (suffix.empty() || suffix.size() > 2) {
      return false;
    }
    unsigned number = 0;
    for (const char c : suffix) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
        return false;
      }
      number = number * 10u + static_cast<unsigned>(c - '0');
    }
    if (number > 32u) {
      return false;
    }
    prefix = static_cast<uint8_t>(number);
  }
  out->prefix = prefix;
  out->network = value;
  out->network = out->first();
  return true;
}

std::optional<Subnet> localSubnet() noexcept {
  if (!net::startup()) {
    return std::nullopt;
  }
  // A connected UDP socket transmits nothing; connect() only asks the kernel which
  // source address the route to `remote` would use. TEST-NET-1 (RFC 5737) is the
  // destination precisely because nothing is ever meant to reach it.
  const net::Socket socket = net::create(AF_INET, SOCK_DGRAM, 0);
  if (!net::valid(socket)) {
    return std::nullopt;
  }
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = net::toNetwork32(0xC0000201u);  // 192.0.2.1
  remote.sin_port = net::fromNetwork16(static_cast<uint16_t>(9));  // discard
  if (::connect(socket, reinterpret_cast<sockaddr*>(&remote),
                static_cast<net::SockLen>(sizeof(remote))) != 0) {
    net::closeSocket(socket);
    return std::nullopt;
  }
  sockaddr_in local{};
  net::SockLen length = static_cast<net::SockLen>(sizeof(local));
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
    net::closeSocket(socket);
    return std::nullopt;
  }
  net::closeSocket(socket);

  uint32_t address = 0;
  const unsigned char* raw = reinterpret_cast<const unsigned char*>(&local.sin_addr);
  address = (static_cast<uint32_t>(raw[0]) << 24) | (static_cast<uint32_t>(raw[1]) << 16) |
            (static_cast<uint32_t>(raw[2]) << 8) | static_cast<uint32_t>(raw[3]);
  if (address == 0) {
    return std::nullopt;
  }
  Subnet subnet;
  subnet.prefix = 24;
  subnet.network = address;
  subnet.network = subnet.first();
  return subnet;
}

// --- Probe and sweep ----------------------------------------------------------------

std::string DiscoveredDevice::responseHex() const {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  out.reserve(dle_eot_response.size() * 3);
  for (size_t i = 0; i < dle_eot_response.size(); ++i) {
    if (i != 0) {
      out.push_back(' ');
    }
    out.push_back(digits[(dle_eot_response[i] >> 4) & 0x0F]);
    out.push_back(digits[dle_eot_response[i] & 0x0F]);
  }
  return out;
}

DiscoveredDevice probeHost(const std::string& ip, const DiscoveryOptions& options) {
  uint32_t address = 0;
  if (!parseIpv4(ip, &address)) {
    throw DiscoveryError("not an IPv4 address: " + ip);
  }
  return probeAddress(address, options);
}

std::vector<DiscoveredDevice> discover(const Subnet& subnet,
                                       const DiscoveryOptions& options,
                                       const DiscoveryCallbacks& callbacks) {
  if (subnet.prefix < kMinDiscoveryPrefix) {
    throw DiscoveryError("refusing to sweep " + subnet.toString() + ": /" +
                         std::to_string(static_cast<int>(kMinDiscoveryPrefix)) +
                         " is the widest supported prefix");
  }
  const uint32_t first = subnet.prefix >= 31 ? subnet.first() : subnet.first() + 1u;
  const uint32_t last = subnet.prefix >= 31 ? subnet.last() : subnet.last() - 1u;
  const uint64_t total = subnet.hostCount();
  if (total == 0) {
    return {};
  }

  std::mutex mutex;
  std::vector<DiscoveredDevice> found;
  uint64_t completed = 0;
  std::atomic<uint64_t> next{0};

  const uint32_t workers = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::min<uint64_t>(
             std::max<uint32_t>(1, options.concurrency), total)));

  auto run = [&] {
    for (;;) {
      const uint64_t index = next.fetch_add(1);
      if (index >= total) {
        return;
      }
      const uint32_t address = first + static_cast<uint32_t>(index);
      if (address > last) {
        return;
      }
      DiscoveredDevice device = probeAddress(address, options);
      DiscoveryProgress progress;
      progress.total = total;
      {
        std::lock_guard<std::mutex> lock(mutex);
        progress.completed = ++completed;
        if (device.port9100_open) {
          found.push_back(device);
        }
      }
      progress.device = device;
      if (device.port9100_open && callbacks.on_found) {
        callbacks.on_found(device);
      }
      if (callbacks.on_progress) {
        callbacks.on_progress(progress);
      }
    }
  };

  if (workers == 1) {
    run();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (uint32_t i = 0; i < workers; ++i) {
      pool.emplace_back(run);
    }
    for (std::thread& worker : pool) {
      worker.join();
    }
  }

  // Completion order is whatever the network decided; a table is read by address.
  std::sort(found.begin(), found.end(),
            [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
              uint32_t left = 0;
              uint32_t right = 0;
              parseIpv4(a.ip, &left);
              parseIpv4(b.ip, &right);
              return left < right;
            });
  return found;
}

std::vector<DiscoveredDevice> discover(std::string_view cidr,
                                       const DiscoveryOptions& options,
                                       const DiscoveryCallbacks& callbacks) {
  Subnet subnet;
  if (!parseCidr(cidr, &subnet)) {
    throw DiscoveryError("not a CIDR block: " + std::string(cidr));
  }
  return discover(subnet, options, callbacks);
}

std::vector<DiscoveredDevice> discover(const DiscoveryOptions& options,
                                       const DiscoveryCallbacks& callbacks) {
  const std::optional<Subnet> subnet = localSubnet();
  if (!subnet.has_value()) {
    throw DiscoveryError(
        "could not determine a local IPv4 subnet; pass an explicit CIDR");
  }
  return discover(*subnet, options, callbacks);
}

}  // namespace pd
