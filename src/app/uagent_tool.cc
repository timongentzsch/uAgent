// Copyright 2026 Timon Gentzsch

#include "include/app/uagent_tool.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
// What both writing tools share: every call needs a person, whatever the
// approval mode, and a rejected call does not echo its arguments.
void RequireHuman(Tool& tool) {
  tool.mutating = true;
  tool.approval_class = [](const json&) {
    return ApprovalClass::kMandatoryHuman;
  };
  tool.capabilities = 0;
  tool.category = "system";
  tool.intent = "setup";
}
}  // namespace

Tool UagentTool(SelfDescriptionProvider describe) {
  Tool tool = MakeTool(
      "uagent",
      "Inspect this running build, not the code in the folder: status, "
      "cli, commands, config, tools, "
      "prompt, routes, or instructions (the AGENTS.md files sessions read "
      "at start). Secrets are never returned or accepted literally.",
      {{"type", "object"},
       {"properties",
        {{"topic",
          {{"type", "string"},
           {"enum", json::array({"status", "cli", "commands", "config", "tools",
                                 "prompt", "routes", "instructions"})}}},
         {"name",
          {{"type", "string"},
           {"description", "exact setting or tool name"}}}}}},
      [describe = std::move(describe)](const json& arguments,
                                       const ToolContext&) -> ToolResult {
        if (JsonValue(arguments, "topic", "") == "instructions") {
          return ToolSuccess(JsonDump(InstructionFiles(CanonicalCwd()), 2));
        }
        SelfTopic topic = SelfTopic::kStatus;
        if (!ParseSelfTopic(JsonValue(arguments, "topic", "status"), topic)) {
          return ToolFailure(ToolErrorCode::kInvalidArguments, "unknown topic");
        }
        json value = describe(topic, Trim(JsonValue(arguments, "name", "")));
        if (topic == SelfTopic::kPrompt) {
          value.erase("inherited");
          value.erase("last_sent");
        }
        // The whole catalogue answers "which settings exist", so it is
        // returned complete but one line shorter per setting; a name asks
        // for the full facts of one.
        const bool catalogue = topic == SelfTopic::kConfig &&
                               JsonValue(arguments, "name", "").empty();
        if (catalogue) {
          for (json& setting : value["settings"]) {
            json brief = json::object();
            for (const char* field : {"key", "name", "type", "default",
                                      "effective", "source", "description"}) {
              if (setting.contains(field)) brief[field] = setting[field];
            }
            setting = std::move(brief);
          }
        }
        std::string output = JsonDump(value, 2);
        int64_t limit = topic == SelfTopic::kPrompt || catalogue
                            ? static_cast<int64_t>(output.size())
                            : -1;
        return ToolSuccess(std::move(output), limit);
      });
  // Inspection survives restricted policies.
  tool.capabilities = 0;
  tool.parallel_safe = true;
  tool.category = "system";
  tool.intent = "setup";
  tool.headline = [](const json& arguments) {
    return Headline{"Inspecting", "Inspected",
                    JsonValue(arguments, "topic", "status") + " " +
                        JsonValue(arguments, "name", "")};
  };
  return tool;
}

