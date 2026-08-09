#include "printerdriver/registrations.hpp"

#include <algorithm>

// M16 — custom method registration (docs/api.md §16). The registries are add-only vectors
// under one mutex: a registration is a rare, startup-time act, while a lookup happens on
// the hot path of every job/probe/render, so the data structure is chosen for readers.

namespace pd {

bool isValidRegistrationId(const std::string& id) noexcept {
  if (id.empty()) {
    return false;
  }
  for (const char c : id) {
    const auto value = static_cast<unsigned char>(c);
    // No whitespace and no control characters: a registration id names a mechanism in a
    // result and in `pdctl verify`, so it must be a single clean token.
    if (value <= 0x20 || value == 0x7F) {
      return false;
    }
  }
  return true;
}

bool isNonPrintingRequest(const std::vector<uint8_t>& bytes) noexcept {
  for (const uint8_t byte : bytes) {
    // A printable run (0x20-0x7E) prints; a line feed advances paper. Either one turns a
    // "non-printing" probe into a receipt, which is exactly what autoDetect must never do.
    if (byte == 0x0A || (byte >= 0x20 && byte <= 0x7E)) {
      return false;
    }
  }
  return true;
}

namespace {

template <typename Vec>
bool idTaken(const Vec& vec, const std::string& id) {
  return std::any_of(vec.begin(), vec.end(),
                     [&id](const auto& entry) { return entry.id == id; });
}

}  // namespace

bool Registrations::addCompletionMethod(CompletionMethod method, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (!isValidRegistrationId(method.id)) {
    return fail("completion method id must be a non-empty namespaced token");
  }
  if (method.method_name.empty()) {
    method.method_name = method.id;
  }
  if (!method.valid()) {
    return fail("completion method \"" + method.id +
                "\" needs both a fence-bytes and a matcher callback");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (idTaken(completions_, method.id)) {
    return fail("completion method id already registered: " + method.id);
  }
  completions_.push_back(std::move(method));
  return true;
}

std::optional<CompletionMethod> Registrations::completionMethod(
    const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const CompletionMethod& method : completions_) {
    if (method.id == id) {
      return method;
    }
  }
  return std::nullopt;
}

bool Registrations::addProbeStep(ProbeStep step, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (!isValidRegistrationId(step.id)) {
    return fail("probe step id must be a non-empty namespaced token");
  }
  if (!step.valid()) {
    return fail("probe step \"" + step.id + "\" needs a classify callback");
  }
  // The giant-letters rule of docs/api.md §15: a probe step must be non-printing, checked
  // here so the failure lands at registration and not on a venue's paper roll.
  if (!isNonPrintingRequest(step.request_bytes)) {
    return fail("probe step \"" + step.id +
                "\" request bytes contain printable data; a probe step must be "
                "non-printing (no 0x20-0x7E and no line feed)");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (idTaken(probe_steps_, step.id)) {
    return fail("probe step id already registered: " + step.id);
  }
  probe_steps_.push_back(std::move(step));
  return true;
}

std::vector<ProbeStep> Registrations::probeSteps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return probe_steps_;
}

bool Registrations::addDrawerKick(DrawerKickReg kick, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (!isValidRegistrationId(kick.id)) {
    return fail("drawer kick id must be a non-empty namespaced token");
  }
  if (!kick.valid()) {
    return fail("drawer kick \"" + kick.id + "\" needs a kick-bytes callback");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (idTaken(drawer_kicks_, kick.id)) {
    return fail("drawer kick id already registered: " + kick.id);
  }
  drawer_kicks_.push_back(std::move(kick));
  return true;
}

std::optional<DrawerKickReg> Registrations::drawerKick(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const DrawerKickReg& kick : drawer_kicks_) {
    if (kick.id == id) {
      return kick;
    }
  }
  return std::nullopt;
}

bool Registrations::addFormatter(FormatterReg formatter, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (formatter.name.empty()) {
    return fail("formatter name must be non-empty");
  }
  if (!formatter.valid()) {
    return fail("formatter \"" + formatter.name + "\" needs a callback");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::any_of(formatters_.begin(), formatters_.end(),
                  [&formatter](const FormatterReg& entry) {
                    return entry.name == formatter.name;
                  })) {
    return fail("formatter name already registered: " + formatter.name);
  }
  formatters_.push_back(std::move(formatter));
  return true;
}

bool Registrations::hasFormatter(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(formatters_.begin(), formatters_.end(),
                     [&name](const FormatterReg& entry) { return entry.name == name; });
}

std::optional<std::string> Registrations::format(const std::string& name,
                                                 const std::string& value,
                                                 const std::string& args,
                                                 const std::string& locale) const {
  std::function<std::optional<std::string>(const std::string&, const std::string&,
                                           const std::string&)>
      fn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const FormatterReg& formatter : formatters_) {
      if (formatter.name == name) {
        fn = formatter.fn;
        break;
      }
    }
  }
  // Invoked outside the lock: the callback runs arbitrary embedder code and must never
  // hold this registry's mutex while it does.
  if (!fn) {
    return std::nullopt;
  }
  return fn(value, args, locale);
}

bool Registrations::addBlockHandler(BlockHandlerReg handler, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (handler.kind.empty()) {
    return fail("block handler kind must be non-empty");
  }
  if (!handler.valid()) {
    return fail("block handler \"" + handler.kind + "\" needs a callback");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::any_of(block_handlers_.begin(), block_handlers_.end(),
                  [&handler](const BlockHandlerReg& entry) {
                    return entry.kind == handler.kind;
                  })) {
    return fail("block handler kind already registered: " + handler.kind);
  }
  block_handlers_.push_back(std::move(handler));
  return true;
}

bool Registrations::hasBlockHandler(const std::string& kind) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(
      block_handlers_.begin(), block_handlers_.end(),
      [&kind](const BlockHandlerReg& entry) { return entry.kind == kind; });
}

std::optional<BlockRenderResult> Registrations::renderBlock(
    const std::string& kind, const std::string& block_json,
    const std::string& profile_json) const {
  std::function<BlockRenderResult(const std::string&, const std::string&)> fn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const BlockHandlerReg& handler : block_handlers_) {
      if (handler.kind == kind) {
        fn = handler.fn;
        break;
      }
    }
  }
  if (!fn) {
    return std::nullopt;
  }
  return fn(block_json, profile_json);
}

}  // namespace pd
