// Copyright 2026 Timon Gentzsch

#include "include/core/config_registry.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

#include "include/core/env.h"
#include "include/core/strings.h"

namespace uagent {

const ConfigDescriptor* FindConfigDescriptor(std::string_view environment) {
  auto found =
      std::find_if(std::begin(kConfigRegistry), std::end(kConfigRegistry),
                   [&](const ConfigDescriptor& descriptor) {
                     return descriptor.environment == environment;
                   });
  return found == std::end(kConfigRegistry) ? nullptr : &*found;
}

namespace {

struct SettingStore {
  std::mutex mutex;
  SettingValues published;
  SettingValues overrides;
};

SettingStore& Settings() {
  // Never destroyed: a thread may still read settings while the process exits.
  static SettingStore& store = *new SettingStore;
  return store;
}

}  // namespace

void PublishSettings(SettingValues values) {
  SettingStore& store = Settings();
  std::lock_guard lock(store.mutex);
  store.published = std::move(values);
}

void OverrideSetting(std::string_view environment, std::string value) {
  SettingStore& store = Settings();
  std::lock_guard lock(store.mutex);
  store.overrides[std::string(environment)] = std::move(value);
}

void ClearSettings() {
  SettingStore& store = Settings();
  std::lock_guard lock(store.mutex);
  store.published.clear();
  store.overrides.clear();
}

SettingValues CurrentSettings() {
  SettingStore& store = Settings();
  std::lock_guard lock(store.mutex);
  SettingValues current = store.overrides;
  current.insert(store.published.begin(), store.published.end());
  return current;
}

std::string SettingText(const ConfigDescriptor& descriptor) {
  {
    SettingStore& store = Settings();
    std::lock_guard lock(store.mutex);
    for (const SettingValues* layer : {&store.overrides, &store.published}) {
      auto found = layer->find(std::string(descriptor.environment));
      if (found != layer->end()) return found->second;
    }
  }
  return EnvStr(descriptor.EnvName());
}

int64_t LongSetting(const ConfigDescriptor& descriptor) {
  const int64_t* declared = std::get_if<int64_t>(&descriptor.default_value);
  int64_t value = declared ? *declared : 0;
  int64_t parsed = 0;
  if (ParseInt64(SettingText(descriptor).c_str(), parsed)) value = parsed;
  return std::clamp(value, descriptor.minimum, descriptor.maximum);
}

bool BoolSetting(const ConfigDescriptor& descriptor) {
  const bool* declared = std::get_if<bool>(&descriptor.default_value);
  bool value = declared && *declared;
  ParseBool(SettingText(descriptor), value);
  return value;
}

std::string StringSetting(const ConfigDescriptor& descriptor) {
  std::string text = SettingText(descriptor);
  if (!text.empty()) return text;
  const std::string_view* value =
      std::get_if<std::string_view>(&descriptor.default_value);
  return value ? std::string(*value) : std::string();
}

double DoubleSetting(const ConfigDescriptor& descriptor) {
  const double* declared = std::get_if<double>(&descriptor.default_value);
  double value = declared ? *declared : 0.0;
  const std::string text = SettingText(descriptor);
  if (!text.empty() && !ParseFiniteDouble(text.c_str(), value)) {
    value = declared ? *declared : 0.0;
  }
  return value;
}

const char* ConfigTypeName(ConfigType type) {
  switch (type) {
    case ConfigType::kInt:
      return "integer";
    case ConfigType::kDouble:
      return "number";
    case ConfigType::kBool:
      return "boolean";
    case ConfigType::kString:
      return "string";
  }
  return "string";
}

const char* ReloadPolicyName(ReloadPolicy policy) {
  return policy == ReloadPolicy::kNextUserTurn ? "next-user-turn"
                                               : "restart-required";
}

const char* SensitivityName(Sensitivity sensitivity) {
  switch (sensitivity) {
    case Sensitivity::kPublic:
      return "public";
    case Sensitivity::kSecret:
      return "secret";
    case Sensitivity::kCompositeSecret:
      return "composite-secret";
  }
  return "secret";
}

}  // namespace uagent
