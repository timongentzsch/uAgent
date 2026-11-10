// Copyright 2026 Timon Gentzsch

#include "include/app/uagent_tool.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/app/schedule.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/project.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"

namespace uagent {
namespace {
// The skill that unlocks the tools that inspect and change µAgent itself:
// most conversations never open it, so they never carry these tools.
constexpr char kConfigSkill[] = "uagent-config";

// What both writing tools share: every call needs a person, whatever the
// approval mode, and a rejected call does not echo its arguments.
void RequireHuman(Tool& tool) {
  tool.mutating = true;
  tool.approval_class = [](const json&) {
    return ApprovalClass::kMandatoryHuman;
  };
  tool.capabilities = 0;
  tool.category = "system";
  tool.skill = kConfigSkill;
}

// How much an approval mode lets a run do alone: a task gets no more than
// the conversation that scheduled it.
int Reach(ApprovalMode mode) {
  switch (mode) {
    case ApprovalMode::kPlan:
      return 0;
    case ApprovalMode::kAsk:
      return 1;
    case ApprovalMode::kAuto:
      return 2;
    case ApprovalMode::kYolo:
      return 3;
  }
  return 0;
}

// A task as the model needs it: enough to check it and name it again.
json TaskBrief(const json& task) {
  json brief = json::object();
  for (const char* field : {"id", "name", "schedule", "check", "permissions",
                            "model", "cwd", "enabled", "upcoming", "error"}) {
    if (task.contains(field)) brief[field] = task[field];
  }
  return brief;
}

ToolResult Schedule(const json& arguments, const std::string& model) {
  const std::string action = JsonValue(arguments, "action", "");
  const std::string key = JsonValue(arguments, "key", "");
  // Tasks wait for the web host; only it starts runs.
  const bool hosted =
      FileLease::HasLiveOwner(UagentDir(kWebDir) + "/" + kMasterLockFile);
  json out = json::object();
  if (action == "list") {
    json listed = ScheduleControl({{"action", "list"}});
    if (listed.contains("error")) {
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         JsonValue(listed, "error", ""));
    }
    out["tasks"] = json::array();
    for (const json& task : listed["tasks"]) {
      out["tasks"].push_back(TaskBrief(task));
    }
    // The latest runs say how a task is doing; older ones are in the view.
    out["runs"] = json::array();
    const json& runs = listed["runs"];
    for (size_t i = runs.size() > 10 ? runs.size() - 10 : 0; i < runs.size();
         ++i) {
      const json& run = runs[i];
      out["runs"].push_back({{"task_id", run["task_id"]},
                             {"scheduled_for", run["scheduled_for"]},
                             {"status", run["status"]},
                             {"error", JsonValue(run, "error", "")}});
    }
  } else {
    json current = json::object();
    if (!key.empty()) {
      json found = ScheduleControl({{"action", "get"}, {"key", key}});
      if (found.contains("error")) {
        return ToolFailure(ToolErrorCode::kNotFound,
                           "no scheduled task has id " + key +
                               "; list shows the ids, and save without key "
                               "creates a task");
      }
      current = found["item"];
    }
    json request = {{"action", action},
                    {"key", key},
                    {"revision", JsonValue(current, "revision", "")}};
    if (action == "save") {
      // A new task runs where and on what this conversation does, in the
      // folder itself: a check's state and scripts are there.
      json task = key.empty() ? json{{"cwd", Workspace().path},
                                     {"host", Workspace().host},
                                     {"model", model},
                                     {"environment", "local"}}
                              : current;
      for (const char* field : {"name", "prompt", "schedule"}) {
        task[field] = arguments[field];
      }
      task["check"] = JsonValue(arguments, "check", "");
      task["permissions"] = JsonValue(arguments, "permissions",
                                      ApprovalModeName(CurrentApprovalMode()));
      request["task"] = std::move(task);
    }
    json result = ScheduleControl(request);
    if (result.contains("error")) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         JsonValue(result, "error", ""));
    }
    out = result.contains("item") ? TaskBrief(result["item"]) : result;
  }
  out["host_running"] = hosted;
  if (!hosted) out["note"] = "tasks run only while uagent --web runs";
  return ToolSuccess(JsonDump(out, 2));
}
}  // namespace

