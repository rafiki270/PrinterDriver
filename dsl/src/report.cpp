#include "printerdriver/dsl/report.hpp"

namespace pd::dsl {

void RenderReport::add(ReportKind kind, std::string block, std::string requested,
                       std::string delivered, RenderPath path, std::string detail) {
  ReportEntry entry;
  entry.kind = kind;
  entry.block = std::move(block);
  entry.requested = std::move(requested);
  entry.delivered = std::move(delivered);
  entry.path = path;
  entry.detail = std::move(detail);
  entries.push_back(std::move(entry));
}

void RenderReport::append(const RenderReport& other) {
  entries.insert(entries.end(), other.entries.begin(), other.entries.end());
}

size_t RenderReport::count(ReportKind kind) const noexcept {
  size_t total = 0;
  for (const ReportEntry& entry : entries) {
    if (entry.kind == kind) {
      ++total;
    }
  }
  return total;
}

std::string RenderReport::toText() const {
  std::string out;
  for (const ReportEntry& entry : entries) {
    out += entry.block.empty() ? "document" : entry.block;
    out += "  ";
    out += to_string(entry.kind);
    out += ": requested \"";
    out += entry.requested;
    out += "\", delivered \"";
    out += entry.delivered;
    out += "\" [";
    out += to_string(entry.path);
    out += "]";
    if (!entry.detail.empty()) {
      out += " - ";
      out += entry.detail;
    }
    out += "\n";
  }
  return out;
}

Json RenderReport::toJson() const {
  Json::Array items;
  items.reserve(entries.size());
  for (const ReportEntry& entry : entries) {
    Json item;
    item.set("block", Json::string(entry.block));
    item.set("requested", Json::string(entry.requested));
    item.set("delivered", Json::string(entry.delivered));
    item.set("path", Json::string(to_string(entry.path)));
    item.set("kind", Json::string(to_string(entry.kind)));
    if (!entry.detail.empty()) {
      item.set("detail", Json::string(entry.detail));
    }
    items.push_back(std::move(item));
  }
  return Json::array(std::move(items));
}

const char* to_string(ReportKind kind) noexcept {
  switch (kind) {
    case ReportKind::MissingPath: return "missingPath";
    case ReportKind::UnknownFormatter: return "unknownFormatter";
    case ReportKind::UnformattableValue: return "unformattableValue";
    case ReportKind::MalformedTemplate: return "malformedTemplate";
    case ReportKind::UnknownStyle: return "unknownStyle";
    case ReportKind::StyleCycle: return "styleCycle";
    case ReportKind::UnsupportedStyle: return "unsupportedStyle";
    case ReportKind::UnsupportedBlock: return "unsupportedBlock";
    case ReportKind::MissingImage: return "missingImage";
    case ReportKind::Truncated: return "truncated";
    case ReportKind::EmptyIteration: return "emptyIteration";
    case ReportKind::UnsupportedTimezone: return "unsupportedTimezone";
    case ReportKind::RawFramingRisk: return "rawFramingRisk";
    case ReportKind::Note: return "note";
  }
  return "note";
}

const char* to_string(RenderPath path) noexcept {
  switch (path) {
    case RenderPath::Hardware: return "hardware";
    case RenderPath::Raster: return "raster";
    case RenderPath::NotRendered: return "notRendered";
  }
  return "hardware";
}

}  // namespace pd::dsl
