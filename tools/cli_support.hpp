// Rack Registry - shared command line support.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This header is used by the inspection tool and by the test harnesses. It is
// not installed.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "rack_registry/rack_registry.hpp"

namespace rackregistry::cli {

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

// A deliberately small parser: positional arguments in order, then "--name
// value" or "--name=value" options. Unknown options are rejected rather than
// ignored, so a typo cannot silently change what a command does.
class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
      tokens_.emplace_back(argv[i]);
    }
  }
  explicit Arguments(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}

  [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }
  [[nodiscard]] const std::vector<std::string>& tokens() const noexcept { return tokens_; }

  [[nodiscard]] bool has(std::string_view name) const {
    return values_.find(std::string(name)) != values_.end();
  }

  [[nodiscard]] std::string get(std::string_view name, std::string_view fallback = {}) const {
    const auto found = values_.find(std::string(name));
    if (found == values_.end()) {
      return std::string(fallback);
    }
    return found->second;
  }

  // Parses the token list. `known` lists every accepted option name and `flags`
  // lists the options that take no value; anything else is rejected rather than
  // ignored, so a typo cannot silently change what a command does.
  [[nodiscard]] Status parse(const std::vector<std::string>& known,
                             const std::vector<std::string>& flags) {
    positional_.clear();
    values_.clear();
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      const std::string& token = tokens_[i];
      if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
        std::string name = token.substr(2);
        std::string value;
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
          value = name.substr(equals + 1);
          name = name.substr(0, equals);
        } else if (std::find(flags.begin(), flags.end(), name) != flags.end()) {
          value = "true";
        } else {
          if (i + 1 >= tokens_.size()) {
            return usage_error("option --" + name + " requires a value");
          }
          value = tokens_[++i];
        }
        if (std::find(known.begin(), known.end(), name) == known.end()) {
          return usage_error("unknown option --" + name);
        }
        values_[name] = value;
      } else {
        positional_.push_back(token);
      }
    }
    return Status{};
  }

  [[nodiscard]] const std::vector<std::string>& positional() const noexcept { return positional_; }

  [[nodiscard]] static RackError usage_error(const std::string& message) {
    return make_error(ErrorCode::InvalidArgument, message,
                      ErrorDetail{.operation = "arguments"});
  }

 private:
  std::vector<std::string> tokens_{};
  std::vector<std::string> positional_{};
  std::map<std::string, std::string> values_{};
};

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

inline std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::ostringstream stream;
          stream << "\\u" << std::hex << static_cast<int>(static_cast<unsigned char>(c));
          out.append(stream.str());
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

inline std::string json_string(std::string_view text) { return "\"" + json_escape(text) + "\""; }

struct JsonObject {
  std::vector<std::pair<std::string, std::string>> fields{};

  void add(std::string name, std::string raw_value) {
    fields.emplace_back(std::move(name), std::move(raw_value));
  }
  void add_string(std::string name, std::string_view value) {
    add(std::move(name), json_string(value));
  }
  void add_number(std::string name, std::uint64_t value) {
    add(std::move(name), std::to_string(value));
  }
  void add_bool(std::string name, bool value) { add(std::move(name), value ? "true" : "false"); }

  [[nodiscard]] std::string render() const {
    std::string out = "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      out.append(json_string(fields[i].first));
      out.push_back(':');
      out.append(fields[i].second);
    }
    out.push_back('}');
    return out;
  }
};

// Prints a rejection in a form that is both stable to parse and readable.
inline void print_error(const RackError& error) {
  std::cerr << "error: " << code_name(error.code) << "(" << code_value(error.code) << "): "
            << error.message;
  if (!error.detail.subject.empty()) {
    std::cerr << " [subject=" << error.detail.subject << "]";
  }
  if (error.detail.expected != 0 || error.detail.actual != 0) {
    std::cerr << " [expected=" << error.detail.expected << " actual=" << error.detail.actual << "]";
  }
  if (!error.detail.items.empty()) {
    std::cerr << " [items=" << join(error.detail.items, '|') << "]";
  }
  std::cerr << "\n";
}

// ---------------------------------------------------------------------------
// Common parsing helpers
// ---------------------------------------------------------------------------

