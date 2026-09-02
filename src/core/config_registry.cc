// Copyright 2026 Timon Gentzsch

#include "include/core/config_registry.h"

#include <algorithm>
#include <string>

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

int64_t LongSetting(const ConfigDescriptor& descriptor) {
  const int64_t* value = std::get_if<int64_t>(&descriptor.default_value);
  return LongSetting(descriptor, value ? *value : 0);
}

int64_t LongSetting(const ConfigDescriptor& descriptor, int64_t fallback) {
  return std::clamp(EnvLong(descriptor.EnvName(), fallback), descriptor.minimum,
                    descriptor.maximum);
}

bool BoolSetting(const ConfigDescriptor& descriptor) {
  const bool* declared = std::get_if<bool>(&descriptor.default_value);
  bool value = declared && *declared;
  ParseBool(EnvStr(descriptor.EnvName()), value);
  return value;
}

std::string StringSetting(const ConfigDescriptor& descriptor) {
  const std::string_view* value =
      std::get_if<std::string_view>(&descriptor.default_value);
  return EnvStr(descriptor.EnvName(),
                value ? std::string(*value) : std::string());
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
