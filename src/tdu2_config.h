// tdu2 - In-place edits of tdu2.toml.
//
// The SDK's rex::cvar::SaveConfig regenerates the whole file from the modified
// cvars, which would throw away the comments in tdu2.toml that document why
// each setting is what it is. These helpers rewrite only the `key = value`
// lines that changed and append keys that are missing.

#pragma once

#include <string>
#include <vector>

namespace tdu2 {

struct ConfigValue {
  std::string key;
  std::string value;
  // Strings are written quoted (and escaped), everything else bare.
  bool quoted = false;
};

// Writes the values into tdu2.toml next to the exe, which is the file the
// runtime reads at startup. Returns false if the file is missing or not
// writable.
bool SaveConfigValues(const std::vector<ConfigValue>& values);

// Writes the current values of the named cvars, quoting string cvars.
bool SaveCvarsToConfig(const std::vector<std::string>& names);

}  // namespace tdu2
