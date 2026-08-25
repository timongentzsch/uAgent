// Copyright 2026 Timon Gentzsch

#include "include/tools/configure.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

bool ParseScope(const std::string& name, ConfigProposalScope& scope) {
  if (name == "user") {
    scope = ConfigProposalScope::kUser;
    return true;
  }
  if (name == "project") {
    scope = ConfigProposalScope::kProject;
    return true;
  }
  return false;
}

bool ParseChanges(const json& arguments, std::vector<ConfigChange>& changes,
                  std::string& error) {
  const json* list = JsonArray(arguments, "changes");
  if (!list || list->empty()) {
    error = "changes must be a non-empty array";
    return false;
  }
  for (const json& entry : *list) {
    ConfigChange change;
    change.key = Trim(JsonValue(entry, "key", ""));
    std::string operation = Trim(JsonValue(entry, "operation", "set"));
    if (change.key.empty()) {
      error = "each change needs a key";
      return false;
    }
    if (operation == "unset") {
      change.unset = true;
    } else if (operation != "set") {
      error = "operation must be set or unset";
      return false;
    } else {
      auto value = entry.find("value");
      if (value == entry.end()) {
        error = "set needs a value for " + change.key;
        return false;
      }
      change.value =
          value->is_string() ? value->get<std::string>() : JsonDump(*value);
    }
    changes.push_back(std::move(change));
  }
  return true;
}

// The approval preview and the commit must describe the same request, so both
// key off the exact arguments. The stored arguments are compared as well, so a
// digest collision cannot substitute one approved request for another.
std::string ProposalKey(const json& arguments) {
  return HashHex(JsonDump(arguments));
}

}  // namespace

Tool ConfigureTool(ConfigProposalFactory prepare,
                   std::shared_ptr<ConfigProposalStore> store) {
  Tool tool = MakeTool(
      "uagent_configure",
      "Persist a change to \u00b5Agent's own configuration. Accepts only "
      "registered UAGENT_* settings; use uagent_info topic=config first to "
      "read the current value, its source and whether a change needs a "
      "restart. The user is shown an exact diff and must approve it: --yolo "
      "does not apply, and a headless or delegated run cannot commit. "
      "Credentials are rejected here and must be entered by the user.",
      {{"type", "object"},
       {"properties",
        {{"scope",
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
       {"required", json::array({"scope", "changes"})}},
      [prepare, store](const json& arguments,
                       const ToolContext&) -> ToolResult {
        // Reaching this point means the human approved the preview built from
        // these exact arguments; the stored proposal carries the bytes they
        // saw.
        ConfigProposal approved =
            store->Take(ProposalKey(arguments), arguments);
        if (!approved.ok) {
          return ToolFailure(
              ToolErrorCode::kPermissionDenied,
              "error: no approved configuration change for this request");
        }
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
  tool.mutating = true;
  tool.approval_class = [](const json&) {
    return ApprovalClass::kMandatoryHuman;
  };
  tool.validate =
      [prepare](const json& arguments) -> std::optional<ToolArgumentIssue> {
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    if (!ParseScope(Trim(JsonValue(arguments, "scope", "")), scope)) {
      return ToolArgumentIssue{"scope", "config.scope",
                               "scope must be user or project"};
    }
    std::vector<ConfigChange> changes;
    std::string error;
    if (!ParseChanges(arguments, changes, error)) {
      return ToolArgumentIssue{"changes", "config.changes", error};
    }
    ConfigProposal proposal = prepare(scope, changes);
    if (!proposal.ok) {
      return ToolArgumentIssue{"changes", "config.rejected", proposal.error};
    }
    return std::nullopt;
  };
  // A genuine one-liner: this is the call label, and it also reaches the debug
  // trace and compaction evidence, so the diff must not be built here.
  tool.summary = [](const json& arguments) {
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
    ConfigProposalScope scope = ConfigProposalScope::kUser;
    std::vector<ConfigChange> changes;
    std::string error;
    if (!ParseScope(Trim(JsonValue(arguments, "scope", "")), scope) ||
        !ParseChanges(arguments, changes, error)) {
      return std::string("invalid configuration request");
    }
    ConfigProposal proposal = prepare(scope, changes);
    if (!proposal.ok) return "cannot apply: " + proposal.error;
    std::string preview = proposal.Preview();
    store->Put(ProposalKey(arguments), arguments, std::move(proposal));
    return preview;
  };
  tool.capabilities = Capability(ToolCapability::kMutate);
  tool.available_in_lean = false;
  return tool;
}

}  // namespace uagent
