#include "printerdriver/dsl/bind.hpp"

#include <algorithm>
#include <vector>

#include "printerdriver/dsl/text.hpp"

namespace pd::dsl {
namespace {

bool isDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Saturating, so "{{items[99999999999999999999]}}" is an out-of-range index rather than
// an out_of_range exception thrown from the middle of a receipt.
size_t parseIndex(std::string_view text) {
  size_t value = 0;
  for (const char c : text) {
    if (value > (static_cast<size_t>(-1) - 9) / 10) {
      return static_cast<size_t>(-1);
    }
    value = value * 10 + static_cast<size_t>(c - '0');
  }
  return value;
}

// "order.items[0].name" and "order.items.0.name" are the same path.
std::vector<std::string> splitPath(std::string_view path) {
  std::vector<std::string> segments;
  std::string current;
  for (size_t i = 0; i < path.size(); ++i) {
    const char c = path[i];
    if (c == '.') {
      if (!current.empty()) {
        segments.push_back(current);
        current.clear();
      }
      continue;
    }
    if (c == '[') {
      if (!current.empty()) {
        segments.push_back(current);
        current.clear();
      }
      const size_t close = path.find(']', i);
      if (close == std::string_view::npos) {
        break;
      }
      segments.emplace_back(path.substr(i + 1, close - i - 1));
      i = close;
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    segments.push_back(current);
  }
  return segments;
}

const Json* descend(const Json* value, const std::vector<std::string>& segments,
                    size_t from) {
  for (size_t i = from; i < segments.size() && value != nullptr; ++i) {
    const std::string& segment = segments[i];
    if (value->isArray() && isDigits(segment)) {
      value = value->at(parseIndex(segment));
      continue;
    }
    value = value->find(segment);
  }
  return value;
}

// One `each` element (or the model root), plus a link outward.
struct Scope {
  const Json* value = nullptr;
  const Scope* parent = nullptr;
  long long index = -1;
  size_t count = 0;
};

const Json* resolve(const Scope& scope, std::string_view path, Json* scratch) {
  const std::string trimmed(path);
  if (trimmed.empty()) {
    return nullptr;
  }
  if (trimmed == "." || trimmed == "this") {
    return scope.value;
  }
  if (trimmed == "@index" || trimmed == "@first" || trimmed == "@last") {
    const Scope* current = &scope;
    while (current != nullptr && current->index < 0) {
      current = current->parent;
    }
    if (current == nullptr) {
      return nullptr;
    }
    if (trimmed == "@index") {
      *scratch = Json::number(static_cast<double>(current->index));
    } else if (trimmed == "@first") {
      *scratch = Json::boolean(current->index == 0);
    } else {
      *scratch = Json::boolean(static_cast<size_t>(current->index) + 1 == current->count);
    }
    return scratch;
  }

  const std::vector<std::string> segments = splitPath(trimmed);
  if (segments.empty()) {
    return nullptr;
  }
  // Mustache scoping: the nearest scope that knows the first segment owns the path.
  for (const Scope* current = &scope; current != nullptr; current = current->parent) {
    if (current->value == nullptr) {
      continue;
    }
    const Json* head = nullptr;
    if (current->value->isArray() && isDigits(segments.front())) {
      head = current->value->at(parseIndex(segments.front()));
    } else {
      head = current->value->find(segments.front());
    }
    if (head != nullptr) {
      return descend(head, segments, 1);
    }
  }
  return nullptr;
}

struct Binder {
  const Document* document = nullptr;
  const BindOptions* options = nullptr;
  RenderReport* report = nullptr;
  FormatContext context;
  size_t emitted = 0;
  bool overflowed = false;

  std::string substitute(std::string_view source, const Scope& scope,
                         const std::string& where);
  void bindBlocks(const BlockList& source, const Scope& scope, const std::string& where,
                  size_t depth, BlockList* out);
  void bindLeaf(const Block& source, const Scope& scope, const std::string& where,
                BlockList* out);
};

std::string Binder::substitute(std::string_view source, const Scope& scope,
                               const std::string& where) {
  if (source.find("{{") == std::string_view::npos) {
    return std::string(source);
  }
  std::string out;
  size_t i = 0;
  while (i < source.size()) {
    // A backslash escapes an opening brace pair: "\{{" prints "{{".
    if (source[i] == '\\' && i + 2 < source.size() && source[i + 1] == '{' &&
        source[i + 2] == '{') {
      out += "{{";
      i += 3;
      continue;
    }
    if (source[i] != '{' || i + 1 >= source.size() || source[i + 1] != '{') {
      out.push_back(source[i]);
      ++i;
      continue;
    }
    const size_t close = source.find("}}", i + 2);
    if (close == std::string_view::npos) {
      if (report != nullptr) {
        report->add(ReportKind::MalformedTemplate, where, std::string(source.substr(i)),
                    std::string(source.substr(i)), RenderPath::Hardware,
                    "unterminated {{ left verbatim");
      }
      out += source.substr(i);
      break;
    }
    std::string expression(source.substr(i + 2, close - i - 2));
    i = close + 2;

    // Split "path|fmt:arg|fmt" — the path is everything before the first pipe.
    std::vector<std::string> formatters;
    std::string path = expression;
    const size_t pipe = expression.find('|');
    if (pipe != std::string::npos) {
      path = expression.substr(0, pipe);
      size_t start = pipe + 1;
      while (start <= expression.size()) {
        const size_t next = expression.find('|', start);
        formatters.push_back(expression.substr(
            start, next == std::string::npos ? std::string::npos : next - start));
        if (next == std::string::npos) {
          break;
        }
        start = next + 1;
      }
    }
    const auto trim = [](std::string& target) {
      while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) {
        target.erase(target.begin());
      }
      while (!target.empty() && (target.back() == ' ' || target.back() == '\t')) {
        target.pop_back();
      }
    };
    trim(path);

    if (path.empty()) {
      if (report != nullptr) {
        report->add(ReportKind::MalformedTemplate, where, "{{" + expression + "}}", "",
                    RenderPath::Hardware, "empty path");
      }
      continue;
    }

    Json scratch;
    const Json* value = resolve(scope, path, &scratch);
    if (value == nullptr) {
      // docs/receipt-dsl.md: missing path → declared warning + empty string.
      if (report != nullptr) {
        report->add(ReportKind::MissingPath, where, "{{" + expression + "}}", "",
                    RenderPath::Hardware, "no such path in the model");
      }
      continue;
    }

    Json current = *value;
    std::string rendered = valueToText(current);
    if (formatters.empty() && (current.isArray() || current.isObject())) {
      // A structure has no receipt representation. It still prints — as compact JSON,
      // which is at least debuggable — and the report says the template asked for
      // something a line of text cannot be.
      if (report != nullptr) {
        report->add(ReportKind::UnformattableValue, where, "{{" + expression + "}}",
                    rendered, RenderPath::Hardware,
                    "the path resolves to a structure, not a value");
      }
    }
    for (std::string& formatter : formatters) {
      trim(formatter);
      if (formatter.empty()) {
        continue;
      }
      const FormatOutcome outcome = applyFormatter(current, formatter, context);
      if (!outcome.ok && report != nullptr) {
        report->add(outcome.kind, where, "{{" + expression + "}}", outcome.text,
                    RenderPath::Hardware, outcome.detail);
      }
      rendered = outcome.text;
      current = Json::string(rendered);
    }
    out += text::sanitize(rendered);
  }
  return out;
}

void Binder::bindLeaf(const Block& source, const Scope& scope, const std::string& where,
                      BlockList* out) {
  Block block = source;
  switch (source.kind) {
    case Block::Kind::Text:
    case Block::Kind::Qr:
    case Block::Kind::Barcode:
      block.content = substitute(source.content, scope, where);
      break;
    case Block::Kind::Columns:
      for (size_t i = 0; i < block.cells.size(); ++i) {
        block.cells[i].content = substitute(source.cells[i].content, scope,
                                            where + ".cells[" + std::to_string(i) + "]");
      }
      break;
    case Block::Kind::Image:
      block.image_ref = substitute(source.image_ref, scope, where);
      break;
    default:
      break;
  }
  out->push_back(std::move(block));
  ++emitted;
}

void Binder::bindBlocks(const BlockList& source, const Scope& scope,
                        const std::string& where, size_t depth, BlockList* out) {
  if (depth > options->max_depth) {
    if (report != nullptr) {
      report->add(ReportKind::MalformedTemplate, where, "nested groups", "stopped",
                  RenderPath::NotRendered, "deeper than the configured limit");
    }
    return;
  }
  for (size_t i = 0; i < source.size(); ++i) {
    const Block& block = source[i];
    const std::string block_where = where + "[" + std::to_string(i) + "]";
    if (emitted >= options->max_blocks) {
      if (!overflowed && report != nullptr) {
        report->add(ReportKind::MalformedTemplate, block_where, "more blocks", "stopped",
                    RenderPath::NotRendered,
                    "the bound document exceeded " + std::to_string(options->max_blocks) +
                        " blocks");
      }
      overflowed = true;
      return;
    }
    switch (block.kind) {
      case Block::Kind::Each: {
        Json scratch;
        const Json* values = resolve(scope, block.path, &scratch);
        if (values == nullptr || values->isNull()) {
          if (report != nullptr) {
            report->add(ReportKind::EmptyIteration, block_where, "each:" + block.path,
                        "0 iterations", RenderPath::NotRendered,
                        "no such path in the model");
          }
          break;
        }
        if (!values->isArray()) {
          if (report != nullptr) {
            report->add(ReportKind::EmptyIteration, block_where, "each:" + block.path,
                        "0 iterations", RenderPath::NotRendered,
                        "the path is not an array");
          }
          break;
        }
        const Json::Array& items = values->asArray();
        for (size_t index = 0; index < items.size(); ++index) {
          Scope element;
          element.value = &items[index];
          element.parent = &scope;
          element.index = static_cast<long long>(index);
          element.count = items.size();
          bindBlocks(block.body, element,
                     block_where + "{" + std::to_string(index) + "}", depth + 1, out);
        }
        break;
      }
      case Block::Kind::If: {
        Json scratch;
        const Json* value = resolve(scope, block.path, &scratch);
        // A missing path is falsy and is not a warning: that is what optional means.
        const bool truthy = value != nullptr && value->truthy();
        if (truthy != block.negated) {
          bindBlocks(block.body, scope, block_where, depth + 1, out);
        }
        break;
      }
      default:
        bindLeaf(block, scope, block_where, out);
        break;
    }
  }
}

}  // namespace

const Json* lookupPath(const Json& model, std::string_view path) {
  const std::vector<std::string> segments = splitPath(path);
  if (segments.empty()) {
    return &model;
  }
  return descend(&model, segments, 0);
}

BindOutcome bind(const Document& document, const Json& model,
                 const BindOptions& options) {
  BindOutcome outcome;
  outcome.document.version = document.version;
  outcome.document.is_template = false;
  outcome.document.meta = document.meta;
  outcome.document.styles = document.styles;

  Binder binder;
  binder.document = &document;
  binder.options = &options;
  binder.report = &outcome.report;
  binder.context.tables = &options.tables;
  binder.context.locale =
      options.locale.empty() ? document.meta.locale : options.locale;
  binder.context.currency =
      options.currency.empty() ? document.meta.currency : options.currency;
  binder.context.tz = options.tz.empty() ? document.meta.tz : options.tz;

  Scope root;
  root.value = &model;
  binder.bindBlocks(document.blocks, root, "blocks", 0, &outcome.document.blocks);
  return outcome;
}

std::string bindString(std::string_view source, const Json& model,
                       const BindOptions& options, RenderReport* report,
                       const std::string& where) {
  Binder binder;
  binder.options = &options;
  binder.report = report;
  binder.context.tables = &options.tables;
  binder.context.locale = options.locale;
  binder.context.currency = options.currency;
  binder.context.tz = options.tz;
  Scope root;
  root.value = &model;
  return binder.substitute(source, root, where);
}

}  // namespace pd::dsl
