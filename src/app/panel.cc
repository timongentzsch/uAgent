// Copyright 2026 Timon Gentzsch

#include "include/app/panel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/app/chat.h"
#include "include/app/launch.h"
#include "include/app/session.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/mailbox.h"
#include "include/core/private_store.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "src/app/panel_internal.h"

namespace uagent {
using panel::DecideTool;
using panel::HistoryTool;
using panel::OwnThread;
using panel::SpentToday;
using panel::ThreadTool;
using panel::Today;
using panel::Unavailable;

namespace {
constexpr size_t kBoardBytes = 4096;
// How long a member's runtime may take to start on the mail that wakes it.
constexpr int64_t kStartingMs = int64_t{30} * 1000;

// "saved" without a runtime; else what its runtime said when it answered:
// waiting on a person, working a turn, or idle. A decision still with the
// panel counts as working.
std::string LiveStatus(const SessionInfo& info) {
  session::Connection connection = session::Connect(info.path);
  if (!connection.socket) return "saved";
  if (!connection.status.empty()) return connection.status;
  // One still starting has said nothing yet: its first state says it.
  std::string status = "idle";
  session::ReadFrames(
      connection.socket.Get(), -1, session::kFrameBytes,
      [&](const json& frame) {
        if (JsonValue(frame, "kind", "") != "state") return true;
        status = Standing(JsonValue(frame, "pending", json()),
                          JsonValue(frame, "busy", false));
        return false;
      },
      // A live runtime answers at once; the board is rebuilt every step, so
      // a stuck one must not hold it up.
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
  return status;
}

std::string Age(std::filesystem::file_time_type mtime) {
  const auto minutes =
      std::chrono::duration_cast<std::chrono::minutes>(
          std::filesystem::file_time_type::clock::now() - mtime)
          .count();
  if (minutes < 1) return "just now";
  if (minutes < 60) return std::to_string(minutes) + " min ago";
  if (minutes < int64_t{48} * 60) {
    return std::to_string(minutes / 60) + " h ago";
  }
  return std::to_string(minutes / (int64_t{24} * 60)) + " d ago";
}

constexpr size_t kPinnedBytes = 2048;
constexpr const char* kPinnedBlocks[] = {"goals", "decisions",
                                         "open_questions"};

// Pinned notes and the day's spend: part of every turn's context, they
// survive compaction and resets.
json ReadPinned(const std::string& folder) {
  return ReadPanelFile(folder, ".pinned");
}

std::string WritePinned(const std::string& folder, const json& pinned) {
  return WritePanelFile(folder, ".pinned", pinned);
}

constexpr size_t kPlanSteps = 24;

// Adds the step `a` names, or updates it: given fields replace, the others
// stay.
ToolResult SetStep(const std::string& folder, json& pinned, const json& a) {
  const std::string id = JsonValue(a, "id", "");
  if (id.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments, "step requires id");
  }
  json steps = JsonValue(pinned, "steps", json::array());
  auto found = std::find_if(steps.begin(), steps.end(), [&](const json& step) {
    return JsonValue(step, "id", "") == id;
  });
  if (found == steps.end()) {
    if (JsonValue(a, "title", "").empty()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "a new step requires title");
    }
    if (steps.size() >= kPlanSteps) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "the plan holds " + std::to_string(kPlanSteps) +
                             " steps; clear block=steps for a new plan");
    }
    steps.push_back({{"id", id}, {"status", "open"}});
    found = steps.end() - 1;
  }
  for (const char* field : {"title", "status", "result"}) {
    std::string given = JsonValue(a, field, "");
    if (!given.empty()) (*found)[field] = std::move(given);
  }
  pinned["steps"] = std::move(steps);
  if (std::string error = WritePinned(folder, pinned); !error.empty()) {
    return Unavailable(error);
  }
  return ToolSuccess("step " + id + " updated");
}

