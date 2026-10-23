// Copyright 2026 Timon Gentzsch
#include "include/app/control.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "include/agent/prompt.h"
#include "include/app/library.h"
#include "include/app/permissions.h"
#include "include/app/schedule.h"
#include "include/app/tool_categories.h"
#include "include/core/capture.h"
#include "include/core/config.h"
#include "include/core/effective_config.h"
#include "include/core/fs.h"
#include "include/core/project.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/providers/providers.h"

namespace uagent {
json ManagementControl(const json& request) {
  const std::string kind = JsonValue(request, "kind", "");
  if (kind == "models") {
    Api api(RuntimeConfig::FromEnvironment());
    auto provider = ConfigureProvider(api);
    return ModelCatalogue(api, provider.routes, provider.providers);
  }
  if (kind == "schedule") return ScheduleControl(request);
  if (kind == "permission_rules") return PermissionRulesControl(request);
  if (kind == "tool_categories") return ToolCategoriesControl(request);
  if (kind == "instructions") {
    // A person edits these directly; an agent asks through uagent.
    const std::string cwd = CanonicalCwd();
    if (JsonValue(request, "action", "show") == "set") {
      bool panel = false, project = false;
      if (!ParseInstructionTarget(JsonValue(request, "audience", ""),
                                  JsonValue(request, "scope", ""), panel,
                                  project)) {
        return {{"error",
                 "audience is sessions or panel, scope user "
                 "or project"}};
      }
      std::optional<std::string> base;
      if (const std::string* text = JsonStringRef(request, "base")) {
        base = *text;
      }
      const std::string error = WriteInstructionFile(
          panel, project, cwd, JsonValue(request, "text", ""), base);
      if (!error.empty()) return {{"error", error}};
    }
    json shown = InstructionFiles(cwd);
    shown["base"] = {{"sessions", SystemPromptBase()},
                     {"panel", PanelPromptBase()}};
    return shown;
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
  if (argument == "-" && ReadBounded(std::cin, MiB(1), bytes)) {
    printf("{\"error\":\"control request exceeds limit\"}\n");
    return 2;
  }
  json request = json::parse(bytes, nullptr, false), result;
  if (!request.is_object()) {
    result = {{"error", "control expects a JSON object"}};
  } else if (JsonValue(request, "kind", "") == "calendar") {
    result = ScheduleCalendar(request);
  } else {
    const auto cwd =
        CanonicalDirectory(JsonValue(request, "cwd", CanonicalCwd()));
    std::error_code error;
    if (cwd) std::filesystem::current_path(*cwd, error);
    if (!cwd || error) {
      result = {{"error", "project directory is unavailable"}};
    } else {
      auto manager = ConfigManager::Capture({});
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
      if (!ReadRegularFile(value.substr(1), KiB(512), body, error)) {
        return {{"error", error}};
      }
      request["content"] = body;
    } else if (action == "set") {
      request["content"] = value;
    }
    if (action == "import" && kind != "schedule") {
      // Memories and skills share one importer; the command names which.
      return ControlProcess({{"kind", "import"},
                             {"action", key.empty() ? "list" : "run"},
                             {"only", kind == "skills" ? "skill" : "memory"},
                             {"key", key}});
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
