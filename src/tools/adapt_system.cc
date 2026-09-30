// Copyright 2026 Timon Gentzsch

#include "include/tools/adapt_system.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

Tool AdaptSystemTool(const AdaptiveSystemState& state,
                     const SelfDirectiveControl& control) {
  // Replacing the base prompt, or revising a replacement, is the user's call.
  auto mandatory = [&state](const json& args) {
    return JsonValue(args, "action", "set") != "show" &&
           (JsonValue(args, "mode", "overlay") == "replace" ||
            state.mode == "replace");
  };
  // What the user approved is the previewed text, committed byte for byte.
  auto proposals = std::make_shared<ApprovedProposals<json>>();
  Tool tool = MakeTool(
      "adapt_system",
      "Show, set, edit or reset your self-directive: text added to (or, with "
      "the user's approval, replacing) your system prompt for this "
      "conversation only. Show first to obtain the revision; set writes "
      "complete text, edit replaces one exact old/new match, reset clears it. "
      "Self-adaptation is an exception, not a planning ritual: name the "
      "triggering observation and material strategy delta. It cannot change "
      "host permissions, tools or limits.",
      json::parse(R"({"type":"object","properties":{
        "action":{"type":"string","enum":["show","set","edit","reset"]},
        "mode":{"type":"string","enum":["overlay","replace"]},
        "revision":{"type":"string","description":"revision from show, required for writes"},
        "text":{"type":"string","maxLength":65536},
        "old":{"type":"string","description":"unique exact text to replace"},
        "new":{"type":"string"},
        "reason":{"type":"string","minLength":1,"maxLength":512,"description":"triggering observation and material strategy delta; required for writes"}
      },"required":["action"]})"),
      [control, mandatory, proposals](const json& args, const ToolContext&) {
        json request = args;
        request["action"] = JsonValue(args, "action", "set");
        if (request["action"] != "show" &&
            Trim(JsonValue(args, "reason", "")).empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "reason must not be blank");
        }
        if (mandatory(args)) {
          std::optional<json> approved = proposals->Take(args);
          if (!approved) {
            return ToolFailure(ToolErrorCode::kPermissionDenied,
                               "No approved self-directive for this request.");
          }
          request = std::move(*approved);
        }
        json result = control(request);
        if (result.contains("error")) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             JsonValue(result, "error", ""));
        }
        if (request["action"] == "show") {
          result.erase("last_sent");
          std::string output = JsonDump(result);
          const auto limit = static_cast<int64_t>(output.size());
          return ToolSuccess(std::move(output), limit);
        }
        return ToolSuccess(JsonValue(result, "diff", "") + "\n" +
                           JsonValue(result, "applies", ""));
      });
  tool.mutates = mandatory;
  tool.approval_class = [mandatory](const json& args) {
    return mandatory(args) ? ApprovalClass::kMandatoryHuman
                           : ApprovalClass::kNone;
  };
  tool.approval_preview = [control, proposals](const json& args) {
    json preview = args;
    preview["action"] = JsonValue(args, "action", "set");
    preview["dry_run"] = true;
    const json result = control(preview);
    if (result.contains("error")) return JsonValue(result, "error", "");
    // Approved is exactly what was shown: the edit or reset, resolved.
    json approved = args;
    approved["action"] = "set";
    approved["mode"] = result["item"]["mode"];
    approved["text"] = result["item"]["text"];
    proposals->Put(args, std::move(approved),
                   std::chrono::steady_clock::now() + std::chrono::minutes(5));
    return "self-directive · next model request\n" +
           JsonValue(result, "diff", "");
  };
  tool.validate = [](const json& args) -> std::optional<ToolArgumentIssue> {
    if (JsonValue(args, "action", "set") != "show" &&
        Trim(JsonValue(args, "reason", "")).empty()) {
      return ArgumentIssue("prompt.reason", "reason must not be blank",
                           "reason");
    }
    return std::nullopt;
  };
  tool.capabilities = Capability(ToolCapability::kMutate);
  tool.parallel_safe = false;
  tool.summary = [](const json& args) {
    return JsonValue(args, "action", "set") + " · self-directive";
  };
  tool.intent = "setup";
  tool.header = [](const json&) {
    return json{{"verb", {"Adapting", "Adapted"}},
                {"target", "self-directive"}};
  };
  return tool;
}

}  // namespace uagent
