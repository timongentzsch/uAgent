// Copyright 2026 Timon Gentzsch

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/memory.h"
#include "src/tools/registry_internal.h"

namespace uagent {

void RegisterMemoryTool(std::vector<Tool>& tools) {
  json memory_schema = json::parse(R"json({"type":"object","properties":{
                    "action":{"type":"string","enum":["get","set","forget","list","search"]},
                    "key":{"type":"string",
                      "description":"exact project/<name> or global/<name> key; codex/<name> and claude/<name> are read-only; search text for search; omit for list"},
                    "content":{"type":"string",
                      "description":"durable lesson"}},
                    "required":["action"]})json");
  Tool& memory = AddTool(
      tools,
      MakeTool(
          "memory",
          "List or search memory when the startup index is insufficient; get "
          "a body only when relevant. Set when the user corrects you or "
          "states a standing preference, or asks; when the index shows a "
          "note on the topic, set that key with the merged rule. Forget only "
          "when the user asks. Never save task progress, guesses, secrets, "
          "commands, or permissions. Codex and Claude memories are "
          "read-only.",
          std::move(memory_schema), [](const json& a, const ToolContext&) {
            std::optional<std::string> content;
            if (a.contains("content") && a["content"].is_string()) {
              content = a["content"].get<std::string>();
            }
            return ToolMemoryAction(JsonValue(a, "action", ""),
                                    JsonValue(a, "key", ""), content);
          }));
  memory.mutates = [](const json& a) {
    std::string action = JsonValue(a, "action", "");
    return action == "set" || action == "forget";
  };
  memory.action_requires = {"action",
                            {{"get", {"key"}},
                             {"set", {"key", "content"}},
                             {"forget", {"key"}},
                             {"search", {"key"}}}};
  memory.capabilities = Capability(ToolCapability::kInspect) |
                        Capability(ToolCapability::kMutate);
  memory.memory_store = true;
  memory.retain_output = true;
  memory.headline = [](const json& a) {
    const std::string action = JsonValue(a, "action", "");
    const std::string target = JsonValue(a, "key", JsonValue(a, "query", ""));
    if (action == "set") {
      return Headline{"Saving memory", "Saved memory", target};
    }
    if (action == "forget") {
      return Headline{"Forgetting memory", "Forgot memory", target};
    }
    if (action == "get") {
      return Headline{"Reading memory", "Read memory", target};
    }
    if (action == "list") {
      return Headline{"Listing memories", "Listed memories"};
    }
    return Headline{"Searching memory", "Searched memory", target};
  };
}

}  // namespace uagent