Tool UagentConfigureTool(const ConfigProposalFactory& prepare,
                         const std::shared_ptr<ConfigApprovals>& store) {
  Tool tool = MakeTool(
      "uagent_configure",
      "Change registered UAGENT_* settings by an exact diff the user "
      "approves (YOLO cannot); inspect config with uagent first.",
      {{"type", "object"},
       {"properties",
        {{"scope",
          {{"type", "string"},
           {"enum", json::array({"user", "project"})},
           {"description",
            "user saves for all conversations; project saves for this "
            "folder"}}},
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
                 {"description",
                  "required for set; omitted for unset. UAGENT_PROVIDERS "
                  "takes only the providers to add or change, as a JSON "
                  "object; null removes one"}}}}},
             {"required", json::array({"key", "operation"})}}}}}}},
       {"required", json::array({"scope", "changes"})}},
      // Reaching here means the human approved the preview built from these
      // exact arguments; the stored proposal carries the bytes they saw.
      [store](const json& arguments, const ToolContext&) -> ToolResult {
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
  RequireHuman(tool);
  // The proposal these arguments ask for. The schema has already held scope
  // to user or project.
  const auto propose = [prepare](const json& arguments) {
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    ParseConfigScope(JsonValue(arguments, "scope", ""), scope);
    std::vector<ConfigChange> changes;
    ConfigProposal proposal;
    if (!ParseConfigChanges(arguments, changes, proposal.error)) {
      return proposal;
    }
    return prepare(scope, changes);
  };
  tool.validate =
      [propose](const json& arguments) -> std::optional<ToolArgumentIssue> {
    const ConfigProposal proposal = propose(arguments);
    if (proposal.ok) return std::nullopt;
    return ArgumentIssue("config.rejected", proposal.error, "changes");
  };
  // A genuine one-liner: this is the call label, and it also reaches the debug
  // trace and compaction evidence, so the diff must not be built here.
  tool.headline = [](const json& arguments) {
    std::string detail;
    if (const json* list = JsonArray(arguments, "changes")) {
      for (const json& entry : *list) {
        if (!detail.empty()) detail += ", ";
        detail += Trim(JsonValue(entry, "key", ""));
        if (Trim(JsonValue(entry, "operation", "set")) == "unset") {
          detail += "=<unset>";
        }
      }
    }
    return Headline{"Configuring", "Configured",
                    JsonValue(arguments, "scope", "") + " · " + detail};
  };
  // Built only when a person is about to be asked. Preparing here also records
  // the proposal, so the bytes shown are exactly the bytes that can commit.
  tool.approval_preview = [propose, store](const json& arguments) {
    ConfigProposal proposal = propose(arguments);
    if (!proposal.ok) return "cannot apply: " + proposal.error;
    std::string preview = proposal.Preview();
    const auto expires = proposal.expires;
    store->Put(arguments, std::move(proposal), expires);
    return preview;
  };
  return tool;
}

Tool UagentInstructionsTool() {
  // The file text the diff was shown against, so the write can refuse when
  // the file changed while the user read the diff.
  auto shown = std::make_shared<std::pair<json, std::string>>();
  Tool tool = MakeTool(
      "uagent_instructions",
      "Replace one whole instructions file once the user approves the exact "
      "text.",
      {{"type", "object"},
       {"properties",
        {{"scope",
          {{"type", "string"},
           {"enum", json::array({"user", "project"})},
           {"description",
            "user for all conversations; project for this folder"}}},
         {"text",
          {{"type", "string"},
           {"description", "the whole new file, Markdown"}}}}},
       {"required", json::array({"scope", "text"})}},
      // Reaching here means the user approved this exact text.
      [shown](const json& arguments, const ToolContext&) {
        bool project = false;
        ParseInstructionScope(JsonValue(arguments, "scope", ""), project);
        std::optional<std::string> base;
        if (shown->first == arguments) base = shown->second;
        const std::string error = WriteInstructionFile(
            project, CanonicalCwd(), JsonValue(arguments, "text", ""), base);
        return error.empty()
                   ? ToolSuccess("saved; new and restarted sessions read it")
                   : ToolFailure(ToolErrorCode::kProcessFailed, error);
      });
  RequireHuman(tool);
  tool.validate =
      [](const json& arguments) -> std::optional<ToolArgumentIssue> {
    if (JsonValue(arguments, "text", "").size() <= ProjectDocBytes()) {
      return std::nullopt;
    }
    return ArgumentIssue("instructions.text",
                         "text takes at most " +
                             std::to_string(ProjectDocBytes() / 1024) + " KiB",
                         "text");
  };
  tool.headline = [](const json& arguments) {
    return Headline{"Configuring", "Configured",
                    JsonValue(arguments, "scope", "") + " instructions"};
  };
  tool.approval_preview = [shown](const json& arguments) {
    bool project = false;
    ParseInstructionScope(JsonValue(arguments, "scope", ""), project);
    const auto path = InstructionPath(project, CanonicalCwd());
    *shown = {arguments, ReadInstructionFile(path)};
    return ConfigUnifiedDiff(shown->second, JsonValue(arguments, "text", ""),
                             path.string()) +
           "\nNew and restarted sessions read it.";
  };
  return tool;
}

}  // namespace uagent