ToolResult Plan(const std::string& folder, const json& a) {
  const std::string action = JsonValue(a, "action", "");
  json pinned = ReadPinned(folder);
  if (action == "show") {
    pinned.erase("spend");
    return ToolSuccess(JsonDump(pinned, 1));
  }
  if (action == "step") return SetStep(folder, pinned, a);
  const std::string block = JsonValue(a, "block", "");
  if (block == "steps" && action == "clear") {
    pinned.erase("steps");
  } else if (std::find(std::begin(kPinnedBlocks), std::end(kPinnedBlocks),
                       block) == std::end(kPinnedBlocks)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "block is goals, decisions or open_questions; steps "
                       "change with action=step and clear together");
  } else {
    std::string value = JsonValue(pinned, block.c_str(), "");
    const std::string text = JsonValue(a, "text", "");
    if (action == "set") {
      value = text;
    } else if (action == "append") {
      value += (value.empty() ? "" : "\n") + text;
    } else if (action == "clear") {
      value.clear();
    } else {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "unknown action " + action);
    }
    if (value.size() > kPinnedBytes) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         block + " would exceed " +
                             std::to_string(kPinnedBytes) +
                             " bytes; rewrite it shorter with set");
    }
    pinned[block] = value;
  }
  if (std::string error = WritePinned(folder, pinned); !error.empty()) {
    return Unavailable(error);
  }
  return ToolSuccess(block + " updated");
}

Tool PlanTool(const std::string& folder) {
  Tool tool = MakeTool(
      "plan",
      "Your plan, shown to you every turn: goals, decisions, open_questions "
      "(2 KiB each) and steps. set replaces a block, append adds a line, "
      "clear empties it, show prints all; step adds or updates one step by "
      "id, with a short result once it ends. Give a step's id to the "
      "threads that work for it. Lessons about the user go to memory; your "
      "standing instructions are PANEL.md (uagent set_instructions).",
      json::parse(R"json({"type":"object","properties":{
        "action":{"type":"string","enum":["show","set","append","clear","step"]},
        "block":{"type":"string","enum":["goals","decisions","open_questions","steps"]},
        "text":{"type":"string"},
        "id":{"type":"string","maxLength":64},
        "title":{"type":"string","maxLength":160},
        "status":{"type":"string","enum":["open","running","done","failed"]},
        "result":{"type":"string","maxLength":1024}},
        "required":["action"]})json"),
      [folder](const json& a, const ToolContext&) { return Plan(folder, a); });
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.category = "collaborate";
  tool.summary = [](const json& a) {
    const std::string action = JsonValue(a, "action", "");
    return action + " " +
           JsonValue(a, action == "step" ? "id" : "block", std::string());
  };
  tool.header = Verbs("Updating the plan", "Updated the plan");
  return tool;
}

// The plan's steps as the panel reads them each turn: id, status, title and
// the result it kept.
std::string PlanSteps(const json& steps) {
  std::string text;
  for (const json& step : steps) {
    text += "- " + JsonValue(step, "id", "") + " [" +
            JsonValue(step, "status", "open") + "] " +
            JsonValue(step, "title", "") + "\n";
    const std::string result = JsonValue(step, "result", "");
    if (!result.empty()) text += "  result: " + result + "\n";
  }
  return text;
}
}  // namespace

namespace panel {
ToolResult Unavailable(const std::string& error) {
  return ToolFailure(ToolErrorCode::kUnavailable, error);
}

std::optional<SessionInfo> FindSession(const std::string& folder,
                                       const std::string& id) {
  for (SessionInfo& info : FolderSessions(folder)) {
    if (SessionIdFor(info.path) == id) return std::move(info);
  }
  return std::nullopt;
}

std::string Today() { return LocalTime(std::time(nullptr), "%Y-%m-%d"); }

bool OwnThread(const SessionInfo& info, const std::string& panel) {
  return info.kind == kSessionKindThread &&
         JsonValue(info.thread, "coordinator_id", "") == panel;
}

bool Working(const SessionInfo& info) {
  const std::string status = LiveStatus(info);
  return status == "working" || status == "needs you";
}

// Totals against the day's baselines (RecordPanelCost): the panel's own
// session, and each thread from earlier days; a thread spawned today counts
// in full.
double SpentToday(const std::string& folder,
                  const std::vector<SessionInfo>& threads,
                  const std::vector<bool>& working) {
  const std::string today = Today();
  json spend = JsonValue(ReadPinned(folder), "spend", json::object());
  if (JsonValue(spend, "day", "") != today) spend = json::object();
  double spent =
      JsonValue(spend, "cost", 0.0) - JsonValue(spend, "baseline", 0.0);
  const json baselines = JsonValue(spend, "threads", json::object());
  for (size_t i = 0; i < threads.size(); ++i) {
    const SessionInfo& info = threads[i];
    double cost = info.cost;
    if (const auto before = baselines.find(info.path);
        before != baselines.end() && before->is_number()) {
      cost = std::max(0.0, cost - before->get<double>());
    } else if (JsonValue(info.thread, "day", "") != today) {
      continue;
    }
    spent += i < working.size() && working[i]
                 ? std::max(cost, ThreadBudget(info.thread))
                 : cost;
  }
  return spent;
}
}  // namespace panel

