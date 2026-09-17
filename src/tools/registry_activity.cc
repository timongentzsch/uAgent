// Copyright 2026 Timon Gentzsch

#include "src/tools/registry_internal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/jobs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

void RegisterActivityTool(std::vector<Tool>& tools,
                          ProcessSupervisor& supervisor) {
  auto schema = [](const char* s) { return json::parse(s); };
  Tool& activity = AddTool(
      tools,
      MakeTool(
          "activity",
          "Inspect or drive activities. poll drains output; wait joins "
          "any/all; "
          "write sends stdin; resize sets PTY dimensions; stop terminates the "
          "process group and removes its log.",
          schema(
              R"json({"type":"object","additionalProperties":false,"properties":{
                  "operation":{"type":"string","enum":["list","poll","wait","write","resize","stop"]},
                  "id":{"type":"integer","minimum":1,"maximum":2147483647,
                    "description":"required for poll, write, resize, stop"},
                  "ids":{"type":"array","minItems":1,"maxItems":64,
                    "items":{"type":"integer","minimum":1,"maximum":2147483647},
                    "description":"wait targets; omitted means all eligible activities"},
                  "chars":{"type":"string","maxLength":65536,
                    "description":"bytes for write; empty is intentional"},
                  "wait_ms":{"type":"integer","minimum":0,"maximum":300000,
                    "description":"required for wait"},
                  "until":{"type":"string","maxLength":256,
                    "description":"readiness marker for poll"},
                  "mode":{"type":"string","enum":["any","all"],
                    "description":"completion mode for wait"},
                  "rows":{"type":"integer","description":"required for resize; 1..1000"},
                  "cols":{"type":"integer","description":"required for resize; 1..1000"},
                  "max_output_chars":{"type":"integer","minimum":256,"maximum":65536}},
                  "required":["operation"]})json"),
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
            if (operation == "stop") return ToolActivityStop(supervisor, id);
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: unknown activity operation");
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
  // budget granted thirty seconds, so a long job cost a round every half
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
  activity.result_chars = kActivityResultChars;
  activity.blocking_wait_default_ms = 0;
  activity.visibility = Tool::Visibility::kDetachedTerminal;
  activity.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    std::string operation = JsonValue(a, "operation", "");
    if ((operation == "poll" || operation == "write" || operation == "resize" ||
         operation == "stop") &&
        !a.contains("id")) {
      return ArgumentIssue("activity.missing_id", operation + " requires id",
                           "id");
    }
    if (operation == "write" && !a.contains("chars")) {
      return ArgumentIssue("activity.missing_chars", "write requires chars",
                           "chars");
    }
    if (operation == "wait" && !a.contains("wait_ms")) {
      return ArgumentIssue("activity.missing_wait", "wait requires wait_ms",
                           "wait_ms");
    }
    if (operation == "resize") {
      int64_t rows = JsonValue(a, "rows", int64_t{0});
      int64_t cols = JsonValue(a, "cols", int64_t{0});
      if (rows < 1 || rows > 1000 || cols < 1 || cols > 1000) {
        return ArgumentIssue("activity.invalid_dimensions",
                             "resize requires rows and cols in 1..1000");
      }
    }
    return std::nullopt;
  };
  activity.summary = [](const json& a) {
    std::string operation = JsonValue(a, "operation", "");
    int64_t id = JsonValue(a, "id", int64_t{0});
    int64_t wait_ms = JsonValue(a, "wait_ms", int64_t{0});
    std::string wait =
        wait_ms > 0
            ? " · wait≤" + FmtDuration(static_cast<double>(wait_ms) / 1000.0)
            : std::string();
    if (operation == "list") return std::string("list activities");
    if (operation == "wait") {
      return "wait for " + JsonValue(a, "mode", "any") + " · " +
             (a.contains("ids") ? JsonDump(a["ids"]) : "all current") + wait;
    }
    std::string target = "activity " + std::to_string(id);
    if (operation == "write") {
      return "write " +
             FmtBytes(static_cast<int64_t>(JsonValue(a, "chars", "").size())) +
             " → " + target + wait;
    }
    if (operation == "resize") {
      return "resize " + std::to_string(JsonValue(a, "rows", int64_t{0})) +
             "×" + std::to_string(JsonValue(a, "cols", int64_t{0})) + " → " +
             target + wait;
    }
    if (operation == "stop") return "stop " + target;
    if (operation == "poll" && a.contains("until")) {
      return "await " + TerminalSafe(JsonValue(a, "until", "")) + " · " +
             target + wait;
    }
    if (operation == "poll") return "poll " + target + wait;
    return std::string("activity");
  };
}

}  // namespace uagent
