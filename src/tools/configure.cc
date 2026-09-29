// Copyright 2026 Timon Gentzsch

#include "include/tools/configure.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/prompt.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {
Tool UagentTool(SelfDescriptionProvider describe,
                const ConfigProposalFactory& prepare,
                const std::shared_ptr<ConfigApprovals>& store) {
  Tool tool = MakeTool(
      "uagent",
      "Inspect this running build: status, cli, commands, config, tools, "
      "prompt, routes, or soul (the folder coordinator's standing character: "
      "user and project). Configure registered UAGENT_* settings with an "
      "exact human-approved diff; YOLO cannot approve configuration changes. "
      "Inspect config first. set_soul replaces one soul (scope, text) once "
      "the user approves the exact text. Secrets are never returned or "
      "accepted literally.",
      {{"type", "object"},
       {"properties",
        {{"action",
          {{"type", "string"},
           {"enum", json::array({"inspect", "configure", "set_soul"})}}},
         {"topic",
          {{"type", "string"},
           {"enum", json::array({"status", "cli", "commands", "config", "tools",
                                 "prompt", "routes", "soul"})}}},
         {"text",
          {{"type", "string"},
           {"description", "set_soul: the whole new soul, Markdown"}}},
         {"name",
          {{"type", "string"}, {"description", "exact setting or tool name"}}},
         {"scope",
          {{"type", "string"},
           {"enum", json::array({"user", "project"})},
           {"description",
            "user writes ~/.uagent/.config; project writes ./.uagent/.config "
            "and requires an already-trusted workspace"}}},
         {"changes",
          {{"type", "array"},
           {"description", "one entry per setting"},
           {"items",
            {{"type", "object"},
             {"properties",
              {{"key",
                {{"type", "string"}, {"description", "exact UAGENT_* name"}}},
               {"operation",
                {{"type", "string"}, {"enum", json::array({"set", "unset"})}}},
               {"value",
                {{"type", "string"},
                 {"description", "required for set; omitted for unset"}}}}},
             {"required", json::array({"key", "operation"})}}}}}}},
       {"required", json::array({"action"})}},
      [describe = std::move(describe), store](
          const json& arguments, const ToolContext&) -> ToolResult {
        const std::string action = JsonValue(arguments, "action", "");
        if (action == "inspect" && JsonValue(arguments, "topic", "") == "soul") {
          return ToolSuccess(JsonDump(SoulDocuments(CanonicalCwd()), 2));
        }
        if (action == "set_soul") {
          // Reaching here means the user approved this exact text.
          const std::string error =
              WriteSoul(JsonValue(arguments, "scope", "user"), CanonicalCwd(),
                        JsonValue(arguments, "text", ""));
          return error.empty()
                     ? ToolSuccess("soul saved; the coordinator reads it from "
                                   "its next turn")
                     : ToolFailure(ToolErrorCode::kProcessFailed,
                                   "error: " + error);
        }
        if (action == "inspect") {
          SelfTopic topic = SelfTopic::kStatus;
          if (!ParseSelfTopic(JsonValue(arguments, "topic", "status"), topic)) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "unknown topic");
          }
          json value = describe(topic, Trim(JsonValue(arguments, "name", "")));
          if (topic == SelfTopic::kPrompt) {
            value.erase("inherited");
            value.erase("last_sent");
          }
          std::string output = JsonDump(value, 2);
          int64_t limit = topic == SelfTopic::kPrompt
                              ? static_cast<int64_t>(output.size())
                              : -1;
          return ToolSuccess(std::move(output), limit);
        }
        // Reaching this point means the human approved the preview built from
        // these exact arguments; the stored proposal carries the bytes they
        // saw.
        if (!store) {
          return ToolFailure(ToolErrorCode::kPermissionDenied,
                             "error: configuration unavailable");
        }
        std::optional<ConfigProposal> taken = store->Take(arguments);
        if (!taken || !taken->ok) {
          return ToolFailure(
              ToolErrorCode::kPermissionDenied,
              "error: no approved configuration change for this request");
        }
        const ConfigProposal& approved = *taken;
        std::string error;
        std::string notice;
        if (!CommitConfigProposal(approved, error, &notice)) {
          return ToolFailure(ToolErrorCode::kProcessFailed, "error: " + error);
        }
        std::string report = "wrote " + approved.target;
        for (const ConfigChangeEffect& effect : approved.effects) {
          report += "\n" + effect.key + ": " + ConfigEffectName(effect.effect);
        }
        if (!notice.empty()) report += "\nnote: " + notice;
        return ToolSuccess(report);
      });
  tool.mutates = [](const json& arguments) {
    return JsonValue(arguments, "action", "") != "inspect";
  };
  tool.redact_invalid_arguments = true;
  tool.approval_class = [](const json& arguments) {
    return JsonValue(arguments, "action", "") != "inspect"
               ? ApprovalClass::kMandatoryHuman
               : ApprovalClass::kNone;
  };
  tool.validate =
      [prepare](const json& arguments) -> std::optional<ToolArgumentIssue> {
    if (JsonValue(arguments, "action", "") == "inspect") {
      if (arguments.contains("scope") || arguments.contains("changes") ||
          arguments.contains("text")) {
        return ArgumentIssue("config.inspect",
                             "inspect does not accept scope or changes");
      }
      return std::nullopt;
    }
    if (!prepare) {
      return ArgumentIssue("config.unavailable",
                           "configuration requires an interactive human");
    }
    if (JsonValue(arguments, "action", "") == "set_soul") {
      const std::string scope = JsonValue(arguments, "scope", "user");
      if (scope != "user" && scope != "project") {
        return ArgumentIssue("soul.scope", "scope must be user or project",
                             "scope");
      }
      if (!arguments.contains("text")) {
        return ArgumentIssue("soul.text", "set_soul needs the whole text",
                             "text");
      }
      return std::nullopt;
    }
    if (arguments.contains("topic") || arguments.contains("name")) {
      return ArgumentIssue("config.configure",
                           "configure does not accept topic or name");
    }
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    if (!ParseConfigScope(Trim(JsonValue(arguments, "scope", "")), scope)) {
      return ArgumentIssue("config.scope", "scope must be user or project",
                           "scope");
    }
    std::vector<ConfigChange> changes;
    std::string error;
    if (!ParseConfigChanges(arguments, changes, error)) {
      return ArgumentIssue("config.changes", error, "changes");
    }
    ConfigProposal proposal = prepare(scope, changes);
    if (!proposal.ok) {
      return ArgumentIssue("config.rejected", proposal.error, "changes");
    }
    return std::nullopt;
  };
  // A genuine one-liner: this is the call label, and it also reaches the debug
  // trace and compaction evidence, so the diff must not be built here.
  tool.summary = [](const json& arguments) {
    if (JsonValue(arguments, "action", "") == "set_soul") {
      return "soul · " + JsonValue(arguments, "scope", "user");
    }
    if (JsonValue(arguments, "action", "") == "inspect") {
      return JsonValue(arguments, "topic", "status") + " " +
             JsonValue(arguments, "name", "");
    }
    std::string scope = Trim(JsonValue(arguments, "scope", ""));
    const json* list = JsonArray(arguments, "changes");
    std::string detail;
    if (list) {
      for (const json& entry : *list) {
        if (!detail.empty()) detail += ", ";
        detail += Trim(JsonValue(entry, "key", ""));
        if (Trim(JsonValue(entry, "operation", "set")) == "unset") {
          detail += "=<unset>";
        }
      }
    }
    return scope + " \u00b7 " + detail;
  };
  // Built only when a person is about to be asked. Preparing here also records
  // the proposal, so the bytes shown are exactly the bytes that can commit.
  tool.approval_preview = [prepare, store](const json& arguments) {
    if (JsonValue(arguments, "action", "") == "set_soul") {
      const std::string scope = JsonValue(arguments, "scope", "user");
      const json current = SoulDocuments(CanonicalCwd())[scope];
      return "Replace " + JsonValue(current, "path", "") + "\n\nNow:\n" +
             JsonValue(current, "text", "(empty)") + "\n\nProposed:\n" +
             JsonValue(arguments, "text", "");
    }
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    std::vector<ConfigChange> changes;
    std::string error;
    if (!ParseConfigScope(Trim(JsonValue(arguments, "scope", "")), scope) ||
        !ParseConfigChanges(arguments, changes, error)) {
      return std::string("invalid configuration request");
    }
    ConfigProposal proposal = prepare(scope, changes);
    if (!proposal.ok) return "cannot apply: " + proposal.error;
    std::string preview = proposal.Preview();
    const auto expires = proposal.expires;
    store->Put(arguments, std::move(proposal), expires);
    return preview;
  };
  // Inspection survives restricted policies; configure still requires a human.
  tool.capabilities = 0;
  if (!prepare) {
    tool.parallel_safe = true;
    tool.parameters["properties"]["action"]["enum"] = json::array({"inspect"});
    tool.parameters["properties"].erase("scope");
    tool.parameters["properties"].erase("changes");
    tool.parameters["properties"].erase("text");
  }
  tool.available_in_lean = false;
  tool.intent = "setup";
  tool.header = [](const json& arguments) {
    return JsonValue(arguments, "action", "") == "inspect"
               ? json{{"verb", {"Inspecting", "Inspected"}}}
               : json{{"verb", {"Configuring", "Configured"}}};
  };
  return tool;
}

}  // namespace uagent
