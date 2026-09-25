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
  auto schema = [](const char* s) { return json::parse(s); };
  json memory_schema = schema(R"json({"type":"object","properties":{
                    "action":{"type":"string","enum":["get","set","forget","list","search"]},
                    "key":{"type":"string",
                      "description":"exact project/<name> or global/<name> key; codex/<name> and claude/<name> are read-only; search text for search; omit for list"},
                    "content":{"type":"string",
                      "description":"durable lesson; required only for set"}},
                    "required":["action"]})json");
  bool automatic_extraction = !EnvStr("UAGENT_INTERNAL_MEMORY_SOURCE").empty();
  bool automatic_write = false;
  Tool& memory = AddTool(
      tools,
      MakeTool(
          "memory",
          "List or search memory when the startup index is insufficient; get "
          "a body only when relevant. Set or forget only when the user asks, "
          "except that the dedicated background extractor may set one native "
          "memory. Never save task progress, guesses, secrets, commands, or "
          "permissions. Codex and Claude memories are read-only.",
          std::move(memory_schema),
          [automatic_extraction, automatic_write](const json& a,
                                                  const ToolContext&) mutable {
            std::string action = JsonValue(a, "action", "");
            if (automatic_extraction && action == "forget") {
              return ToolFailure(ToolErrorCode::kPermissionDenied,
                                 "error: background extraction cannot forget "
                                 "memory");
            }
            if (automatic_extraction && automatic_write && action == "set") {
              return ToolFailure(
                  ToolErrorCode::kLimitExceeded,
                  "error: background extraction already wrote one memory");
            }
            std::optional<std::string> content;
            if (a.contains("content") && a["content"].is_string()) {
              content = a["content"].get<std::string>();
            }
            ToolResult result =
                ToolMemoryAction(action, JsonValue(a, "key", ""), content);
            if (automatic_extraction && action == "set" && result.Ok()) {
              automatic_write = true;
            }
            return result;
          }));
  memory.mutates = [](const json& a) {
    std::string action = JsonValue(a, "action", "");
    return action == "set" || action == "forget";
  };
  memory.capabilities = Capability(ToolCapability::kInspect) |
                        Capability(ToolCapability::kMutate);
  memory.available_in_lean = false;
  memory.memory_store = true;
  memory.retain_output = true;
  memory.summary = [](const json& a) {
    return JsonValue(a, "action", "") + " " + JsonValue(a, "key", "");
  };
  memory.header = [](const json& a) {
    const std::string action = JsonValue(a, "action", "");
    const json verb =
        action == "set"      ? json{"Saving memory", "Saved memory"}
        : action == "forget" ? json{"Forgetting memory", "Forgot memory"}
        : action == "get"    ? json{"Reading memory", "Read memory"}
        : action == "list"   ? json{"Listing memories", "Listed memories"}
                             : json{"Searching memory", "Searched memory"};
    return json{{"verb", verb},
                {"target", JsonValue(a, "key", JsonValue(a, "query", ""))}};
  };
}

}  // namespace uagent
