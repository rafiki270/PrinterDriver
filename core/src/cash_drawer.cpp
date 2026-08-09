#include "printerdriver/cash_drawer.hpp"

#include <algorithm>
#include <fstream>

#include "platform_file.hpp"

namespace pd {

const char* to_string(DrawerState state) noexcept {
  switch (state) {
    case DrawerState::Closed: return "Closed";
    case DrawerState::Open: return "Open";
    case DrawerState::Opening: return "Opening";
    case DrawerState::KickSentUnverified: return "KickSentUnverified";
    case DrawerState::OpenVerified: return "OpenVerified";
    case DrawerState::FailedToOpen: return "FailedToOpen";
    case DrawerState::NoSensor: return "NoSensor";
    case DrawerState::Unknown: return "Unknown";
  }
  return "Unknown";
}

const char* to_string(DrawerPortStandard standard) noexcept {
  switch (standard) {
    case DrawerPortStandard::Epson24V6P6C: return "Epson24V6P6C";
    case DrawerPortStandard::Star24V6P6C: return "Star24V6P6C";
    case DrawerPortStandard::Generic12V6P6C: return "Generic12V6P6C";
    case DrawerPortStandard::Unknown: return "Unknown";
  }
  return "Unknown";
}

const char* to_string(DrawerKickMethod method) noexcept {
  switch (method) {
    case DrawerKickMethod::EpsonEscP: return "EpsonEscP";
    case DrawerKickMethod::EpsonEpos: return "EpsonEpos";
    case DrawerKickMethod::StarPrnt: return "StarPrnt";
    case DrawerKickMethod::BixolonSdk: return "BixolonSdk";
    case DrawerKickMethod::CitizenEscP: return "CitizenEscP";
    case DrawerKickMethod::SnbcEscP: return "SnbcEscP";
    case DrawerKickMethod::Vendor: return "Vendor";
    case DrawerKickMethod::Unsupported: return "Unsupported";
  }
  return "Unsupported";
}

const char* to_string(DrawerStatusMethod method) noexcept {
  switch (method) {
    case DrawerStatusMethod::GsR2: return "GsR2";
    case DrawerStatusMethod::Asb: return "Asb";
    case DrawerStatusMethod::StarSignal: return "StarSignal";
    case DrawerStatusMethod::VendorSdk: return "VendorSdk";
    case DrawerStatusMethod::None: return "None";
  }
  return "None";
}

// --- DrawerCapabilities --------------------------------------------------------------

bool DrawerCapabilities::kickable() const noexcept {
  if (!present || kick.method == DrawerKickMethod::Unsupported) {
    return false;
  }
  // The giant-letters rule. A port nobody has classified gets no current, whatever the
  // command set looks like: the plug that fits is not proof of the pinout behind it.
  if (!electricalKnown()) {
    return false;
  }
  // Only the two ESC/POS-shaped methods are things this engine can put on a wire. The
  // rest describe real vendor paths this core does not speak, and describing them is
  // the point — a Star drawer is driven through appendPeripheral or not at all.
  switch (kick.method) {
    case DrawerKickMethod::EpsonEscP:
    case DrawerKickMethod::CitizenEscP:
    case DrawerKickMethod::SnbcEscP:
      return true;
    case DrawerKickMethod::EpsonEpos:
    case DrawerKickMethod::StarPrnt:
    case DrawerKickMethod::BixolonSdk:
    case DrawerKickMethod::Vendor:
    case DrawerKickMethod::Unsupported:
      return false;
  }
  return false;
}

uint16_t DrawerCapabilities::pulseFor(uint16_t requested_ms) const noexcept {
  uint16_t pulse = requested_ms != 0 ? requested_ms : kick.default_pulse_ms;
  if (kick.max_pulse_ms != 0 && pulse > kick.max_pulse_ms) {
    pulse = kick.max_pulse_ms;
  }
  // Epson programs t1 in 2 ms units, so an odd request is not representable. Rounding
  // down rather than up keeps every clamp in the "less energy into the solenoid"
  // direction.
  pulse = static_cast<uint16_t>(pulse - (pulse % 2));
  if (pulse < 2) {
    pulse = 2;
  }
  return pulse;
}

uint8_t DrawerCapabilities::channelFor(uint8_t requested) const noexcept {
  const uint8_t channels = electrical.channel_count != 0 ? electrical.channel_count : 1;
  uint8_t channel = requested != 0 ? requested : 1;
  if (channel > channels) {
    channel = channels;
  }
  return channel;
}

// --- Sensor arithmetic ----------------------------------------------------------------

bool drawerPinHigh(uint8_t gs_r2_answer) noexcept {
  return (gs_r2_answer & 0x01u) != 0;
}

DrawerState drawerStateFrom(bool pin_high, const DrawerPolarity& polarity) noexcept {
  if (!polarity.calibrated) {
    // Star's warning, obeyed: the meaning of the level depends on the drawer that is
    // plugged in, so an uncalibrated reading is a level and not a state.
    return DrawerState::Unknown;
  }
  const bool open = polarity.high_means_open ? pin_high : !pin_high;
  return open ? DrawerState::Open : DrawerState::Closed;
}

// --- DrawerPolarityStore ---------------------------------------------------------------

DrawerPolarityStore::DrawerPolarityStore(std::string directory, std::string file_name) {
  if (directory.empty()) {
    return;
  }
  if (!platform_file::createDirectory(directory, nullptr)) {
    // A polarity file that cannot be written costs a re-calibration, never a wrong
    // answer: an uncalibrated drawer reports Unknown rather than guessing.
    return;
  }
  if (platform_file::isSeparator(directory.back())) {
    directory.pop_back();
  }
  path_ = directory + "/" + file_name;
  load();
}

void DrawerPolarityStore::load() {
  std::ifstream input(path_);
  if (!input.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    // "<0|1> <printer id>". The flag comes first so the id may contain anything a
    // printer id can contain, spaces included, and is simply the rest of the line.
    if (line.size() < 3 || (line[0] != '0' && line[0] != '1') || line[1] != ' ') {
      continue;
    }
    Record record;
    record.high_means_open = line[0] == '1';
    record.printer_id = line.substr(2);
    if (!record.printer_id.empty()) {
      records_.push_back(std::move(record));
    }
  }
}

void DrawerPolarityStore::flush() const {
  if (path_.empty()) {
    return;
  }
  const std::string temp = path_ + ".tmp";
  const NativeFile file = platform_file::openTruncate(temp, nullptr);
  if (!nativeFileValid(file)) {
    return;
  }
  std::string blob;
  for (const Record& record : records_) {
    blob += record.high_means_open ? '1' : '0';
    blob += ' ';
    blob += record.printer_id;
    blob += '\n';
  }
  const bool ok = platform_file::writeAll(file, blob.data(), blob.size(), nullptr);
  if (ok) {
    platform_file::sync(file, nullptr);
  }
  platform_file::closeFile(file);
  if (ok) {
    platform_file::replaceFile(temp, path_, nullptr);
  } else {
    platform_file::removeFile(temp);
  }
}

std::optional<bool> DrawerPolarityStore::find(const std::string& printer_id) const {
  if (printer_id.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Record& record : records_) {
    if (record.printer_id == printer_id) {
      return record.high_means_open;
    }
  }
  return std::nullopt;
}

void DrawerPolarityStore::save(const std::string& printer_id, bool high_means_open) {
  if (printer_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(records_.begin(), records_.end(),
                                  [&printer_id](const Record& record) {
                                    return record.printer_id == printer_id;
                                  });
  if (found != records_.end()) {
    found->high_means_open = high_means_open;
  } else {
    records_.push_back(Record{printer_id, high_means_open});
  }
  flush();
}

size_t DrawerPolarityStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

}  // namespace pd
