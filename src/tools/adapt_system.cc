// Copyright 2026 Timon Gentzsch

#include "include/tools/adapt_system.h"

#include <string>
#include <utility>

#include "include/core/debug.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

Tool AdaptSystemTool(AdaptiveSystemState& state) {
  Tool tool = MakeTool(
      "adapt_system",
      "Replace your free-form mutable system directive only when a concrete "
      "task-specific observation warrants materially different behavior "
      "later. Treat revision as an exception, not a planning ritual: do not "
      "call when the proposed guidance implies the same behavior as the "
      "current one. The instructions replace the directive whole and persist "
      "until changed; an empty string clears it. Useful changes include a new "
      "decomposition, expert perspective, evidence standard, phase priority, "
      "or recovery from a failed assumption. Never call to repeat the "
      "request, install a generic inspect/edit/test workflow, or announce "
      "completion. In reason, name the triggering observation and the "
      "strategy delta. It cannot change user authority, permissions, tools, "
      "or host-enforced limits.",
      {{"type", "object"},
       {"properties",
        {{"instructions",
          {{"type", "string"},
           {"maxLength", kAdaptiveSystemBytes},
           {"description", "complete replacement directive; empty clears it"}}},
         {"reason",
          {{"type", "string"},
           {"minLength", 1},
           {"maxLength", kAdaptiveSystemReasonBytes},
           {"description",
            "concrete triggering observation and the material strategy delta "
            "it warrants"}}}}},
       {"required", json::array({"instructions", "reason"})}},
      [&state](const json& arguments, const ToolContext&) {
        std::string instructions =
            Trim(JsonValue(arguments, "instructions", ""));
        std::string reason = Trim(JsonValue(arguments, "reason", ""));
        if (reason.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: reason must not be blank");
        }
        // The schema declares maxLength; enforce it here too, or an oversized
        // directive makes every later session save fail as incomplete.
        if (instructions.size() > kAdaptiveSystemBytes) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: instructions are " +
                                 std::to_string(instructions.size()) +
                                 " bytes; the limit is " +
                                 std::to_string(kAdaptiveSystemBytes));
        }
        if (instructions == state.instructions) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: mutable system directive is unchanged");
        }
        std::string previous = state.instructions;
        state.instructions = std::move(instructions);
        ++state.revision;
        DebugLog("system_adapted", {{"revision", state.revision},
                                    {"reason", std::move(reason)},
                                    {"previous", std::move(previous)},
                                    {"instructions", state.instructions},
                                    {"chars", state.instructions.size()}});
        return ToolSuccess("system revision " + std::to_string(state.revision) +
                           (state.instructions.empty()
                                ? " cleared for the next model request"
                                : " active for the next model request"));
      });
  // This changes only model guidance. Host policy may still hide the tool by
  // name, but capability filters must not mistake it for workspace mutation.
  tool.capabilities = 0;
  tool.summary = [](const json& arguments) {
    std::string operation =
        Trim(JsonValue(arguments, "instructions", "")).empty() ? "clear"
                                                               : "replace";
    return operation + " · " +
           FirstLine(Trim(JsonValue(arguments, "reason", "")));
  };
  return tool;
}

}  // namespace uagent
