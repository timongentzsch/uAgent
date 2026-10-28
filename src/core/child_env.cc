// Copyright 2026 Timon Gentzsch

#include "include/core/child_env.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/platform.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

std::string KeyOf(std::string_view entry) {
  size_t equals = entry.find('=');
  return std::string(entry.substr(0, equals));
}

// A comma-separated setting as a list of names. Parsed once per child rather
// than once per inherited variable.
std::vector<std::string> NameList(const ConfigDescriptor& setting) {
  std::vector<std::string> names;
  for (std::string entry : SplitPathList(StringSetting(setting), ',')) {
    entry = Trim(entry);
    if (!entry.empty()) {
      names.push_back(std::move(entry));
    }
  }
  return names;
}

bool SensitiveEnvironmentKey(std::string_view key) {
  std::string lower = AsciiLower(std::string(key));
  if (std::ranges::find(kHiddenEnvNames, lower) != std::end(kHiddenEnvNames)) {
    return true;
  }
  if (CredentialLikeKey(lower)) return true;
  for (std::string_view marker : kHiddenEnvMarkers) {
    if (lower.find(marker) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

ChildEnvironment::ChildEnvironment(const EnvironmentOverrides& overrides,
                                   ChildEnvironmentPolicy policy) {
  std::vector<std::string> allow_list;
  if (policy == ChildEnvironmentPolicy::kApprovedShell) {
    allow_list = NameList(Cfg("UAGENT_SHELL_ENV_ALLOW"));
  }
  std::vector<std::string> hidden = NameList(Cfg("UAGENT_SANDBOX_HIDE_ENV"));
  for (std::string& name : hidden) {
    name = AsciiLower(name);
  }
  const bool agent = policy == ChildEnvironmentPolicy::kAgent;
  const bool user_shell = policy == ChildEnvironmentPolicy::kUserShell;
  auto permitted = [&](const std::string& key) {
    // A name the user hides stays hidden, whoever asks.
    if (std::ranges::find(hidden, AsciiLower(key)) != hidden.end()) {
      return false;
    }
    // What the user lets an approved shell have by name, it has, a
    // credential of uagent's own included.
    if (std::ranges::find(allow_list, key) != allow_list.end()) return true;
    // The person's own terminal pushes with their keys, as any terminal does.
    if (user_shell && key == "SSH_AUTH_SOCK") return true;
    return (agent || !AgentConfigKey(key)) && !SensitiveEnvironmentKey(key);
  };
  auto set = [&](const std::string& key, const std::string& value) {
    std::erase_if(
        values_, [&](const std::string& entry) { return KeyOf(entry) == key; });
    values_.push_back(key + "=" + value);
  };
  for (char** current = ProcessEnvironment(); current && *current; ++current) {
    std::string entry(*current);
    if (permitted(KeyOf(entry))) values_.push_back(std::move(entry));
  }
  // environ predates reloads and session choices.
  if (agent) {
    for (const auto& [key, value] : CurrentSettings()) {
      if (permitted(key)) set(key, value);
    }
  }
  // A uagent started from a command is no freer than the session it runs in;
  // one the person starts in their own terminal chooses for itself.
  if (!user_shell) {
    set("UAGENT_APPROVAL", ApprovalModeName(CurrentApprovalMode()));
  }
  for (const auto& [key, value] : overrides) set(key, value);
  pointers_.reserve(values_.size() + 1);
  for (std::string& value : values_) pointers_.push_back(value.data());
  pointers_.push_back(nullptr);
}

bool ChildEnvironment::Contains(std::string_view key) const {
  return std::any_of(
      values_.begin(), values_.end(),
      [&](const std::string& entry) { return KeyOf(entry) == key; });
}

}  // namespace uagent
