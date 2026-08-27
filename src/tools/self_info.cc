// Copyright 2026 Timon Gentzsch

#include "include/tools/self_info.h"

#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

Tool SelfInfoTool(SelfDescriptionProvider describe) {
  Tool tool = MakeTool(
      "uagent_info",
      "Describe this running \u00b5Agent build. Answers come from the "
      "installed "
      "binary, so prefer it over documentation or memory for actual "
      "behaviour, a setting's default, why a value is active, or whether a "
      "change needs a restart. Secrets read as set or unset. One topic per "
      "call.",
      {{"type", "object"},
       {"properties",
        {{"topic",
          {{"type", "string"},
           {"enum", json::array({"status", "cli", "commands", "config", "tools",
                                 "prompt", "routes"})},
           {"description",
            "status: version, route, effort, approval mode and budgets; cli: "
            "flags; commands: slash commands; config: settings with defaults, "
            "active values, source and reload policy; tools: the live tool "
            "surface; prompt: the system prompt actually in effect; routes: "
            "the model routes and providers this build can reach, and the "
            "selection grammar for naming one"}}},
         {"name",
          {{"type", "string"},
           {"description",
            "optional exact setting or tool name to narrow the answer"}}}}},
       {"required", json::array({"topic"})}},
      [describe = std::move(describe)](const json& arguments,
                                       const ToolContext&) {
        SelfTopic topic = SelfTopic::kStatus;
        std::string requested = Trim(JsonValue(arguments, "topic", ""));
        if (!ParseSelfTopic(requested, topic)) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: unknown topic: " + requested);
        }
        std::string name = Trim(JsonValue(arguments, "name", ""));
        json described = describe(topic, name);
        return ToolSuccess(JsonDump(described, 2));
      });
  // Inspect-only and never a workspace mutation. Withheld from lean children
  // all the same: a delegated task is briefed, not left to introspect the
  // harness, and the schema is a kilobyte on every one of its requests.
  tool.capabilities = 0;
  tool.available_in_lean = false;
  tool.parallel_safe = true;
  tool.summary = [](const json& arguments) {
    std::string topic = Trim(JsonValue(arguments, "topic", ""));
    std::string name = Trim(JsonValue(arguments, "name", ""));
    return name.empty() ? topic : topic + " \u00b7 " + name;
  };
  return tool;
}

}  // namespace uagent
