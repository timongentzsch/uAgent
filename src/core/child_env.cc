// Copyright 2026 Timon Gentzsch

#include "include/core/child_env.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/platform.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

std::string KeyOf(std::string_view entry) {
  size_t equals = entry.find('=');
  return std::string(entry.substr(0, equals));
}

// The keys an approved shell may keep despite looking sensitive. Parsed once
// per child rather than once per inherited variable.
std::vector<std::string> ShellAllowList() {
  std::vector<std::string> allowed;
  const char* configured = getenv("UAGENT_SHELL_ENV_ALLOW");
  if (!configured) return allowed;
  for (std::string entry : SplitPathList(configured, ',')) {
    entry = Trim(entry);
    if (!entry.empty()) allowed.push_back(std::move(entry));
  }
  return allowed;
}

bool SensitiveEnvironmentKey(std::string_view key) {
  std::string lower = AsciiLower(std::string(key));
  if (lower == "uagent_api_key" || lower == "uagent_providers" ||
      lower == "uagent_session_budget" || lower == "uagent_usage_file" ||
      lower == "openrouter_api_key" || lower == "ssh_auth_sock" ||
      lower == "xauthority") {
    return true;
  }
  static constexpr std::string_view kMarkers[] = {
      "_api_key",  "_access_key", "_private_key",  "_token",      "_secret",
      "_password", "_credential", "authorization", "_auth_token", "_cookie",
  };
  return std::any_of(std::begin(kMarkers), std::end(kMarkers),
                     [&](std::string_view marker) {
                       return lower.find(marker) != std::string::npos;
                     });
}

}  // namespace

ChildEnvironment::ChildEnvironment(const EnvironmentOverrides& overrides,
                                   ChildEnvironmentPolicy policy) {
  std::vector<std::string> allow_list;
  if (policy == ChildEnvironmentPolicy::kApprovedShell) {
    allow_list = ShellAllowList();
  }
  for (char** current = ProcessEnvironment(); current && *current; ++current) {
    std::string entry(*current);
    std::string key = KeyOf(entry);
    bool allowed = std::find(allow_list.begin(), allow_list.end(), key) !=
                   allow_list.end();
    if (!SensitiveEnvironmentKey(key) || allowed) {
      values_.push_back(std::move(entry));
    }
  }
  for (const auto& [key, value] : overrides) {
    values_.erase(std::remove_if(values_.begin(), values_.end(),
                                 [&](const std::string& entry) {
                                   return KeyOf(entry) == key;
                                 }),
                  values_.end());
    values_.push_back(key + "=" + value);
  }
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
