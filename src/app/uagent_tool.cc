// Copyright 2026 Timon Gentzsch

#include "include/app/uagent_tool.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/prompt.h"
#include "include/app/library.h"
#include "include/app/permissions.h"
#include "include/app/schedule.h"
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/project.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"

namespace uagent {
namespace {
// The skill that unlocks the tool that inspects and changes µAgent itself:
// most conversations never open it, so they never carry the tool.
constexpr char kConfigSkill[] = "uagent-config";

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
  return JsonPick(task, {"id", "name", "schedule", "check", "permissions",
                         "model", "cwd", "enabled", "upcoming", "error"});
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

namespace {
ToolResult Show(const json& arguments,
                const SelfDescriptionProvider& describe) {
  const std::string asked = JsonValue(arguments, "topic", "status");
  const std::string name = Trim(JsonValue(arguments, "name", ""));
  json value;
  // Returned to the last byte: cut short, it would answer wrongly.
  bool whole = false;
  if (asked == "instructions") {
    value = InstructionFiles(Workspace().path);
  } else if (asked == "style") {
    value = {{"path", UserStylePath().string()}, {"text", ReadUserStyle()}};
  } else if (asked == "permissions") {
    value = PermissionRulesControl({{"action", "list"}});
  } else if (asked == "skills") {
    const json listed = SkillControl({{"action", "list"}}, CanonicalCwd());
    value = json::array();
    for (const json& skill : listed["items"]) {
      value.push_back(
          JsonPick(skill, {"name", "description", "status", "scope", "path"}));
    }
  } else {
    SelfTopic topic = SelfTopic::kStatus;
    if (!ParseSelfTopic(asked, topic)) {
      return ToolFailure(ToolErrorCode::kInvalidArguments, "unknown topic");
    }
    value = describe(topic, name);
    if (topic == SelfTopic::kPrompt) {
      value.erase("inherited");
      value.erase("last_sent");
      // The file `write` replaces.
      value["saved"] = ReadSavedPrompt();
    }
    // The whole catalogue answers "which settings exist", so it is
    // returned complete but one line shorter per setting; a name asks
    // for the full facts of one.
    const bool catalogue = topic == SelfTopic::kConfig && name.empty();
    if (catalogue) {
      for (json& setting : value["settings"]) {
        setting = JsonPick(setting, {"key", "name", "type", "default",
                                     "effective", "source", "description"});
      }
    }
    whole = topic == SelfTopic::kPrompt || catalogue;
  }
  if (value.contains("error")) {
    return ToolFailure(ToolErrorCode::kProcessFailed,
                       JsonValue(value, "error", ""));
  }
  std::string output = JsonDump(value, 2);
  const int64_t limit = whole ? static_cast<int64_t>(output.size()) : -1;
  return ToolSuccess(std::move(output), limit);
}

// A file `write` replaces, named by kind and a skill by its folder, never by
// a path from the model: where it is, the most it may hold, when a change
// shows, and why this text cannot be saved (empty when it can).
struct Edited {
  std::filesystem::path path;
  size_t limit;
  const char* effect;
  std::string issue;
};

Edited EditedFile(const json& arguments) {
  constexpr const char* kSessions = "New and restarted sessions read it.";
  const std::string kind = JsonValue(arguments, "file", "");
  const std::string name = JsonValue(arguments, "name", "");
  const std::string text = JsonValue(arguments, "text", "");
  const json parsed = json::parse(text, nullptr, false);
  const std::filesystem::path folder = Workspace().path;
  if (kind == "style") {
    return {UserStylePath(), kUserStyleBytes,
            "Open web pages apply it when this turn ends.", ""};
  }
  if (kind == "prompt") {
    return {SavedPromptPath(), kSavedPromptBytes, kSessions,
            text.empty() ? "" : PromptOverlayIssue(parsed)};
  }
  if (kind == "mcp") {
    // A remote folder's servers are not started, so its file is not read.
    return {folder / kMcpConfigFile, kMcpConfigBytes,
            "New and restarted sessions here ask to trust it, then start its "
            "servers.",
            RemoteWorkspace()    ? "a remote folder's .mcp.json is not used"
            : parsed.is_object() ? ""
                                 : ".mcp.json is a JSON object"};
  }
  if (kind == "skill") {
    if (!LibraryName(name)) {
      return {{}, 0, "", "skill takes name: letters, digits, - or _"};
    }
    return {std::filesystem::path(GlobalBase()) / "skills" / name / kSkillFile,
            kSkillBodyBytes, kSessions, ""};
  }
  bool project = false;
  const bool scoped =
      ParseInstructionScope(JsonValue(arguments, "scope", ""), project);
  return {InstructionPath(project, folder), ProjectDocBytes(), kSessions,
          scoped ? "" : "instructions take scope user or project"};
}

// What it holds now; an instruction file may be on the folder's host.
std::string EditedText(const json& arguments, const Edited& file) {
  return JsonValue(arguments, "file", "") == "instructions"
             ? ReadInstructionFile(file.path)
             : ReadFile(file.path.string(), file.limit).value_or("");
}

// Replaces the file, unless it no longer reads `shown`, the text the
// approved diff was made against.
ToolResult SaveEdited(const json& arguments, const Edited& file,
                      const std::string& shown) {
  const std::string kind = JsonValue(arguments, "file", "");
  const std::string text = JsonValue(arguments, "text", "");
  std::string error;
  if (kind == "instructions") {
    error = WriteInstructionFile(JsonValue(arguments, "scope", "") == "project",
                                 Workspace().path, text, shown);
  } else if (kind == "prompt") {
    error = WriteSavedPrompt(text, shown);
  } else if (kind == "skill") {
    // The Library's own write, with its lease and its checks.
    const json saved = SkillControl(
        {{"action", "set"},
         {"key", "global/" + JsonValue(arguments, "name", "")},
         {"content", text},
         {"revision", PathExists(file.path)
                          ? DocumentRevision(file.path.string(), shown)
                          : ""}},
        CanonicalCwd());
    error = JsonValue(saved, "error", "");
  } else {
    // Private when new, the folder's .mcp.json too: it may carry a key.
    error = WriteEditedFile(file.path, EditedText(arguments, file), text, shown,
                            /*shared=*/false);
  }
  return error.empty() ? ToolSuccess(std::string("saved. ") + file.effect)
                       : ToolFailure(ToolErrorCode::kProcessFailed, error);
}

ToolResult CommitSettings(const ConfigProposal& approved) {
  std::string error;
  if (!CommitConfigProposal(approved, error)) {
    return ToolFailure(ToolErrorCode::kProcessFailed, error);
  }
  std::string report = "saved for " + approved.target;
  for (const ConfigChangeEffect& effect : approved.effects) {
    report += "\n" + effect.key + ": " + ConfigEffectName(effect.effect);
  }
  return ToolSuccess(report);
}

// A genuine one-liner: this is the call label, and it also reaches the debug
// trace and compaction evidence, so no diff is built here.
Headline UagentHeadline(const json& arguments) {
  const auto word = [&arguments](const char* field) {
    return JsonValue(arguments, field, "");
  };
  if (word("action") == "show") {
    return {"Inspecting", "Inspected",
            Trim(JsonValue(arguments, "topic", "status") + " " + word("name"))};
  }
  std::string target =
      Trim(word("scope") + " " + word("file") + " " + word("name"));
  const char* separator = " · ";
  for (const json& entry : JsonValue(arguments, "changes", json::array())) {
    target += separator + Trim(JsonValue(entry, "key", ""));
    if (JsonValue(entry, "operation", "") == "unset") target += "=<unset>";
    separator = ", ";
  }
  return {"Configuring", "Configured", target};
}

// set and write, added where a person can approve them. Each prepares and
// previews; the mandatory-human approval lane decides, and what then runs is
// what the approved preview recorded.
void OfferChanges(Tool& tool, ConfigProposalFactory prepare) {
  tool.description +=
      " set changes UAGENT_* settings; write replaces one whole file. The "
      "user approves the exact diff of each, in every approval mode: show "
      "what is there first.";
  tool.parameters["properties"].update(json::parse(R"json({
    "action":{"type":"string","enum":["show","set","write"]},
    "name":{"type":"string","description":
      "show: one setting or tool by exact name; write skill: its folder name"},
    "scope":{"type":"string","enum":["user","project"],"description":
      "set, write instructions: user for all conversations, project for this folder"},
    "changes":{"type":"array","description":"set: one entry per setting",
      "items":{"type":"object","properties":{
        "key":{"type":"string","description":"exact UAGENT_* name"},
        "operation":{"type":"string","enum":["set","unset"]},
        "value":{"type":"string","description":
          "required for set; omitted for unset. UAGENT_PROVIDERS takes only the providers to add or change, as a JSON object, a key only as $VARIABLE; null removes one"}},
        "required":["key","operation"]}},
    "file":{"type":"string","enum":["instructions","style","prompt","mcp","skill"],
      "description":
      "instructions: AGENTS.md at scope; style: CSS over the web UI's design tokens; prompt: JSON {\"replace\":{\"## Section\":\"text\"},\"append\":\"text\"} over the system message; mcp: this folder's .mcp.json; skill: the SKILL.md of skill `name`"},
    "text":{"type":"string","description":
      "write: the whole new file; empty resets style or prompt"}})json"));
  // `text` is checked below: empty is a value here, not a field left out.
  tool.action_requires = {"action",
                          {{"set", {"scope", "changes"}}, {"write", {"file"}}}};
  // A change keeps its place among the calls around it.
  tool.parallel_safe = false;
  // Every change needs a person, whatever the approval mode.
  tool.mutates = [](const json& arguments) {
    return JsonValue(arguments, "action", "") != "show";
  };
  tool.approval_class = [mutates = tool.mutates](const json& arguments) {
    return mutates(arguments) ? ApprovalClass::kMandatoryHuman
                              : ApprovalClass::kNone;
  };
  // The proposal these arguments ask for. The schema has already held scope
  // to user or project.
  const auto propose = [prepare = std::move(prepare)](const json& arguments) {
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
    if (JsonValue(arguments, "action", "") == "set") {
      const ConfigProposal proposal = propose(arguments);
      if (proposal.ok) return std::nullopt;
      return ArgumentIssue("config.rejected", proposal.error, "changes");
    }
    if (JsonValue(arguments, "action", "") != "write") return std::nullopt;
    const Edited file = EditedFile(arguments);
    const std::string issue =
        !file.issue.empty()           ? file.issue
        : !arguments.contains("text") ? "write takes text: the whole new file"
        : JsonValue(arguments, "text", "").size() > file.limit
            ? "text takes at most " + std::to_string(file.limit / 1024) + " KiB"
            : "";
    if (issue.empty()) return std::nullopt;
    return ArgumentIssue("file.rejected", issue, "file");
  };
  auto approved =
      std::make_shared<ApprovedProposals<std::function<ToolResult()>>>();
  // Built only when a person is about to be asked, and recorded with what it
  // would do: the bytes shown are exactly the bytes that can be written.
  tool.approval_preview = [propose, approved](const json& arguments) {
    if (JsonValue(arguments, "action", "") == "set") {
      const ConfigProposal proposal = propose(arguments);
      if (!proposal.ok) return "cannot apply: " + proposal.error;
      approved->Put(
          arguments, [proposal] { return CommitSettings(proposal); },
          proposal.expires);
      return proposal.Preview();
    }
    const Edited file = EditedFile(arguments);
    if (!file.issue.empty()) return "cannot apply: " + file.issue;
    const std::string shown = EditedText(arguments, file);
    approved->Put(
        arguments,
        [arguments, file, shown] { return SaveEdited(arguments, file, shown); },
        std::chrono::steady_clock::time_point::max());
    return ConfigUnifiedDiff(shown, JsonValue(arguments, "text", ""),
                             file.path.string()) +
           "\n" + file.effect;
  };
  tool.run = [show = std::move(tool.run), approved](
                 const json& arguments, const ToolContext& context) {
    if (JsonValue(arguments, "action", "") == "show") {
      return show(arguments, context);
    }
    // Reaching here means a person approved the preview built from these
    // exact arguments.
    const auto commit = approved->Take(arguments);
    return commit ? (*commit)()
                  : ToolFailure(ToolErrorCode::kPermissionDenied,
                                "no approved change for this request");
  };
}
}  // namespace

Tool UagentTool(SelfDescriptionProvider describe,
                ConfigProposalFactory prepare) {
  Tool tool = MakeTool(
      "uagent",
      "uAgent itself: this running build and its configuration, not the code "
      "in the folder. show reads a topic and never returns a secret.",
      json::parse(R"json({"type":"object","properties":{
        "action":{"type":"string","enum":["show"]},
        "topic":{"type":"string","enum":["status","cli","commands","config",
          "tools","prompt","routes","instructions","style","mcp",
          "permissions","skills"],"description":
          "instructions: the AGENTS.md files; style: the web UI's stylesheet; permissions: saved approval rules"},
        "name":{"type":"string","description":
          "one setting or tool by exact name"}},
        "required":["action"]})json"),
      [describe = std::move(describe)](const json& arguments,
                                       const ToolContext&) {
        return Show(arguments, describe);
      });
  // Inspection survives restricted policies.
  tool.capabilities = 0;
  tool.parallel_safe = true;
  tool.category = "system";
  tool.skill = kConfigSkill;
  tool.headline = UagentHeadline;
  if (prepare) OfferChanges(tool, std::move(prepare));
  return tool;
}

}  // namespace uagent
