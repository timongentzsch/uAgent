// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_JSON_H_
#define UAGENT_INCLUDE_CORE_JSON_H_
// JSON access helpers that never throw: a typed lookup returns the
// fallback rather than raising on a missing key or wrong type.

#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "include/core/checked.h"
#include "third_party/json.hpp"

namespace uagent {

using nlohmann::json;

inline bool JsonValue(const json& object, const char* key,
                      bool fallback = false) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_boolean() ? value->get<bool>()
                                                      : fallback;
}

template <std::integral Integer>
  requires(!std::same_as<Integer, bool>)
inline Integer JsonValue(const json& object, const char* key,
                         Integer fallback) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  if (value == object.end()) return fallback;
  if (value->is_number_unsigned()) {
    uint64_t number = value->get<uint64_t>();
    return number <= static_cast<uint64_t>(std::numeric_limits<Integer>::max())
               ? static_cast<Integer>(number)
               : fallback;
  }
  if (!value->is_number_integer()) return fallback;
  int64_t number = value->get<int64_t>();
  if constexpr (std::signed_integral<Integer>) {
    if (number < static_cast<int64_t>(std::numeric_limits<Integer>::lowest()) ||
        number > static_cast<int64_t>(std::numeric_limits<Integer>::max())) {
      return fallback;
    }
  } else if (number < 0 ||
             static_cast<uint64_t>(number) >
                 static_cast<uint64_t>(std::numeric_limits<Integer>::max())) {
    return fallback;
  }
  return static_cast<Integer>(number);
}

template <std::floating_point Number>
inline Number JsonValue(const json& object, const char* key, Number fallback) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_number()
             ? static_cast<Number>(value->get<double>())
             : fallback;
}

inline std::string JsonValue(const json& object, const char* key,
                             std::string fallback) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_string() ? value->get<std::string>()
                                                     : fallback;
}

inline const std::string* JsonStringRef(const json& object, const char* key) {
  if (!object.is_object()) return nullptr;
  auto value = object.find(key);
  return value != object.end() && value->is_string()
             ? &value->get_ref<const std::string&>()
             : nullptr;
}

// OpenAI-compatible APIs use either {"error":"..."} or
// {"error":{"message":"..."}} for HTTP and streamed failures.
inline std::string JsonErrorMessage(const json& object,
                                    std::string fallback = {}) {
  if (!object.is_object()) return fallback;
  auto error = object.find("error");
  if (error == object.end()) return fallback;
  if (error->is_string()) return error->get<std::string>();
  return error->is_object() ? JsonValue(*error, "message", std::move(fallback))
                            : fallback;
}

inline std::string JsonValue(const json& object, const char* key,
                             const char* fallback) {
  return JsonValue(object, key, std::string(fallback));
}

inline json JsonValue(const json& object, const char* key, json fallback) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value == object.end() ? fallback : *value;
}

#ifdef UAGENT_VERSION
inline constexpr char kVersion[] = UAGENT_VERSION;
#else
inline constexpr char kVersion[] = "dev";
#endif

// External bytes (tool output, filenames, pasted input) may be malformed UTF-8,
// which plain dump() throws on; substitute U+FFFD instead.
inline std::string JsonDump(const json& value, int indent = -1) {
  return value.dump(indent, ' ', false, json::error_handler_t::replace);
}

// Rough serialized size, used for context accounting rather than exactness.
inline size_t JsonEstimatedBytes(const json& value) {
  if (value.is_string()) {
    return SaturatingAdd(value.get_ref<const std::string&>().size(), 2);
  }
  size_t total = 16;
  if (value.is_array()) {
    for (const json& item : value) {
      total = SaturatingAdd(total, JsonEstimatedBytes(item));
    }
  } else if (value.is_object()) {
    for (const auto& [key, item] : value.items()) {
      total = SaturatingAdd(total, key.size());
      total = SaturatingAdd(total, JsonEstimatedBytes(item));
    }
  }
  return total;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_JSON_H_
