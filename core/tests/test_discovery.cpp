#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "printerdriver/discovery.hpp"
#include "printerdriver/net_platform.hpp"
#include "test_harness.hpp"

// Discovery is the one part of the SDK whose failure mode is measured in paper: a probe
// byte above 0x1F comes out of every printer on the subnet. So the first test here is
// about what is *written*, and the rest are three loopback listeners standing in for the
// three answers a real venue gives — a printer with a working backchannel, a LAN module
// that forwards nothing, and a port with nothing behind it.

namespace {

// A loopback listener that optionally answers whatever it is sent with one status byte.
class Listener {
 public:
  explicit Listener(bool answer, uint8_t reply = 0x16) : answer_(answer), reply_(reply) {}
  ~Listener() { stop(); }

  bool start() {
    socket_ = pd::net::create(AF_INET, SOCK_STREAM, 0);
    if (!pd::net::valid(socket_)) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = pd::net::loopbackAddress();
    address.sin_port = 0;
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&address),
               static_cast<pd::net::SockLen>(sizeof(address))) != 0 ||
        ::listen(socket_, 4) != 0) {
      pd::net::closeSocket(socket_);
      socket_ = pd::net::invalidSocket();
      return false;
    }
    pd::net::SockLen length = static_cast<pd::net::SockLen>(sizeof(address));
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      pd::net::closeSocket(socket_);
      socket_ = pd::net::invalidSocket();
      return false;
    }
    port_ = pd::net::fromNetwork16(address.sin_port);
    thread_ = std::thread([this] { serve(); });
    return true;
  }

  uint16_t port() const { return port_; }

  std::vector<uint8_t> received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
  }

  void stop() {
    running_.store(false);
    // Shut down to wake a blocked accept(), but close only after the join: serve() is
    // still polling on this descriptor, and a closed number can be reissued under it.
    if (pd::net::valid(socket_.load())) {
      pd::net::shutdownBoth(socket_.load());
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    const pd::net::Socket listener = socket_.exchange(pd::net::invalidSocket());
    if (pd::net::valid(listener)) {
      pd::net::closeSocket(listener);
    }
  }

 private:
  void serve() {
    while (running_.load()) {
      const pd::net::Socket listener = socket_.load();
      pd::net::PollFd waiter;
      waiter.socket = listener;
      waiter.events = pd::net::kPollIn;
      if (!pd::net::valid(listener) || pd::net::poll(&waiter, 1, 50) <= 0) {
        continue;
      }
      const pd::net::Socket client =
          static_cast<pd::net::Socket>(::accept(listener, nullptr, nullptr));
      if (!pd::net::valid(client)) {
        continue;
      }
      uint8_t buffer[64];
      pd::net::PollFd reader;
      reader.socket = client;
      reader.events = pd::net::kPollIn;
      if (pd::net::poll(&reader, 1, 500) > 0) {
        const int64_t got = pd::net::recvSome(client, buffer, sizeof(buffer));
        if (got > 0) {
          std::lock_guard<std::mutex> lock(mutex_);
          received_.insert(received_.end(), buffer, buffer + got);
          if (answer_) {
            const int64_t ignored = pd::net::sendSome(client, &reply_, 1);
            (void)ignored;
          }
        }
      }
      // Silent listeners hold the socket open a moment so the probe's read budget is
      // what expires, not the peer's FIN.
      if (!answer_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      pd::net::closeSocket(client);
    }
  }

  bool answer_ = true;
  uint8_t reply_ = 0x16;
  // Atomic: stop() retires it while serve() is still polling on it.
  std::atomic<pd::net::Socket> socket_{pd::net::invalidSocket()};
  uint16_t port_ = 0;
  std::atomic<bool> running_{true};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<uint8_t> received_;
};

pd::DiscoveryOptions fast(uint16_t port) {
  pd::DiscoveryOptions options;
  options.port = port;
  options.connect_timeout_ms = 500;
  options.response_timeout_ms = 250;
  return options;
}

// A port nothing is listening on. Bound, read for its number, then closed — so the
// number is real and the port is refusing by the time discovery reaches it.
uint16_t closedPort() {
  Listener listener(false);
  listener.start();
  const uint16_t port = listener.port();
  listener.stop();
  return port;
}

}  // namespace

PD_TEST(the_probe_is_dle_eot_1_and_nothing_printable) {
  CHECK_EQ(pd::kDiscoveryProbeSize, static_cast<size_t>(3));
  CHECK_EQ(pd::kDiscoveryProbe[0], static_cast<uint8_t>(0x10));
  CHECK_EQ(pd::kDiscoveryProbe[1], static_cast<uint8_t>(0x04));
  CHECK_EQ(pd::kDiscoveryProbe[2], static_cast<uint8_t>(0x01));
  for (size_t i = 0; i < pd::kDiscoveryProbeSize; ++i) {
    // A port-9100 device prints what it receives; 0x20 is the first byte that would.
    CHECK(pd::kDiscoveryProbe[i] < 0x20);
  }
}

