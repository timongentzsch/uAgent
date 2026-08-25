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
      "Describe this running \u00b5Agent build: its version, command-line "
      "flags, "
      "slash commands, configuration schema with effective values and their "
      "source, or the tools available right now. Answers come from the "
      "installed binary, so prefer it over documentation or memory when the "
      "question is about actual behaviour, a setting's default, why a value is "
      "active, or whether a change needs a restart. Secret values are reported "
      "only as set or unset. Returns one topic per call.",
      {{"type", "object"},
       {"properties",
        {{"topic",
          {{"type", "string"},
           {"enum",
            json::array({"status", "cli", "commands", "config", "tools"})},
           {"description",
            "status: version, route, effort, approval mode and budgets; cli: "
            "flags; commands: slash commands; config: settings with defaults, "
            "active values, source and reload policy; tools: the live tool "
            "surface"}}},
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
  // Inspect-only: safe in a lean subagent and never a workspace mutation.
  tool.capabilities = 0;
  tool.parallel_safe = true;
  tool.summary = [](const json& arguments) {
    std::string topic = Trim(JsonValue(arguments, "topic", ""));
    std::string name = Trim(JsonValue(arguments, "name", ""));
    return name.empty() ? topic : topic + " \u00b7 " + name;
  };
  return tool;
}

}  // namespace uagent