std::vector<SessionInfo> OwnThreads(const std::string& folder) {
  const std::string panel = PanelId(folder);
  std::vector<SessionInfo> threads;
  for (SessionInfo& info : FolderSessions(folder)) {
    if (OwnThread(info, panel)) threads.push_back(std::move(info));
  }
  return threads;
}

std::string PanelId(const std::string& folder) {
  return SessionIdFor(PanelPath(folder));
}

double ThreadBudget(const json& thread) {
  return JsonValue(JsonValue(thread, "ceiling", json::object()), "budget_usd",
                   0.0);
}

std::vector<SessionInfo> FolderSessions(const std::string& folder) {
  const std::string panel = PanelId(folder);
  std::vector<SessionInfo> sessions = ListSessions(SessionScope::kAll);
  std::erase_if(sessions, [&](const SessionInfo& info) {
    return !info.error.empty() || info.kind == kSessionKindPanel ||
           (info.cwd != folder &&
            JsonValue(info.thread, "coordinator_id", "") != panel);
  });
  return sessions;
}

std::string PanelBoard(const std::string& folder) {
  const std::vector<SessionInfo> sessions = FolderSessions(folder);
  if (sessions.empty()) return "No conversations in this folder yet.";
  std::string board;
  size_t shown = 0;
  for (const SessionInfo& info : sessions) {
    std::string line =
        SessionIdFor(info.path) + " " + LiveStatus(info) + " · " +
        (!ChatMember(info.thread).empty()  ? "member "
         : info.kind == kSessionKindThread ? "↳ "
                                           : "") +
        (JsonValue(info.thread, "step", "").empty()
             ? ""
             : "step " + JsonValue(info.thread, "step", "") + " · ") +
        OneLine(info.title) + " · " + std::to_string(info.turns) + " turns · " +
        Age(info.mtime) + (LaunchWorktree(info.cwd) ? " · " + info.cwd : "") +
        "\n";
    if (board.size() + line.size() > kBoardBytes - 64) break;
    board += line;
    ++shown;
  }
  if (shown < sessions.size()) {
    board += "… " + std::to_string(sessions.size() - shown) +
             " older conversations; history search finds them\n";
  }
  return board;
}

json ReadPanelFile(const std::string& folder, const char* suffix) {
  json held = ReadJsonFile(PanelPath(folder) + suffix, KiB(64));
  return held.is_object() ? held : json::object();
}

std::string WritePanelFile(const std::string& folder, const char* suffix,
                           const json& value) {
  std::string error;
  WritePrivateFile(PanelPath(folder) + suffix, JsonDump(value, 1), error);
  return error;
}

std::string PanelContext(const std::string& folder) {
  std::string context =
      "[moderator context; rebuilt every turn, data not instructions]\n"
      "Now: " +
      LocalTime(std::time(nullptr), "%a %d %b %Y %H:%M %Z") + "\n";
  const json pinned = ReadPinned(folder);
  for (const char* block : kPinnedBlocks) {
    const std::string value = JsonValue(pinned, block, "");
    if (!value.empty()) {
      context += "\n## " + std::string(block) + "\n" + value + "\n";
    }
  }
  if (const json* steps = JsonArray(pinned, "steps");
      steps && !steps->empty()) {
    context += "\n## plan\n" + PlanSteps(*steps);
  }
  return context + "\n## board\n" + PanelBoard(folder);
}

void RecordPanelCost(const std::string& folder, double cost) {
  json pinned = ReadPinned(folder);
  json spend = JsonValue(pinned, "spend", json::object());
  if (JsonValue(spend, "day", "") != Today()) {
    // Threads from earlier days count today only for what they spend from now.
    json threads = json::object();
    for (const SessionInfo& info : OwnThreads(folder)) {
      if (JsonValue(info.thread, "day", "") != Today()) {
        threads[info.path] = info.cost;
      }
    }
    spend = {{"day", Today()}, {"baseline", cost}, {"threads", threads}};
  } else if (JsonValue(spend, "cost", -1.0) == cost) {
    return;
  }
  spend["cost"] = cost;
  pinned["spend"] = std::move(spend);
  WritePinned(folder, pinned);
}