Tool ScheduleTool(std::function<std::string()> model) {
  Tool tool = MakeTool(
      "schedule",
      "Tasks the uAgent host runs by itself; each run is a new conversation "
      "in this folder on this model. save creates one or replaces key's.",
      json::parse(R"json({"type":"object","properties":{
        "action":{"type":"string",
          "enum":["list","save","pause","resume","forget","run"]},
        "key":{"type":"string","description":"an existing task's id from list; leave out to create"},
        "name":{"type":"string"},
        "prompt":{"type":"string"},
        "schedule":{"type":"object","description":
          "type once+at (unix) | interval+seconds (>=60) | weekly+days (0=Sun)+time (HH:MM)+timezone (IANA)"},
        "check":{"type":"string","description":
          "optional shell command run first in the folder: exit 0 starts the run with its output, 1 skips it without a model call"},
        "permissions":{"type":"string","enum":["plan","ask","auto","yolo"],
          "description":"default and most: this conversation's"}},
        "required":["action"]})json"),
      [model = std::move(model)](const json& arguments, const ToolContext&) {
        return Schedule(arguments, model());
      });
  tool.action_requires = {"action",
                          {{"save", {"name", "prompt", "schedule"}},
                           {"pause", {"key"}},
                           {"resume", {"key"}},
                           {"forget", {"key"}},
                           {"run", {"key"}}}};
  tool.mutates = [](const json& arguments) {
    return JsonValue(arguments, "action", "") != "list";
  };
  tool.routine = [](const json& arguments) {
    return JsonValue(arguments, "action", "") == "list";
  };
  tool.validate =
      [](const json& arguments) -> std::optional<ToolArgumentIssue> {
    if (!arguments.contains("permissions")) return std::nullopt;
    ApprovalMode asked = ApprovalMode::kAsk;
    const ApprovalMode own = CurrentApprovalMode();
    if (ParseApprovalMode(JsonValue(arguments, "permissions", ""), asked) &&
        Reach(asked) <= Reach(own)) {
      return std::nullopt;
    }
    return ArgumentIssue("schedule.permissions",
                         std::string("permissions may be at most this "
                                     "conversation's: ") +
                             ApprovalModeName(own),
                         "permissions");
  };
  // A run executes whatever its prompt leads to, unattended.
  tool.capabilities = Capability(ToolCapability::kInspect) |
                      Capability(ToolCapability::kMutate) |
                      Capability(ToolCapability::kExecute);
  tool.category = "system";
  tool.headline = [](const json& arguments) {
    const std::string action = JsonValue(arguments, "action", "");
    const std::string target =
        JsonValue(arguments, "name", JsonValue(arguments, "key", ""));
    if (action == "list") {
      return Headline{"Listing", "Listed", "scheduled tasks"};
    }
    if (action == "save") return Headline{"Scheduling", "Scheduled", target};
    if (action == "pause") return Headline{"Pausing", "Paused", target};
    if (action == "resume") return Headline{"Resuming", "Resumed", target};
    if (action == "run") return Headline{"Starting", "Started", target};
    return Headline{"Deleting", "Deleted", target};
  };
  return tool;
}

Tool UagentTool(SelfDescriptionProvider describe) {
  Tool tool = MakeTool(
      "uagent",
      "Inspect this running build, not the code in the folder: status, "
      "cli, commands, config, tools, "
      "prompt, routes, instructions (the AGENTS.md files sessions read "
      "at start), or style (the web UI's stylesheet). Secrets are never "
      "returned or accepted literally.",
      {{"type", "object"},
       {"properties",
        {{"topic",
          {{"type", "string"},
           {"enum",
            json::array({"status", "cli", "commands", "config", "tools",
                         "prompt", "routes", "instructions", "style"})}}},
         {"name",
          {{"type", "string"},
           {"description", "exact setting or tool name"}}}}}},
      [describe = std::move(describe)](const json& arguments,
                                       const ToolContext&) -> ToolResult {
        if (JsonValue(arguments, "topic", "") == "instructions") {
          return ToolSuccess(JsonDump(InstructionFiles(Workspace().path), 2));
        }
        if (JsonValue(arguments, "topic", "") == "style") {
          return ToolSuccess(JsonDump(
              {{"path", UserStylePath().string()}, {"text", ReadUserStyle()}},
              2));
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
  tool.skill = kConfigSkill;
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
                  "object, a key only as $VARIABLE; null removes one"}}}}},
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

