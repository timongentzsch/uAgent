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

#include "third_party/json.hpp"

namespace uagent {

using nlohmann::json;

inline bool JsonBool(const json& object, const char* key,
                     bool fallback = false) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_boolean() ? value->get<bool>()
                                                      : fallback;
}

inline int64_t JsonInt(const json& object, const char* key,
                       int64_t fallback = 0) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  if (value == object.end()) return fallback;
  if (value->is_number_unsigned()) {
    uint64_t number = value->get<uint64_t>();
    return number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(number)
               : fallback;
  }
  return value->is_number_integer() ? value->get<int64_t>() : fallback;
}

inline double JsonNumber(const json& object, const char* key,
                         double fallback = 0.0) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_number() ? value->get<double>()
                                                     : fallback;
}

inline std::string JsonString(const json& object, const char* key,
                              std::string fallback = {}) {
  if (!object.is_object()) return fallback;
  auto value = object.find(key);
  return value != object.end() && value->is_string() ? value->get<std::string>()
                                                     : fallback;
}

// OpenAI-compatible APIs use either {"error":"..."} or
// {"error":{"message":"..."}} for HTTP and streamed failures.
inline std::string JsonErrorMessage(const json& object,
                                    std::string fallback = {}) {
  if (!object.is_object()) return fallback;
  auto error = object.find("error");
  if (error == object.end()) return fallback;
  if (error->is_string()) return error->get<std::string>();
  return error->is_object() ? JsonString(*error, "message", std::move(fallback))
                            : fallback;
}

inline bool JsonValue(const json& object, const char* key, bool fallback) {
  return JsonBool(object, key, fallback);
}

template <std::integral Integer>
  requires(!std::same_as<Integer, bool>)
inline Integer JsonValue(const json& object, const char* key,
                         Integer fallback) {
  int64_t value = JsonInt(object, key, static_cast<int64_t>(fallback));
  if constexpr (std::signed_integral<Integer>) {
    if (value < static_cast<int64_t>(std::numeric_limits<Integer>::lowest()) ||
        value > static_cast<int64_t>(std::numeric_limits<Integer>::max())) {
      return fallback;
    }
  } else if (value < 0 ||
             static_cast<uint64_t>(value) >
                 static_cast<uint64_t>(std::numeric_limits<Integer>::max())) {
    return fallback;
  }
  return static_cast<Integer>(value);
}

template <std::floating_point Number>
inline Number JsonValue(const json& object, const char* key, Number fallback) {
  return static_cast<Number>(JsonNumber(object, key, fallback));
}

inline std::string JsonValue(const json& object, const char* key,
                             const char* fallback) {
  return JsonString(object, key, fallback);
}

inline std::string JsonValue(const json& object, const char* key,
                             const std::string& fallback) {
  return JsonString(object, key, fallback);
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

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_JSON_H_
