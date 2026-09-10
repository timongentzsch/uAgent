// Copyright 2026 Timon Gentzsch
#include "include/app/control.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/core/capture.h"
#include "include/core/config.h"
#include "include/core/effective_config.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/providers.h"

namespace uagent {
json ManagementControl(const json& request) {
  if (JsonValue(request, "kind", "") == "models") {
    Api api(RuntimeConfig::FromEnvironment());
    auto provider = ConfigureProvider(api);
    return ModelCatalogue(api, provider.routes, provider.providers);
  }
  if (JsonValue(request, "kind", "") == "schedule") {
    return ScheduleControl(request);
  }
  return LibraryControl(request, JsonValue(request, "cwd", CanonicalCwd()));
}
json ControlProcess(const json& request) {
  auto child = CaptureProcess({ExecutablePath(), "--control", "-"}, 15,
                              JsonDump(request));
  json result = json::parse(child.output, nullptr, false);
  if (!result.is_object()) {
    return {
        {"error", child.error.empty() ? "native control failed" : child.error}};
  }
  return result;
}
int ControlMain(const std::string& argument) {
  std::string bytes = argument;
  if (argument == "-" && ReadBounded(std::cin, size_t{1024} * 1024, bytes)) {
    printf("{\"error\":\"control request exceeds limit\"}\n");
    return 2;
  }
  json request = json::parse(bytes, nullptr, false), result;
  if (!request.is_object()) {
    result = {{"error", "control expects a JSON object"}};
  } else if (JsonValue(request, "kind", "") == "calendar") {
    result = ScheduleCalendar(request);
  } else {
    std::error_code error;
    auto cwd = std::filesystem::canonical(
        JsonValue(request, "cwd", CanonicalCwd()), error);
    if (!error && std::filesystem::is_directory(cwd, error)) {
      std::filesystem::current_path(cwd, error);
    } else if (!error) {
      error = std::make_error_code(std::errc::not_a_directory);
    }
    if (error) {
      result = {{"error", "project directory is unavailable"}};
    } else {
      auto manager = ConfigManager::Capture(ProjectConfigTrusted(), {});
      manager.Initialize();
      result = ManagementControl(request);
    }
  }
  printf("%s\n", JsonDump(result).c_str());
  return result.contains("error") ? 1 : 0;
}
json ManagementCommand(const std::string& kind, const std::string& argument) {
  json request;
  if (Trim(argument).starts_with("{")) {
    request = json::parse(argument, nullptr, false);
  } else {
    std::istringstream input(argument);
    std::string action = "list", key, value;
    input >> action >> key;
    std::getline(input >> std::ws, value);
    request = {{"action", action}, {"key", key}};
    if (action == "set" && value.starts_with("@")) {
      std::string body, error;
      if (!ReadRegularFile(value.substr(1), size_t{512} * 1024, body, error)) {
        return {{"error", error}};
      }
      request["content"] = body;
    } else if (action == "set") {
      request["content"] = value;
    }
    if (action == "rename" || action == "copy") request["target"] = value;
    if (action == "set" || action == "forget" || action == "rename" ||
        action == "copy" || action == "pause" || action == "resume") {
      const auto current =
          ControlProcess({{"kind", kind}, {"action", "get"}, {"key", key}});
      request["revision"] =
          JsonValue(JsonValue(current, "item", json::object()), "revision", "");
    }
  }
  if (!request.is_object()) return {{"error", "invalid management command"}};
  request["kind"] = kind;
  return ControlProcess(request);
}
}  // namespace uagent