namespace {
// The file a uagent_file call names: the web UI's stylesheet, or one of the
// two instruction files.
bool IsStyle(const json& arguments) {
  return JsonValue(arguments, "file", "") == "style";
}

bool InProject(const json& arguments) {
  bool project = false;
  ParseInstructionScope(JsonValue(arguments, "scope", ""), project);
  return project;
}

std::filesystem::path EditedPath(const json& arguments) {
  return IsStyle(arguments)
             ? UserStylePath()
             : InstructionPath(InProject(arguments), Workspace().path);
}

std::string EditedText(const json& arguments) {
  return IsStyle(arguments) ? ReadUserStyle()
                            : ReadInstructionFile(EditedPath(arguments));
}

// When the change shows, which the preview and the result both say.
const char* EditedEffect(const json& arguments) {
  return IsStyle(arguments) ? "Open web pages apply it when this turn ends."
                            : "New and restarted sessions read it.";
}
}  // namespace

Tool UagentFileTool() {
  // The file text the diff was shown against, so the write can refuse when
  // the file changed while the user read the diff.
  auto shown = std::make_shared<std::pair<json, std::string>>();
  Tool tool = MakeTool(
      "uagent_file",
      "Replace one whole file once the user approves the exact text: an "
      "instructions file, or the web UI's stylesheet (overrides of its "
      "design tokens; empty text resets the look).",
      {{"type", "object"},
       {"properties",
        {{"file",
          {{"type", "string"},
           {"enum", json::array({"instructions", "style"})}}},
         {"scope",
          {{"type", "string"},
           {"enum", json::array({"user", "project"})},
           {"description",
            "instructions only: user for all conversations; project for this "
            "folder"}}},
         {"text",
          {{"type", "string"},
           {"description", "the whole new file: Markdown or CSS"}}}}},
       {"required", json::array({"file", "text"})}},
      // Reaching here means the user approved this exact text.
      [shown](const json& arguments, const ToolContext&) {
        std::optional<std::string> base;
        if (shown->first == arguments) base = shown->second;
        const std::string text = JsonValue(arguments, "text", "");
        const std::string error =
            IsStyle(arguments)
                ? WriteUserStyle(text, base)
                : WriteInstructionFile(InProject(arguments), Workspace().path,
                                       text, base);
        return error.empty()
                   ? ToolSuccess(std::string("saved. ") +
                                 EditedEffect(arguments))
                   : ToolFailure(ToolErrorCode::kProcessFailed, error);
      });
  RequireHuman(tool);
  tool.validate =
      [](const json& arguments) -> std::optional<ToolArgumentIssue> {
    bool project = false;
    if (!IsStyle(arguments) &&
        !ParseInstructionScope(JsonValue(arguments, "scope", ""), project)) {
      return ArgumentIssue("instructions.scope",
                           "instructions take scope user or project", "scope");
    }
    const size_t limit =
        IsStyle(arguments) ? kUserStyleBytes : ProjectDocBytes();
    if (JsonValue(arguments, "text", "").size() <= limit) return std::nullopt;
    return ArgumentIssue(
        "file.text",
        "text takes at most " + std::to_string(limit / 1024) + " KiB", "text");
  };
  tool.headline = [](const json& arguments) {
    return Headline{"Configuring", "Configured",
                    IsStyle(arguments)
                        ? std::string("style")
                        : JsonValue(arguments, "scope", "") + " instructions"};
  };
  tool.approval_preview = [shown](const json& arguments) {
    *shown = {arguments, EditedText(arguments)};
    return ConfigUnifiedDiff(shown->second, JsonValue(arguments, "text", ""),
                             EditedPath(arguments).string()) +
           "\n" + EditedEffect(arguments);
  };
  return tool;
}

}  // namespace uagent
