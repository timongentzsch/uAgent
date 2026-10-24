// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/app/launch.h"
#include "include/app/panel.h"
#include "include/app/session.h"
#include "include/core/capture.h"
#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"
#include "src/app/panel_internal.h"

namespace uagent::panel {
namespace {
constexpr size_t kMessageBytes = KiB(8);
constexpr size_t kDiffBytes = KiB(16);

std::string Brief(const json& brief) {
  std::string text = "Objective: " + JsonValue(brief, "objective", "");
  for (const auto& [key, label] : {std::pair{"output", "Expected output"},
                                   {"done_when", "Done when"},
                                   {"boundaries", "Boundaries"}}) {
    const std::string value = JsonValue(brief, key, "");
    if (!value.empty()) text += "\n" + std::string(label) + ": " + value;
  }
  return text +
         "\n\n(From this folder's moderator. End with a short report: what "
         "you changed, how you verified it, and anything left open.)";
}

// Starts a thread on a brief.
ToolResult Spawn(const std::string& folder, const json& a,
                 const std::function<std::string()>& own_model) {
  const std::string title = Utf8Prefix(JsonValue(a, "title", ""), 120);
  const std::string objective = JsonValue(a, "objective", "");
  if (title.empty() || objective.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "spawn needs a title and an objective");
  }
  const std::string panel = PanelId(folder);
  const int64_t cap = LongSetting(Cfg("UAGENT_PANEL_MAX_THREADS"));
  const std::vector<SessionInfo> threads = OwnThreads(folder);
  // Each probe opens the thread's socket: one per thread serves both checks.
  std::vector<bool> busy;
  busy.reserve(threads.size());
  for (const SessionInfo& info : threads) busy.push_back(Working(info));
  // The cap is on threads at work.
  const int64_t working = std::ranges::count(busy, true);
  if (working >= cap) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       std::to_string(cap) +
                           " threads are already working; wait for one to "
                           "finish or stop one");
  }
  const double limit = DoubleSetting(Cfg("UAGENT_PANEL_DAILY_SPEND_USD"));
  // Each thread gets an equal share of what is left for the free slots.
  const double left = limit > 0 ? limit - SpentToday(folder, threads, busy) : 0;
  const double budget =
      left / static_cast<double>(std::max<int64_t>(1, cap - working));
  if (limit > 0 && budget < 0.01) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       FmtCost(std::max(left, 0.0)) + " of today's " +
                           FmtCost(limit) +
                           " spend limit is left, too little to share among " +
                           std::to_string(cap - working) +
                           " more threads; ask the user to raise "
                           "UAGENT_PANEL_DAILY_SPEND_USD or wait for tomorrow");
  }
  // Unasked, a folder outside git runs in place: it can have no worktree.
  std::string environment = JsonValue(a, "environment", "");
  if (environment.empty()) {
    environment = StringSetting(Cfg("UAGENT_PANEL_ENVIRONMENT"));
    if (environment == "worktree" && !InGitRepository(folder)) {
      environment = "local";
    }
  }
  const LaunchPaths launch = PlanLaunch(folder, environment == "worktree",
                                        "thread-", HashHex(MakeSessionId()));
  if (environment == "worktree") {
    if (std::string failed = CreateWorktree(folder, launch.cwd);
        !failed.empty()) {
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         failed +
                             "; spawn with environment=local to run in the "
                             "folder itself");
    }
  }
  json thread = {{"parent", panel},
                 {"parent_session", PanelPath(folder)},
                 {"lifetime", kSessionKindThread},
                 {"folder", folder},
                 {"day", Today()},
                 {"ceiling", {{"budget_usd", budget}}}};
  if (std::string step = JsonValue(a, "step", ""); !step.empty()) {
    thread["step"] = std::move(step);
  }
  thread["brief"] = {{"objective", objective},
                     {"output", JsonValue(a, "output", "")},
                     {"done_when", JsonValue(a, "done_when", "")},
                     {"boundaries", JsonValue(a, "boundaries", "")}};
  Options options;
  // Named, else the panel's own: a thread is told its model and
  // resolves the rest like any session. It inherits no endpoint from this
  // process.
  std::string model = Trim(JsonValue(a, "model", ""));
  if (model.empty()) model = own_model();
  if (!model.empty()) options.overrides["UAGENT_MODEL"] = model;
  options.session = {{"kind", kSessionKindThread}, {"delegation", thread}};
  std::string error;
  session::Connection connection = session::Open(
      ExecutablePath(), launch.cwd, launch.path, title, options, error);
  if (!connection.socket) {
    return Unavailable(error);
  }
  error = SendWhenReady(connection, launch.path,
                        {{"kind", "submit"}, {"text", Brief(thread["brief"])}},
                        true);
  if (!error.empty()) {
    return Unavailable(error);
  }
  return ToolSuccess(
      JsonDump({{"session_id", SessionIdFor(launch.path)},
                {"title", title},
                {"cwd", launch.cwd},
                {"environment", environment},
                // Said where it applies: waiting costs nothing, polling does.
                {"next",
                 "It reports when its turn ends, and that report starts your "
                 "next turn. Unless other work is waiting, end this turn with "
                 "one line saying what you started: an empty answer counts "
                 "as a failure."}}));
}

