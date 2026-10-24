#include "tdu2_config.h"

#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace tdu2 {
namespace {

std::string TomlQuote(const std::string& value) {
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') {
      quoted += '\\';
    }
    quoted += c;
  }
  quoted += '"';
  return quoted;
}

// Replaces the value of a flat `key = ...` line, preserving everything else in
// the file including comments. Returns false when the key was not present.
//
// The separator is matched with optional surrounding whitespace: a hand-written
// config normally reads `key = value`, while older code wrote `key=value`.
// Matching the bare `key=` prefix silently missed the former and appended a
// duplicate key instead of replacing it.
bool ReplaceTomlValue(std::vector<std::string>& lines, const std::string& key,
                      const std::string& value_text) {
  for (auto& line : lines) {
    if (line.rfind(key, 0) != 0) {
      continue;
    }
    size_t pos = key.size();
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
      ++pos;
    }
    if (pos >= line.size() || line[pos] != '=') {
      continue;  // a different key that merely starts with the same text
    }
    line = key + " = " + value_text;
    return true;
  }
  return false;
}

}  // namespace

bool SaveConfigValues(const std::vector<ConfigValue>& values) {
  namespace fs = std::filesystem;
  std::error_code ec;

  const fs::path config = fs::current_path(ec) / "tdu2.toml";
  if (ec) {
    REXLOG_WARN("Could not resolve current directory: {}", ec.message());
    return false;
  }
  if (!fs::exists(config, ec)) {
    REXLOG_WARN("No tdu2.toml next to the exe; settings not saved");
    return false;
  }

  std::ifstream in(config, std::ios::binary);
  if (!in) {
    REXLOG_WARN("Could not open tdu2.toml for reading");
    return false;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();  // avoid accumulating CR in CRLF files
    }
    lines.push_back(line);
  }
  in.close();

  for (const auto& kv : values) {
    const std::string value_text = kv.quoted ? TomlQuote(kv.value) : kv.value;
    if (!ReplaceTomlValue(lines, kv.key, value_text)) {
      lines.push_back(kv.key + " = " + value_text);
    }
  }

  std::ofstream out(config, std::ios::binary | std::ios::trunc);
  if (!out) {
    REXLOG_WARN("Could not open tdu2.toml for writing");
    return false;
  }
  for (const auto& l : lines) {
    out << l << "\n";
  }
  out.close();
  if (!out) {
    REXLOG_WARN("Failed while writing tdu2.toml");
    return false;
  }
  return true;
}

bool SaveCvarsToConfig(const std::vector<std::string>& names) {
  std::vector<ConfigValue> values;
  values.reserve(names.size());
  for (const auto& name : names) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
    if (info == nullptr) {
      REXLOG_WARN("Not saving unknown cvar {}", name);
      continue;
    }
    values.push_back({name, rex::cvar::GetFlagByName(name),
                      info->type == rex::cvar::FlagType::String});
  }
  return SaveConfigValues(values);
}

}  // namespace tdu2
