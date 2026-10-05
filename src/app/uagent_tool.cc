// Copyright 2026 Timon Gentzsch

#include "include/app/uagent_tool.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/strings.h"

namespace uagent {
Tool UagentTool(SelfDescriptionProvider describe,
                const ConfigProposalFactory& prepare,
                const std::shared_ptr<ConfigApprovals>& store) {
  // The file text an instructions diff was shown against, so the write can
  // refuse when the file changed while the user read the diff.
  auto shown = std::make_shared<std::pair<json, std::string>>();
  Tool tool = MakeTool(
      "uagent",
      "Inspect this running build: status, cli, commands, config, tools, "
      "prompt, routes, or instructions (the AGENTS.md and COORDINATOR.md "
      "files sessions and coordinators read at start). Secrets are never "
      "returned or accepted literally.",
      {{"type", "object"},
       {"properties",
        {{"action",
          {{"type", "string"},
           {"enum",
            json::array({"inspect", "configure", "set_instructions"})}}},
         {"topic",
          {{"type", "string"},
           {"enum", json::array({"status", "cli", "commands", "config", "tools",
                                 "prompt", "routes", "instructions"})}}},
         {"audience",
          {{"type", "string"},
           {"enum", json::array({"sessions", "coordinator"})},
           {"description",
            "set_instructions: AGENTS.md (every session) or COORDINATOR.md"}}},
         {"text",
          {{"type", "string"},
           {"description", "set_instructions: the whole new file, Markdown"}}},
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
      [describe = std::move(describe), store, shown](
          const json& arguments, const ToolContext&) -> ToolResult {
        const std::string action = JsonValue(arguments, "action", "");
        if (action == "inspect" &&
            JsonValue(arguments, "topic", "") == "instructions") {
          return ToolSuccess(JsonDump(InstructionFiles(CanonicalCwd()), 2));
        }
        if (action == "set_instructions") {
          // Reaching here means the user approved this exact text.
          bool coordinator = false, project = false;
          ParseInstructionTarget(JsonValue(arguments, "audience", ""),
                                 JsonValue(arguments, "scope", ""), coordinator,
                                 project);
          std::optional<std::string> base;
          if (shown->first == arguments) base = shown->second;
          const std::string error =
              WriteInstructionFile(coordinator, project, CanonicalCwd(),
                                   JsonValue(arguments, "text", ""), base);
          return error.empty()
                     ? ToolSuccess("saved; new and restarted sessions read it")
                     : ToolFailure(ToolErrorCode::kProcessFailed, error);
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
                             "configuration unavailable");
        }
        std::optional<ConfigProposal> taken = store->Take(arguments);
        if (!taken || !taken->ok) {
          return ToolFailure(
              ToolErrorCode::kPermissionDenied,
              "no approved configuration change for this request");
        }
        const ConfigProposal& approved = *taken;
        std::string error;
        if (!CommitConfigProposal(approved, error)) {
          return ToolFailure(ToolErrorCode::kProcessFailed, error);
        }
        std::string report = "saved for " + approved.target;
        for (const ConfigChangeEffect& effect : approved.effects) {
          report += "\n" + effect.key + ": " + ConfigEffectName(effect.effect);
        }
        return ToolSuccess(report);
      });
  const auto writes = [](const json& arguments) {
    const std::string action = JsonValue(arguments, "action", "");
    return action == "configure" || action == "set_instructions";
  };
  tool.mutates = writes;
  tool.redact_invalid_arguments = true;
  tool.approval_class = [writes](const json& arguments) {
    return writes(arguments) ? ApprovalClass::kMandatoryHuman
                             : ApprovalClass::kNone;
  };
  // Looking is not changing: what only a change takes is dropped from an
  // inspect call, which some models send along empty for every field.
  tool.canonicalize = [](json& arguments) {
    if (JsonValue(arguments, "action", "") != "inspect") return;
    for (const char* field : {"scope", "changes", "text", "audience"}) {
      arguments.erase(field);
    }
  };
  tool.validate =
      [prepare](const json& arguments) -> std::optional<ToolArgumentIssue> {
    if (JsonValue(arguments, "action", "") == "inspect") return std::nullopt;
    if (!prepare) {
      return ArgumentIssue("config.unavailable",
                           "configuration requires an interactive human");
    }
    if (JsonValue(arguments, "action", "") == "set_instructions") {
      bool coordinator = false, project = false;
      if (!ParseInstructionTarget(JsonValue(arguments, "audience", ""),
                                  JsonValue(arguments, "scope", ""),
                                  coordinator, project)) {
        return ArgumentIssue("instructions.target",
                             "audience must be sessions or coordinator, and "
                             "scope user or project");
      }
      if (!arguments.contains("text") ||
          JsonValue(arguments, "text", "").size() > kProjectDocBytes) {
        return ArgumentIssue("instructions.text",
                             "set_instructions needs the whole text, at most " +
                                 std::to_string(kProjectDocBytes / 1024) +
                                 " KiB",
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
    if (JsonValue(arguments, "action", "") == "set_instructions") {
      return JsonValue(arguments, "scope", "") + " " +
             JsonValue(arguments, "audience", "") + " instructions";
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
  tool.approval_preview = [prepare, store, shown](const json& arguments) {
    if (JsonValue(arguments, "action", "") == "set_instructions") {
      bool coordinator = false, project = false;
      ParseInstructionTarget(JsonValue(arguments, "audience", ""),
                             JsonValue(arguments, "scope", ""), coordinator,
                             project);
      const auto path = InstructionPath(coordinator, project, CanonicalCwd());
      *shown = {arguments, ReadInstructionFile(path)};
      return ConfigUnifiedDiff(shown->second, JsonValue(arguments, "text", ""),
                               path.string()) +
             "\nNew and restarted sessions read it.";
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
  if (prepare) {
    tool.description +=
        " configure changes registered UAGENT_* settings by an exact diff "
        "the user approves (YOLO cannot); inspect config first. "
        "set_instructions replaces one file (audience, scope, whole text) "
        "once the user approves the exact text.";
  } else {
    tool.parallel_safe = true;
    tool.parameters["properties"]["action"]["enum"] = json::array({"inspect"});
    tool.parameters["properties"].erase("scope");
    tool.parameters["properties"].erase("changes");
    tool.parameters["properties"].erase("text");
    tool.parameters["properties"].erase("audience");
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