// Guidance into a session of the folder: a running turn takes it as
// steering, an idle one as its next message. Either way it is labelled. Only
// the panel's own threads are started again for it; they always run
// within their ceiling, where the user's own sessions would not.
ToolResult Message(const SessionInfo& info, const std::string& folder,
                   const std::string& text) {
  if (text.empty() || text.size() > kMessageBytes) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "a message needs 1 to " + std::to_string(kMessageBytes) +
                           " bytes of text");
  }
  std::string error =
      "the session is not running; only this moderator's "
      "threads can be started again";
  session::Connection connection =
      OwnThread(info, PanelId(folder))
          ? session::Wake(info.cwd, info.path, error)
          : session::Connect(info.path);
  if (!connection.socket) {
    return Unavailable(error);
  }
  error = SendWhenReady(connection, info.path,
                        {{"kind", "steer"},
                         {"origin", kRoutePanel},
                         {"text", "[from the folder's moderator] " + text}},
                        false);
  return error.empty() ? ToolSuccess("sent") : Unavailable(error);
}

ToolResult Stop(const SessionInfo& info) {
  const auto error = SendToRunning(info.path, {{"kind", "interrupt"}});
  if (!error) return ToolSuccess("not running");
  return error->empty() ? ToolSuccess("interrupted") : Unavailable(*error);
}

ToolResult Close(const SessionInfo& info) {
  const std::string error = CloseRuntime(info.path);
  return error.empty() ? ToolSuccess("closed") : Unavailable(error);
}

// A running session is closed first. A thread's worktree goes with it, but
// only when nothing in it is lost.
ToolResult Delete(const SessionInfo& info, const std::string& folder) {
  if (const std::string error = CloseRuntime(info.path); !error.empty()) {
    return Unavailable(error);
  }
  if (LaunchWorktree(info.cwd)) {
    const std::string kept =
        RemoveWorktree(JsonValue(info.delegation, "folder", folder), info.cwd);
    if (!kept.empty()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments, kept);
    }
  }
  SessionStoreStatus removed = SessionStore::Remove(info.path);
  return removed.Ok() ? ToolSuccess("deleted") : Unavailable(removed.message);
}

// What a thread changed, from the host's git, never a model-run shell.
ToolResult Diff(const SessionInfo& info) {
  auto diff = HostGit(info.cwd, {"diff", "--no-ext-diff", "--no-textconv",
                                 "--stat", "--patch", "HEAD"});
  if (!diff.Ok()) {
    return ToolFailure(ToolErrorCode::kProcessFailed,
                       Utf8Prefix(diff.error, 1024));
  }
  return SessionData(info,
                     Trim(diff.output).empty() ? "(no changes)" : diff.output,
                     kDiffBytes);
}
}  // namespace

