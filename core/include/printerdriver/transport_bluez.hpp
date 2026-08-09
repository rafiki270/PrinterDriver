#pragma once

// BlueZ RFCOMM (Bluetooth Classic SPP) for the Linux agent — docs/compatibility-brief.md
// §25, the `classicSPP` facet of BluetoothTransport.
//
// The whole header is behind the platform guard, so including it on a platform without
// BlueZ is legal and declares nothing. Apple and Android reach Bluetooth through
// frameworks the core must not link and therefore use CustomTransportLink in
// transport.hpp instead; Linux gets a real transport because BlueZ classic SPP is a
// BSD socket with a different address family and nothing else.
//
// STATUS: syntax- and type-checked only, never compiled against real BlueZ headers,
// never linked against libbluetooth, never connected to a printer. See the long note
// at the top of core/src/transport_bluez.cpp and
// scripts/check_linux_bluetooth_syntax.sh.

#if defined(__linux__) || defined(PD_FORCE_LINUX_BLUETOOTH)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "printerdriver/transport.hpp"

namespace pd {

struct BluezRfcommConfig {
  // "00:11:22:33:44:55" — the paired device's address. Pairing itself is an operator
  // action through the system's own agent; this transport never pairs, because a
  // printing SDK that can pair can also pair with the wrong printer.
  std::string address;
  // 0 selects channel 1, which is what the receipt printers in docs/compatibility-brief.md
  // advertise for SPP. Discovering it properly needs an SDP client, which would pull
  // libbluetooth's SDP layer into a core that has no third-party dependencies.
  uint8_t channel = 0;
};

class BluezRfcommTransport : public Transport {
 public:
  explicit BluezRfcommTransport(BluezRfcommConfig config);
  ~BluezRfcommTransport() override;

  using Transport::write;  // the vector overload, hidden by the override below

  void onBytes(BytesCallback callback) override;
  void onDisconnected(DisconnectCallback callback) override;

  TransportResult connect() override;
  TransportResult write(const uint8_t* data, size_t size) override;
  void close() override;
  bool isConnected() const override;
  std::string describe() const override;

  const BluezRfcommConfig& config() const noexcept { return config_; }

 private:
  void readerLoop();

  BluezRfcommConfig config_;
  BytesCallback on_bytes_;
  DisconnectCallback on_disconnected_;

  mutable std::mutex mutex_;
  int fd_ = -1;
  std::thread reader_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<bool> notified_{false};
};

TransportFactory bluezRfcomm(BluezRfcommConfig config);

}  // namespace pd

#endif  // __linux__ || PD_FORCE_LINUX_BLUETOOTH
