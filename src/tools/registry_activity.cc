// Copyright 2026 Timon Gentzsch

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "src/tools/registry_internal.h"

namespace uagent {

void RegisterActivityTool(std::vector<Tool>& tools,
                          ActivitySupervisor& supervisor) {
  json schema = json::parse(
      R"json({"type":"object","additionalProperties":false,"properties":{
                  "operation":{"type":"string","enum":["list","poll","wait","write","resize","stop"]},
                  "id":{"type":"integer","minimum":1,"maximum":2147483647},
                  "ids":{"type":"array","minItems":1,"maxItems":64,
                    "items":{"type":"integer","minimum":1,"maximum":2147483647},
                    "description":"wait targets; omitted means all eligible activities"},
                  "chars":{"type":"string",
                    "description":"bytes for write; empty is intentional"},
                  "wait_ms":{"type":"integer","minimum":0,"maximum":300000},
                  "until":{"type":"string","maxLength":256,
                    "description":"readiness marker for poll"},
                  "mode":{"type":"string","enum":["any","all"],
                    "description":"completion mode for wait"},
                  "rows":{"type":"integer"},
                  "cols":{"type":"integer"},
                  "max_output_chars":{"type":"integer"}},
                  "required":["operation"]})json");
  json& properties = schema["properties"];
  properties["chars"]["maxLength"] = kToolTextArgChars;
  const std::string dimensions = "1.." + std::to_string(kPtyMaxDimension);
  properties["rows"]["description"] = dimensions;
  properties["cols"]["description"] = dimensions;
  properties["max_output_chars"]["minimum"] = kToolMinOutputChars;
  properties["max_output_chars"]["maximum"] = kToolTextArgChars;
  Tool& activity = AddTool(
      tools,
      MakeTool(
          "activity",
          "Inspect or drive activities. poll drains output; wait joins "
          "any/all; "
          "write sends stdin; resize sets PTY dimensions; stop terminates the "
          "process group and removes its log.",
          std::move(schema),
          [&supervisor](const json& a, const ToolContext& context) {
            std::string operation = JsonValue(a, "operation", "");
            int64_t id = JsonValue(a, "id", int64_t{0});
            int64_t wait_ms = JsonValue(a, "wait_ms", int64_t{0});
            int64_t cap = JsonValue(a, "max_output_chars", int64_t{0});
            if (operation == "list") {
              return ToolActivityOutput(supervisor, 0, 0, {}, context, cap);
            }
            if (operation == "poll") {
              return ToolActivityOutput(supervisor, id, wait_ms,
                                        JsonValue(a, "until", ""), context,
                                        cap);
            }
            if (operation == "wait") {
              return ToolActivityWait(
                  supervisor, JsonValue(a, "ids", std::vector<int64_t>{}),
                  JsonValue(a, "mode", "any"), wait_ms, context, cap);
            }
            if (operation == "write") {
              return ToolActivityInput(
                  supervisor, id, JsonValue(a, "chars", ""),
                  a.contains("wait_ms") ? wait_ms : kActivityInputSettleMs,
                  context, 0, 0, cap);
            }
            if (operation == "resize") {
              return ToolActivityInput(
                  supervisor, id, "",
                  a.contains("wait_ms") ? wait_ms : kActivityInputSettleMs,
                  context, JsonValue(a, "rows", int64_t{0}),
                  JsonValue(a, "cols", int64_t{0}), cap);
            }
            return ToolActivityStop(supervisor, id);
          }));
  activity.canonicalize = [](json& a) {
    std::string operation = JsonValue(a, "operation", "");
    if (operation != "list" && operation != "poll" && operation != "wait" &&
        operation != "write" && operation != "resize" && operation != "stop") {
      return;
    }
    auto relevant = [&](std::string_view field) {
      if (field == "id") {
        return operation == "poll" || operation == "write" ||
               operation == "resize" || operation == "stop";
      }
      if (field == "chars") return operation == "write";
      if (field == "wait_ms") return operation != "list" && operation != "stop";
      if (field == "until") return operation == "poll";
      if (field == "mode") return operation == "wait";
      if (field == "ids") return operation == "wait";
      if (field == "rows" || field == "cols") return operation == "resize";
      return false;
    };
    for (std::string_view field :
         {"id", "ids", "chars", "wait_ms", "until", "mode", "rows", "cols"}) {
      if (!relevant(field)) a.erase(std::string(field));
    }
    auto cap = a.find("max_output_chars");
    if (cap != a.end() && cap->is_number_integer() &&
        cap->get<int64_t>() <= 0) {
      a.erase(cap);
    }
  };
  activity.clamped_arguments = {"wait_ms", "max_output_chars"};
  // Waiting is not work: no process runs, nothing is held. The tool timeout
  // exists to stop a command from running away, and applying it here truncated
  // the caller's own wait_ms — the schema offers five minutes and the default
  // budget granted thirty seconds, so a long activity cost a round every half
  // minute. `run` and `scratch`, which do occupy a process, are already exempt
  // for the same reason. The turn deadline still bounds this, and Escape and
  // queued steering still return immediately.
  activity.timeout_s = 0;
  activity.parallel_safe = true;
  activity.capabilities = Capability(ToolCapability::kInspect) |
                          Capability(ToolCapability::kExecute) |
                          Capability(ToolCapability::kMutate);
  activity.mutates = [](const json& a) {
    std::string operation = JsonValue(a, "operation", "");
    return operation == "write" || operation == "resize" || operation == "stop";
  };
  // A wait, a poll or a list observes commands whose own rows show them.
  activity.routine = [](const json& a) {
    const std::string operation = JsonValue(a, "operation", "");
    return operation == "list" || operation == "poll" || operation == "wait";
  };
  activity.result_chars = kActivityResultChars;
  activity.blocking_wait_default_ms = 0;
  // An empty write is intentional, so chars, given or not, is never missing.
  activity.action_requires = {"operation",
                              {{"poll", {"id"}},
                               {"wait", {"wait_ms"}},
                               {"write", {"id"}},
                               {"resize", {"id", "rows", "cols"}},
                               {"stop", {"id"}}}};
  activity.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    if (JsonValue(a, "operation", "") != "resize") return std::nullopt;
    int64_t rows = JsonValue(a, "rows", int64_t{0});
    int64_t cols = JsonValue(a, "cols", int64_t{0});
    if (rows < 1 || rows > kPtyMaxDimension || cols < 1 ||
        cols > kPtyMaxDimension) {
      return ArgumentIssue("activity.invalid_dimensions",
                           "resize requires rows and cols in 1.." +
                               std::to_string(kPtyMaxDimension));
    }
    return std::nullopt;
  };
  activity.intent = "run";
  activity.headline = [](const json& a) {
    const std::string operation = JsonValue(a, "operation", "");
    const std::string target =
        "activity " + std::to_string(JsonValue(a, "id", int64_t{0}));
    if (operation == "list") return Headline{"Listing", "Listed", "activities"};
    if (operation == "wait") {
      return Headline{"Waiting for", "Waited for", "activities"};
    }
    if (operation == "stop") return Headline{"Stopping", "Stopped", target};
    if (operation == "write" || operation == "resize") {
      return Headline{"Sending input to", "Sent input to", target};
    }
    return Headline{"Checking", "Checked", target};
  };
}

}  // namespace uagent