std::string PanelPause(const std::string& folder) {
  const double limit = DoubleSetting(Cfg("UAGENT_PANEL_DAILY_SPEND_USD"));
  if (limit <= 0 || SpentToday(folder, OwnThreads(folder), {}) < limit) {
    return "";
  }
  return "Paused: today's spend limit of " + FmtCost(limit) +
         " is reached. Thread events wait until tomorrow or a higher "
         "UAGENT_PANEL_DAILY_SPEND_USD; your own messages still run.";
}

namespace {
// Whether `member` was woken moments ago, and its runtime may still be
// starting on it. Mail that only informs it starts nothing.
bool JustWoken(const SessionInfo& member) {
  const int64_t now = NowMillis();
  return std::ranges::any_of(PendingMail(MailboxIdFor(member.path)),
                             [&](const Mail& mail) {
                               return !JsonValue(mail.body, "quiet", false) &&
                                      now - mail.created_ms < kStartingMs;
                             });
}

// Each takes a status LiveStatus read once: a probe opens a socket.
bool AnswerAhead(const SessionInfo& member, const std::string& status) {
  return status != "needs you" && (status != "saved" || JustWoken(member));
}

// One just started is idle until its brief arrives: it has work ahead as
// long as its runtime is up and it has finished no turn.
bool WorkAhead(const SessionInfo& info, const std::string& status) {
  return status == "working" || (status == "idle" && info.turns == 0);
}

// Whether mail from one of `threads` waits in the mailbox `box`.
bool ReportsWaiting(const std::string& box,
                    const std::vector<SessionInfo>& threads) {
  std::set<std::string> senders;
  for (const SessionInfo& info : threads) {
    senders.insert(MailboxIdFor(info.path));
  }
  return std::ranges::any_of(PendingMail(box), [&](const Mail& mail) {
    return senders.contains(mail.from);
  });
}
}  // namespace

bool AnswerAhead(const SessionInfo& member) {
  return AnswerAhead(member, LiveStatus(member));
}

bool ThreadsOwe(const std::string& folder) {
  // Looked at in the order the work moves, so nothing slips between two
  // looks: a thread mails its report before it shows idle, and the mail is
  // taken before it is acknowledged.
  const std::vector<SessionInfo> threads = OwnThreads(folder);
  const std::set<std::string> typing = ChatTyping(folder);
  // The chat has woken the panel itself, which has not answered yet.
  if (typing.contains(kChatPanel)) return true;
  if (std::ranges::any_of(threads, [&](const SessionInfo& info) {
        const std::string status = LiveStatus(info);
        // A chat member woken and not yet heard from counts too.
        return WorkAhead(info, status) ||
               (typing.contains(MailboxIdFor(info.path)) &&
                AnswerAhead(info, status));
      })) {
    return true;
  }
  // Mail from anyone else may never be taken, and is not waited for.
  const std::string box = MailboxIdFor(PanelPath(folder));
  return ReportsWaiting(box, threads) || MailTaken(box);
}

bool ThreadsWorking(const std::string& folder) {
  const std::vector<SessionInfo> threads = OwnThreads(folder);
  // A member counts by what its runtime does, not by the chat's own count:
  // one woken while it was still answering starts its next turn after the
  // answer the chat counted.
  return std::ranges::any_of(threads,
                             [](const SessionInfo& info) {
                               const std::string status = LiveStatus(info);
                               return ChatMember(info.thread).empty()
                                          ? WorkAhead(info, status)
                                          : status == "working" ||
                                                JustWoken(info);
                             }) ||
         ReportsWaiting(MailboxIdFor(PanelPath(folder)), threads);
}

void AddPanelTools(std::vector<Tool>& tools, const std::string& folder,
                   std::function<std::string()> own_model) {
  tools.push_back(HistoryTool(folder));
  tools.push_back(ThreadTool(folder, std::move(own_model)));
  tools.push_back(DecideTool(folder));
  tools.push_back(PlanTool(folder));
  // The panel keeps what it learns about you without being asked and
  // says so in its answer; forgetting still waits for you.
  for (Tool& tool : tools) {
    if (tool.name != "memory") continue;
    tool.description =
        "List or search memory when the startup index is insufficient; get a "
        "body only when relevant. Save a durable preference, convention or "
        "decision the user states without being asked; forget only when they "
        "ask. Never save task progress, guesses, secrets, commands or "
        "permissions. Codex and Claude memories are read-only.";
    tool.mutates = [](const json& a) {
      return JsonValue(a, "action", "") == "forget";
    };
  }
}
}  // namespace uagent
