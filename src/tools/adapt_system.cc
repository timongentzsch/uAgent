// Copyright 2026 Timon Gentzsch

#include "include/tools/adapt_system.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "include/agent/prompt.h"
#include "include/core/debug.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

Tool AdaptSystemTool(AdaptiveSystemState& state, PromptController control) {
  if (!control) {
    control = [&state](const json& request) {
      return PromptControl(request, &state, SystemPromptBase(), json::array());
    };
  }
  auto mandatory = [&state](const json& args) {
    return JsonValue(args, "action", "set") != "show" &&
           (JsonValue(args, "scope", "conversation") != "conversation" ||
            JsonValue(args, "mode", "overlay") == "replace" ||
            state.mode == "replace" ||
            (JsonValue(args, "action", "set") == "edit" &&
             state.instructions.empty()));
  };
  auto normalize = [](const json& args) {
    auto request = args;
    request["action"] = JsonValue(args, "action", "set");
    request["scope"] = JsonValue(args, "scope", "conversation");
    return request;
  };
  struct Prepared {
    json request;
    std::string digest;
  };
  auto proposals = std::make_shared<ApprovedProposals<Prepared>>();
  Tool tool = MakeTool(
      "adapt_system",
      "Show, overlay, replace or edit the system prompt at conversation, "
      "project or global scope. "
      "Show first to obtain the revision; set writes complete text, edit "
      "replaces one exact old/new match, reset inherits. "
      "Global/project writes and complete replacements require human approval, "
      "including in YOLO. "
      "Self-adaptation is an exception, not a planning ritual: name the "
      "triggering observation and material strategy delta. "
      "Prompt text cannot change host permissions, tools or limits.",
      json::parse(R"({"type":"object","properties":{
        "action":{"type":"string","enum":["show","set","edit","reset"]},
        "scope":{"type":"string","enum":["conversation","project","global"]},
        "mode":{"type":"string","enum":["overlay","replace"]},
        "revision":{"type":"string","description":"revision from show, required for writes"},
        "text":{"type":"string","maxLength":65536},
        "old":{"type":"string","description":"unique exact text to replace"},
        "new":{"type":"string"},
        "reason":{"type":"string","minLength":1,"maxLength":512,"description":"triggering observation and material strategy delta; required for writes"}
      },"required":["action"]})"),
      [control, normalize, mandatory, proposals](const json& args,
                                                 const ToolContext&) {
        auto request = normalize(args);
        if (mandatory(args)) {
          std::optional<Prepared> approved = proposals->Take(args);
          if (!approved) {
            return ToolFailure(ToolErrorCode::kPermissionDenied,
                               "No approved prompt change for this request.");
          }
          request = approved->request;
          auto preview_request = request;
          preview_request["action"] = "preview";
          const auto preview = control(preview_request);
          if (preview.contains("error") ||
              HashHex(JsonDump(preview["sources"])) != approved->digest) {
            return ToolFailure(
                ToolErrorCode::kInvalidArguments,
                "Prompt changed since approval; show and propose again.");
          }
        }
        if (JsonValue(request, "action", "show") != "show" &&
            Trim(JsonValue(args, "reason", "")).empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "reason must not be blank");
        }
        if (JsonValue(request, "text", "").size() > kAdaptiveSystemBytes) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "system prompt exceeds 64 KiB");
        }
        auto result = control(request);
        if (request["action"] == "show" && !result.contains("error")) {
          result.erase("inherited");
          result.erase("last_sent");
          auto output = JsonDump(result);
          const auto limit = static_cast<int64_t>(output.size());
          return ToolSuccess(std::move(output), limit);
        }
        return result.contains("error")
                   ? ToolFailure(ToolErrorCode::kInvalidArguments,
                                 JsonValue(result, "error", ""))
                   : ToolSuccess(JsonValue(result, "diff", "") + "\n" +
                                 JsonValue(result, "applies", ""));
      });
  tool.mutates = mandatory;
  tool.approval_class = [mandatory](const json& args) {
    return mandatory(args) ? ApprovalClass::kMandatoryHuman
                           : ApprovalClass::kNone;
  };
  tool.approval_preview = [control, normalize, proposals](const json& args) {
    auto request = normalize(args);
    // Resolve exact edits before approval; commit only these replacement bytes.
    auto preview_request = request;
    preview_request["action"] = "preview";
    if (request["action"] == "reset") preview_request["mode"] = "inherit";
    if (request["action"] == "edit") preview_request["operation"] = "edit";
    auto preview = control(preview_request);
    if (preview.contains("error")) return JsonValue(preview, "error", "");
    request["action"] = preview["item"]["mode"] == "inherit" ? "reset" : "set";
    request["mode"] = preview["item"]["mode"];
    request["text"] = preview["item"]["text"];
    proposals->Put(args, {request, HashHex(JsonDump(preview["sources"]))},
                   std::chrono::steady_clock::now() + std::chrono::minutes(5));
    return JsonValue(args, "scope", "conversation") +
           " system prompt · next model request\n" +
           JsonValue(preview, "diff", "");
  };
  tool.validate =
      [normalize](const json& args) -> std::optional<ToolArgumentIssue> {
    auto request = normalize(args);
    if (request["action"] != "show" &&
        Trim(JsonValue(args, "reason", "")).empty()) {
      return ArgumentIssue("prompt.reason", "reason must not be blank",
                           "reason");
    }
    return std::nullopt;
  };
  tool.capabilities = Capability(ToolCapability::kMutate);
  tool.parallel_safe = false;
  tool.summary = [](const json& args) {
    return JsonValue(args, "action", "set") + " · " +
           JsonValue(args, "scope", "conversation") + " system prompt";
  };
  return tool;
}

}  // namespace uagent