Tool ThreadTool(const std::string& folder,
                std::function<std::string()> own_model) {
  Tool tool = MakeTool(
      "thread",
      "Delegate work to threads: ordinary sessions that edit and run code "
      "for you. spawn starts one on a brief (title, objective, output, "
      "done_when, boundaries; environment worktree or local; step, the plan "
      "step it works for). message "
      "guides a running session in this folder, or restarts your own "
      "thread (session_id, text; three restarts in a row at most, then ask "
      "the user); stop interrupts its turn; close ends its runtime; diff shows "
      "what it changed; delete closes and removes a session, and its "
      "worktree if nothing would be lost, after the user confirms. One "
      "thread per independent part; keep dependent steps in one thread. "
      "A question that only needs reading goes to a subagent instead.",
      json::parse(R"json({"type":"object","properties":{
        "action":{"type":"string",
          "enum":["spawn","message","stop","close","diff","delete"]},
        "session_id":{"type":"string"},
        "title":{"type":"string"},
        "objective":{"type":"string"},
        "output":{"type":"string"},
        "done_when":{"type":"string"},
        "boundaries":{"type":"string"},
        "environment":{"type":"string","enum":["worktree","local"]},
        "model":{"type":"string"},
        "step":{"type":"string","maxLength":64},
        "text":{"type":"string"}},
        "required":["action"]})json"),
      [folder, own_model = std::move(own_model)](const json& a,
                                                 const ToolContext&) {
        const std::string action = JsonValue(a, "action", "");
        if (action == "spawn") return Spawn(folder, a, own_model);
        return WithSession(folder, a, [&](const SessionInfo& info) {
          if (action == "message") {
            return Message(info, folder, JsonValue(a, "text", ""));
          }
          if (action == "stop") return Stop(info);
          if (action == "close") return Close(info);
          if (action == "diff") return Diff(info);
          if (action == "delete") return Delete(info, folder);
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "unknown action " + action);
        });
      });
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.category = "collaborate";
  tool.intent = "delegate";
  // Deleting a conversation is the user's call, whatever the approval mode.
  tool.approval_class = [](const json& a) {
    return JsonValue(a, "action", "") == "delete"
               ? ApprovalClass::kMandatoryHuman
               : ApprovalClass::kNone;
  };
  tool.mandatory_reason = "deleting a session";
  tool.summary = [](const json& a) {
    return JsonValue(a, "action", "") + " " +
           JsonValue(a, "title", JsonValue(a, "session_id", ""));
  };
  tool.header = Verbs("Delegating", "Delegated");
  return tool;
}

namespace {
// Answers a thread's routed decision in its runtime. A denial carries the
// reason back as guidance, so the thread learns why and can adjust.
ToolResult Decide(const SessionInfo& info, const json& a) {
  const std::string action = JsonValue(a, "decision", "");
  const std::string interaction = JsonValue(a, "interaction_id", "");
  const std::string reason = Utf8Prefix(JsonValue(a, "reason", ""), 1024);
  json command;
  if (action == "yield") {
    command = {{"kind", "escalate"},
               {"text", reason.empty() ? "The moderator asks you." : reason}};
  } else if (action == "answer") {
    // The thread's question, answered in its ask tool's reply shape.
    const json* answers = JsonArray(a, "answers");
    if (!answers) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "answer needs answers, one per question");
    }
    command = {{"kind", "reply"}, {"text", JsonDump(*answers)}};
  } else if (action == "allow_once" || action == "allow_thread" ||
             action == "deny") {
    command = {
        {"kind", "reply"},
        {"text", action == "allow_once"     ? "y"
                 : action == "allow_thread" ? "s"
                 : reason.empty() ? "n"
                                  : "The moderator denied this: " + reason}};
  } else {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "decision is allow_once, allow_thread, deny, "
                       "answer or yield");
  }
  if (action != "yield") {
    command["origin"] = kRoutePanel;
    command["reason"] = reason;
  }
  command["interaction_id"] = interaction;
  const auto error = SendToRunning(info.path, std::move(command));
  if (!error) return Unavailable("that thread is not running");
  return error->empty() ? ToolSuccess("sent " + action) : Unavailable(*error);
}
}  // namespace

Tool DecideTool(const std::string& folder) {
  Tool tool = MakeTool(
      "decide",
      "Decide what a thread sent you. An approval request: allow_once, "
      "allow_thread (this and identical calls for the rest of the thread) "
      "or deny (the reason goes back to the thread). A question: answer, "
      "with answers holding one {choices, other} per question. Or yield to "
      "hand either to the user. Judge against the brief you gave, not the "
      "thread's own justification. Yield when it leaves the brief, touches "
      "shared or remote state, is the user's preference, or you are unsure.",
      json::parse(R"json({"type":"object","properties":{
        "session_id":{"type":"string"},
        "interaction_id":{"type":"string"},
        "decision":{"type":"string","enum":["allow_once","allow_thread","deny","answer","yield"]},
        "answers":{"type":"array","items":{"type":"object","properties":{
          "choices":{"type":"array","items":{"type":"string"}},
          "other":{"type":"string"}}}},
        "reason":{"type":"string"}},
        "required":["session_id","interaction_id","decision","reason"]})json"),
      [folder](const json& a, const ToolContext&) {
        return WithSession(
            folder, a, [&](const SessionInfo& info) { return Decide(info, a); },
            /*thread=*/true);
      });
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.category = "collaborate";
  tool.summary = [](const json& a) {
    return JsonValue(a, "decision", "") + " " + JsonValue(a, "session_id", "");
  };
  tool.header = Verbs("Deciding", "Decided");
  return tool;
}

}  // namespace uagent::panel