inline Result<std::uint64_t> parse_u64(std::string_view text, std::string_view what) {
  if (text.empty() || text.size() > 20) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(what) + " must be a decimal integer",
                      ErrorDetail{.operation = std::string(what), .subject = std::string(text)});
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (!ascii_digit(c)) {
      return make_error(ErrorCode::InvalidArgument,
                        std::string(what) + " must be a decimal integer",
                        ErrorDetail{.operation = std::string(what), .subject = std::string(text)});
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (static_cast<std::uint64_t>(-1) - digit) / 10u) {
      return make_error(ErrorCode::InvalidArgument, std::string(what) + " overflows",
                        ErrorDetail{.operation = std::string(what), .subject = std::string(text)});
    }
    value = value * 10u + digit;
  }
  return value;
}

inline std::vector<std::string> split_csv(std::string_view text) {
  std::vector<std::string> parts;
  if (text.empty()) {
    return parts;
  }
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, comma - start));
    start = comma + 1;
  }
  return parts;
}

// Accepts the canonical mount grammar ("full:[1,5)", "shared:[1,5)/smc:x/2",
// "zero-u") and the floor shorthand "U10-U12" or "U10" for a full-span mount
// over rack units.
inline Result<MountSpan> parse_mount(std::string_view text) {
  if (text == "zero-u" || text.find(':') != std::string_view::npos) {
    return MountSpan::parse(text);
  }
  const auto units = RackUnitRange::parse(text);
  if (!units) {
    return units.error();
  }
  return MountSpan::full_units(units.value());
}

inline Result<Trait> parse_trait(std::string_view text) { return Trait::parse(text); }

inline Result<TraitSet> parse_traits(const std::vector<std::string>& parts) {
  std::vector<Trait> traits;
  traits.reserve(parts.size());
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    const auto trait = Trait::parse(part);
    if (!trait) {
      return trait.error();
    }
    traits.push_back(trait.value());
  }
  return TraitSet::create(std::move(traits));
}

inline Result<std::vector<PowerDomainReference>> parse_power_domains(
    const std::vector<std::string>& parts) {
  std::vector<PowerDomainReference> references;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    const auto reference = PowerDomainReference::parse(part);
    if (!reference) {
      return reference.error();
    }
    references.push_back(reference.value());
  }
  std::sort(references.begin(), references.end(),
            [](const PowerDomainReference& a, const PowerDomainReference& b) { return a < b; });
  for (std::size_t i = 1; i < references.size(); ++i) {
    if (references[i] == references[i - 1]) {
      return make_error(ErrorCode::DuplicateDomainReference,
                        "power domain " + references[i].text() + " appears more than once",
                        ErrorDetail{.operation = "parse_power_domains",
                                    .subject = references[i].text()});
    }
  }
  return references;
}

inline Result<std::vector<CoolingDomainReference>> parse_cooling_domains(
    const std::vector<std::string>& parts) {
  std::vector<CoolingDomainReference> references;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    const auto reference = CoolingDomainReference::parse(part);
    if (!reference) {
      return reference.error();
    }
    references.push_back(reference.value());
  }
  std::sort(references.begin(), references.end(),
            [](const CoolingDomainReference& a, const CoolingDomainReference& b) { return a < b; });
  for (std::size_t i = 1; i < references.size(); ++i) {
    if (references[i] == references[i - 1]) {
      return make_error(ErrorCode::DuplicateDomainReference,
                        "cooling domain " + references[i].text() + " appears more than once",
                        ErrorDetail{.operation = "parse_cooling_domains",
                                    .subject = references[i].text()});
    }
  }
  return references;
}

inline std::uint64_t now_unix_ns() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

// Every option name any command accepts. Passing them all to the parser keeps
// the "unknown option" check meaningful while allowing one shared list.
inline const std::vector<std::string>& all_option_names() {
  static const std::vector<std::string> names = {
      "state",      "writer",      "actor",         "at",           "request-id",
      "json",       "units",       "profile",       "provides",     "power",
      "cooling",    "label",       "expected-generation",
      "expected-membership-generation",             "expected-state", "to",
      "member",     "asset",       "mount",         "member-state", "requires",
      "forbids",    "order",       "from",          "to-generation", "producer",
      "source",     "source-ref",  "source-sequence", "note",       "out",
      "allow-missing",             "recursive"};
  return names;
}

// Options that take no value.
inline const std::vector<std::string>& all_flag_names() {
  static const std::vector<std::string> flags = {"json", "allow-missing", "recursive"};
  return flags;
}

}  // namespace rackregistry::cli