PD_TEST(cidr_parsing_covers_the_shapes_a_venue_types) {
  pd::Subnet subnet;
  CHECK(pd::parseCidr("192.168.1.0/24", &subnet));
  CHECK_EQ(subnet.prefix, static_cast<uint8_t>(24));
  CHECK_EQ(subnet.toString(), std::string("192.168.1.0/24"));
  CHECK_EQ(subnet.hostCount(), static_cast<uint64_t>(254));

  // A host address inside the block names the block.
  CHECK(pd::parseCidr("192.168.1.101/24", &subnet));
  CHECK_EQ(subnet.toString(), std::string("192.168.1.0/24"));

  // /31 and /32 have no network or broadcast address to skip.
  CHECK(pd::parseCidr("10.0.0.5/32", &subnet));
  CHECK_EQ(subnet.hostCount(), static_cast<uint64_t>(1));
  CHECK(pd::parseCidr("10.0.0.4/31", &subnet));
  CHECK_EQ(subnet.hostCount(), static_cast<uint64_t>(2));

  // A bare address is /32.
  CHECK(pd::parseCidr("127.0.0.1", &subnet));
  CHECK_EQ(subnet.prefix, static_cast<uint8_t>(32));

  CHECK(!pd::parseCidr("192.168.1.0/33", &subnet));
  CHECK(!pd::parseCidr("192.168.1", &subnet));
  CHECK(!pd::parseCidr("192.168.1.256/24", &subnet));
  CHECK(!pd::parseCidr("", &subnet));
  CHECK(!pd::parseCidr("not-an-address", &subnet));

  CHECK_EQ(pd::formatIpv4(0x7F000001u), std::string("127.0.0.1"));
}

PD_TEST(a_sweep_wider_than_the_supported_prefix_is_refused) {
  pd::Subnet subnet;
  CHECK(pd::parseCidr("10.0.0.0/8", &subnet));
  CHECK_THROWS(pd::discover(subnet), pd::DiscoveryError);
  CHECK_THROWS(pd::discover("not-a-cidr"), pd::DiscoveryError);
  CHECK_THROWS(pd::probeHost("nope"), pd::DiscoveryError);
}

PD_TEST(a_listener_answering_dle_eot_is_found_with_its_response) {
  Listener printer(true, 0x16);
  CHECK(printer.start());

  pd::DiscoveryCallbacks callbacks;
  std::atomic<int> found{0};
  std::atomic<int> progressed{0};
  callbacks.on_found = [&](const pd::DiscoveredDevice&) { ++found; };
  callbacks.on_progress = [&](const pd::DiscoveryProgress& progress) {
    ++progressed;
    CHECK_EQ(progress.total, static_cast<uint64_t>(1));
  };

  const std::vector<pd::DiscoveredDevice> devices =
      pd::discover("127.0.0.1/32", fast(printer.port()), callbacks);
  CHECK_EQ(devices.size(), static_cast<size_t>(1));
  if (!devices.empty()) {
    CHECK_EQ(devices[0].ip, std::string("127.0.0.1"));
    CHECK(devices[0].port9100_open);
    CHECK(devices[0].answered());
    CHECK_EQ(devices[0].responseHex(), std::string("16"));
  }
  CHECK_EQ(found.load(), 1);
  CHECK_EQ(progressed.load(), 1);

  // The device saw the probe and only the probe.
  const std::vector<uint8_t> seen = printer.received();
  CHECK_EQ(seen.size(), pd::kDiscoveryProbeSize);
  if (seen.size() == pd::kDiscoveryProbeSize) {
    CHECK(std::memcmp(seen.data(), pd::kDiscoveryProbe, pd::kDiscoveryProbeSize) == 0);
  }
}

PD_TEST(a_silent_listener_is_an_open_port_with_no_backchannel) {
  Listener mute(false);
  CHECK(mute.start());

  const pd::DiscoveredDevice device = pd::probeHost("127.0.0.1", fast(mute.port()));
  CHECK(device.port9100_open);
  // docs/techspec.md §4: the LAN module that does not forward status bytes. Reported
  // as an open port with an empty answer, never as "not a printer".
  CHECK(!device.answered());
  CHECK_EQ(device.responseHex(), std::string());
  CHECK_EQ(device.port, mute.port());
}

PD_TEST(a_refused_port_is_absent_from_the_sweep) {
  const uint16_t port = closedPort();
  const pd::DiscoveredDevice device = pd::probeHost("127.0.0.1", fast(port));
  CHECK(!device.port9100_open);
  CHECK(!device.answered());

  const std::vector<pd::DiscoveredDevice> devices =
      pd::discover("127.0.0.1/32", fast(port));
  CHECK(devices.empty());
}

PD_TEST(the_port_sweep_can_run_without_writing_anything) {
  Listener printer(true, 0x16);
  CHECK(printer.start());
  pd::DiscoveryOptions options = fast(printer.port());
  options.probe_backchannel = false;

  const std::vector<pd::DiscoveredDevice> devices =
      pd::discover("127.0.0.1/32", options);
  CHECK_EQ(devices.size(), static_cast<size_t>(1));
  if (!devices.empty()) {
    CHECK(devices[0].port9100_open);
    CHECK(!devices[0].answered());
  }
  CHECK(printer.received().empty());
}
