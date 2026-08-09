#include "printerdriver/platform.hpp"

#if PD_PLATFORM_WINDOWS

#include "test_harness.hpp"

// The Windows serial path would be Win32 COM handles and a DCB, which this core does not
// carry; SerialTransport::supported() says so rather than pretending.
PD_TEST(serial_is_honestly_unsupported_on_this_platform) {
  CHECK(!pd::SerialTransport::supported());
}

#else

#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fake_printer.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/transport.hpp"
#include "test_harness.hpp"

// M13b. RS-232 over POSIX termios, exercised end to end.
//
// A pty pair is the only way to test a serial transport without hardware, and it is a
// genuinely good one: /dev/ptmx gives a real character device with a real line discipline,
// so cfmakeraw actually has to be right or the 0x11 and 0x13 bytes inside a raster block
// come out mangled. The scripted printer on the far side is the same pdfake::FakePrinter
// the TCP engine tests use, so a fence that passes here passes for the same reasons it
// passes there — which is the point: the completion story is the transport's business
// only insofar as bytes move.

using namespace pd;

namespace {

// A pseudo-terminal pair. The driver opens the slave by path exactly as it would open
// /dev/ttyUSB0; the test drives the master.
class PtyPair {
 public:
  ~PtyPair() { close(); }

  bool open() {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master_ < 0) {
      return false;
    }
    if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
      close();
      return false;
    }
    const char* name = ::ptsname(master_);
    if (name == nullptr) {
      close();
      return false;
    }
    slave_path_ = name;
    // Raw on the master side too, so nothing echoes our own writes back at us and turns
    // a status byte into an infinite loop.
    termios settings{};
    if (::tcgetattr(master_, &settings) == 0) {
      ::cfmakeraw(&settings);
      ::tcsetattr(master_, TCSANOW, &settings);
    }
    return true;
  }

  int master() const { return master_; }
  const std::string& slavePath() const { return slave_path_; }

  void close() {
    if (master_ >= 0) {
      ::close(master_);
      master_ = -1;
    }
  }

 private:
  int master_ = -1;
  std::string slave_path_;
};

