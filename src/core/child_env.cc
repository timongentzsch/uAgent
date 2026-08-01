// Copyright 2026 Timon Gentzsch

#include "include/core/child_env.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/platform.h"

namespace uagent {
namespace {

std::string KeyOf(std::string_view entry) {
  size_t equals = entry.find('=');
  return std::string(entry.substr(0, equals));
}

bool ContainsMarker(const std::string& key, std::string_view marker) {
  return key.find(marker) != std::string::npos;
}

bool ShellAllows(std::string_view key) {
  const char* configured = getenv("UAGENT_SHELL_ENV_ALLOW");
  if (!configured) return false;
  std::string_view list(configured);
  while (!list.empty()) {
    size_t comma = list.find(',');
    std::string_view item = list.substr(0, comma);
    while (!item.empty() &&
           std::isspace(static_cast<unsigned char>(item.front()))) {
      item.remove_prefix(1);
    }
    while (!item.empty() &&
           std::isspace(static_cast<unsigned char>(item.back()))) {
      item.remove_suffix(1);
    }
    if (item == key) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

}  // namespace

bool SensitiveEnvironmentKey(std::string_view key) {
  std::string upper(key);
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::toupper(value));
                 });
  if (upper == "UAGENT_API_KEY" || upper == "UAGENT_PROVIDERS" ||
      upper == "UAGENT_SESSION_BUDGET" || upper == "UAGENT_USAGE_FILE" ||
      upper == "OPENROUTER_API_KEY" || upper == "SSH_AUTH_SOCK" ||
      upper == "XAUTHORITY") {
    return true;
  }
  static constexpr std::string_view kMarkers[] = {
      "_API_KEY",  "_ACCESS_KEY", "_PRIVATE_KEY",  "_TOKEN",      "_SECRET",
      "_PASSWORD", "_CREDENTIAL", "AUTHORIZATION", "_AUTH_TOKEN", "_COOKIE",
  };
  return std::any_of(
      std::begin(kMarkers), std::end(kMarkers),
      [&](std::string_view marker) { return ContainsMarker(upper, marker); });
}

ChildEnvironment::ChildEnvironment(const EnvironmentOverrides& overrides,
                                   ChildEnvironmentPolicy policy) {
  for (char** current = ProcessEnvironment(); current && *current; ++current) {
    std::string entry(*current);
    std::string key = KeyOf(entry);
    bool allowed =
        policy == ChildEnvironmentPolicy::kApprovedShell && ShellAllows(key);
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
