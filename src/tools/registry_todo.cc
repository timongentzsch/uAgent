// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/json.h"
#include "src/tools/registry_internal.h"

namespace uagent {

void RegisterTodoTool(std::vector<Tool>& tools) {
  Tool& todo = AddTool(
      tools,
      MakeTool(
          "todo",
          "Your to-do list for this conversation; you and the user see it "
          "until it is done. Each call replaces the whole list. Keep one for "
          "work of three or more steps: mark an item in_progress as you start "
          "it and done as soon as it is, and drop what no longer applies.",
          json::parse(R"json({"type":"object","properties":{
            "items":{"type":"array","maxItems":24,"items":{"type":"object",
              "properties":{
                "text":{"type":"string","minLength":1,"maxLength":160},
                "status":{"type":"string","enum":["pending","in_progress","done"]}},
              "required":["text","status"]}}},
            "required":["items"]})json"),
          [](const json& a, const ToolContext&) {
            ToolResult result =
                ToolSuccess(a["items"].empty() ? "The to-do list is empty."
                                               : TodoText(a["items"]));
            result.body_kind = "markdown";
            result.todo = a["items"];
            return result;
          }));
  // The list changes nothing outside the conversation, so it needs no
  // capability.
  todo.capabilities = 0;
  todo.category = "collaborate";
  todo.intent = "todo";
  todo.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    const json* items = JsonArray(a, "items");
    if (items && std::ranges::count_if(*items, [](const json& item) {
                   return JsonValue(item, "status", "") == "in_progress";
                 }) > 1) {
      return ArgumentIssue("todo.in_progress",
                           "at most one item is in_progress; mark the others "
                           "pending or done",
                           "items");
    }
    return std::nullopt;
  };
  todo.headline = [](const json& a) {
    const json* items = JsonArray(a, "items");
    if (!items || items->empty()) {
      return Headline{"Clearing", "Cleared", "to-dos"};
    }
    const auto done = std::ranges::count_if(*items, [](const json& item) {
      return JsonValue(item, "status", "") == "done";
    });
    return Headline{
        "Updating to-dos", "Updated to-dos",
        std::to_string(done) + "/" + std::to_string(items->size()) + " done"};
  };
  // The result shows the list.
  todo.present = [](const json&) { return json::array(); };
}

}  // namespace uagent