// Reads the master end, hands the bytes to the scripted printer, writes its answers back.
class PtyPrinter {
 public:
  PtyPrinter(int master, std::shared_ptr<pdfake::FakePrinter> device)
      : master_(master), device_(std::move(device)) {
    thread_ = std::thread([this] { run(); });
  }
  ~PtyPrinter() { stop(); }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void run() {
    std::vector<uint8_t> buffer(4096);
    // Non-blocking with a short sleep rather than a blocking read: the loop has to notice
    // stop() without needing the far end to send anything.
    const int flags = ::fcntl(master_, F_GETFL, 0);
    ::fcntl(master_, F_SETFL, flags | O_NONBLOCK);
    while (running_.load()) {
      const ssize_t got = ::read(master_, buffer.data(), buffer.size());
      if (got > 0) {
        const std::vector<uint8_t> response =
            device_->receive(buffer.data(), static_cast<size_t>(got));
        size_t sent = 0;
        while (sent < response.size() && running_.load()) {
          const ssize_t wrote =
              ::write(master_, response.data() + sent, response.size() - sent);
          if (wrote > 0) {
            sent += static_cast<size_t>(wrote);
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  int master_;
  std::shared_ptr<pdfake::FakePrinter> device_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

CapabilityProfile serialProfile(CompletionMechanism mechanism) {
  CapabilityProfile profile = pdfake::fastProfile(mechanism);
  // A pty carries data as fast as it is written; the budgets only have to cover the
  // scheduling round trip between two threads.
  profile.completion_timeout_ms = 2000;
  profile.preflight_timeout_ms = 1000;
  return profile;
}

}  // namespace

PD_TEST(serial_is_supported_and_describes_its_frame) {
  CHECK(SerialTransport::supported());
  SerialConfig config;
  config.device = "/dev/ttyUSB0";
  config.baud = 19200;
  SerialTransport transport(config);
  CHECK_EQ(transport.describe(), std::string("serial:///dev/ttyUSB0@19200"));
  CHECK(!transport.isConnected());
}

PD_TEST(serial_refuses_a_frame_it_cannot_actually_set) {
  PtyPair pty;
  CHECK(pty.open());

  // A rate outside the documented table is refused rather than rounded to a neighbour: a
  // port opened at the wrong speed does not fail, it prints noise, and rounding would
  // make that the SDK's fault instead of a visible configuration error.
  SerialConfig config;
  config.device = pty.slavePath();
  config.baud = 12345;
  SerialTransport bad_baud(config);
  const TransportResult baud_result = bad_baud.connect();
  CHECK(!baud_result.ok);
  CHECK(baud_result.message.find("baud") != std::string::npos);

  config.baud = 9600;
  config.data_bits = 9;
  SerialTransport bad_bits(config);
  CHECK(!bad_bits.connect().ok);

  config.data_bits = 8;
  config.stop_bits = 3;
  SerialTransport bad_stop(config);
  CHECK(!bad_stop.connect().ok);
}

PD_TEST(serial_missing_device_fails_without_pretending_to_open_it) {
  SerialConfig config;
  config.device = "/dev/pd-nonexistent-serial-device";
  SerialTransport transport(config);
  const TransportResult result = transport.connect();
  CHECK(!result.ok);
  CHECK_EQ(result.error, TransportError::ConnectFailed);
  CHECK(!transport.isConnected());
}

PD_TEST(serial_carries_bytes_both_ways_over_a_pty) {
  PtyPair pty;
  CHECK(pty.open());
  auto device = std::make_shared<pdfake::FakePrinter>();
  PtyPrinter far_end(pty.master(), device);

  SerialConfig config;
  config.device = pty.slavePath();
  config.baud = 115200;
  SerialTransport transport(config);

  std::mutex mutex;
  std::vector<uint8_t> received;
  transport.onBytes([&mutex, &received](const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    received.insert(received.end(), data, data + size);
  });
  transport.onDisconnected([](TransportError, const std::string&) {});

  CHECK(transport.connect().ok);
  CHECK(transport.isConnected());

  const escpos::Bytes query = escpos::dleEot(escpos::DleEotKind::PrinterStatus);
  CHECK(transport.write(query).ok);

  for (int i = 0; i < 400; ++i) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!received.empty()) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(received.size(), static_cast<size_t>(1));
    if (!received.empty()) {
      // The scripted device's healthy DLE EOT 1 answer, unmangled by the line discipline.
      CHECK_EQ(static_cast<int>(received[0]), 0x16);
    }
  }

  transport.close();
  CHECK(!transport.isConnected());
  far_end.stop();
}

PD_TEST(serial_runs_the_whole_gs_h_fence_over_a_pty) {
  PtyPair pty;
  CHECK(pty.open());
  auto device = std::make_shared<pdfake::FakePrinter>();
  PtyPrinter far_end(pty.master(), device);

  SerialConfig config;
  config.device = pty.slavePath();
  config.baud = 115200;
  config.flow = SerialFlowControl::None;

  StorageConfig storage;  // in-memory
  PrinterDriver driver(storage);
  PrinterConfig printer_config;
  printer_config.id = "serial-under-test";
  printer_config.transport = serial(config);
  printer_config.profile = serialProfile(CompletionMechanism::GsParenH);
  auto printer = driver.addPrinter(printer_config);

  escpos::Encoder encoder;
  encoder.line("SERIAL TICKET");
  auto job = printer->print(Payload::document(encoder.take(), escpos::CodePage::PC437));
  const JobResult result = job->result();

  // The whole point of the exercise: a fence over RS-232 earns exactly what the same
  // fence earns over Ethernet, because the transport is not the thing making the claim.
  CHECK_EQ(result.outcome, JobOutcome::Done);
  CHECK_EQ(result.grade, ConfidenceGrade::A_JobLevelConfirmation);
  CHECK_EQ(result.authority, CompletionAuthority::PhysicalPrinter);
  CHECK_EQ(result.method, std::string("GS(H) fn48"));
  CHECK(result.confidence >= ConfidenceLevel::CutProcessed);

  CHECK(device->printText().find("SERIAL TICKET") != std::string::npos);
  CHECK_EQ(device->cuts(), static_cast<size_t>(1));
  // Two markers: one behind the payload, one behind the cut — the same sequencing the
  // TCP tests assert, reached over a character device.
  CHECK_EQ(device->markers().size(), static_cast<size_t>(2));
  CHECK(!job->printToken().empty());
  CHECK(!job->cutToken().empty());

  driver.shutdown();
  far_end.stop();
}

PD_TEST(serial_link_loss_fails_a_waiting_job_rather_than_burning_the_budget) {
  auto pty = std::make_shared<PtyPair>();
  CHECK(pty->open());
  auto device = std::make_shared<pdfake::FakePrinter>();
  // The far end never answers a marker, so the job would otherwise sit out its whole
  // completion budget.
  pdfake::Script script;
  script.answer_process_id = false;
  device->setScript(script);
  auto far_end = std::make_shared<PtyPrinter>(pty->master(), device);

  SerialConfig config;
  config.device = pty->slavePath();
  config.baud = 115200;

  StorageConfig storage;
  PrinterDriver driver(storage);
  PrinterConfig printer_config;
  printer_config.id = "serial-drop";
  printer_config.transport = serial(config);
  CapabilityProfile profile = serialProfile(CompletionMechanism::GsParenH);
  profile.completion_timeout_ms = 400;
  printer_config.profile = profile;
  auto printer = driver.addPrinter(printer_config);

  escpos::Encoder encoder;
  encoder.line("DROPPED");
  auto job = printer->print(Payload::document(encoder.take(), escpos::CodePage::PC437));
  const JobResult result = job->result();

  // Bytes went out and nothing came back. Unknown, never Failed — the receipt may be in
  // the customer's hand.
  CHECK_EQ(result.outcome, JobOutcome::Unknown);

  driver.shutdown();
  far_end->stop();
}

#endif  // PD_PLATFORM_WINDOWS
