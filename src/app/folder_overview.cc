// Copyright 2026 Timon Gentzsch

#include "include/app/folder_overview.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/app/launch.h"
#include "include/core/debug.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/thread_annotations.h"

namespace uagent {
namespace {
constexpr auto kFresh = std::chrono::seconds(10);
constexpr std::time_t kWeek = 7 * 24 * 60 * 60;

std::time_t StampTime(const std::string& stamp) {
  std::tm parsed{};
  if (!strptime(stamp.c_str(), "%Y-%m-%dT%H:%M:%SZ", &parsed)) return 0;
  return timegm(&parsed);
}

std::string Day(std::time_t at) { return LocalTime(at, "%Y-%m-%d"); }

// The folder's conversations, its own and the agents saved beside them.
std::vector<SessionInfo> Conversations(const std::string& folder) {
  return SessionsBeside(HistoryPath(folder, "overview"));
}

// Spend and changes from the turn summaries of the conversations saved in
// the last week.
void AddTurns(const std::vector<SessionInfo>& sessions, json& facts) {
  const std::time_t now = std::time(nullptr);
  const std::string today = Day(now);
  double spent_today = 0, spent_week = 0;
  int64_t added = 0, removed = 0;
  std::set<std::string> files;
  for (const SessionInfo& info : sessions) {
    if (std::filesystem::file_time_type::clock::now() - info.mtime >
        std::chrono::seconds(kWeek)) {
      continue;
    }
    SessionLoadResult loaded = SessionStore::Inspect(info.path);
    if (!loaded.record) continue;
    const json* display = JsonObject(loaded.record->state.display, "facts");
    if (!display) continue;
    for (const auto& [key, entry] : display->items()) {
      if (JsonValue(entry, "kind", "") != "turn_summary") continue;
      const std::time_t at = StampTime(JsonValue(entry, "time", ""));
      if (now - at > kWeek) continue;
      const json summary = JsonValue(entry, "summary", json::object());
      const double cost =
          JsonValue(JsonValue(summary, "usage", json::object()), "cost", 0.0);
      spent_week += cost;
      if (Day(at) != today) continue;
      spent_today += cost;
      for (const json& file : JsonValue(summary, "files", json::array())) {
        files.insert(JsonValue(file, "path", ""));
        added += JsonValue(file, "added", int64_t{0});
        removed += JsonValue(file, "removed", int64_t{0});
      }
    }
  }
  facts["spend"] = {{"today", spent_today}, {"week", spent_week}};
  facts["changed"] = {
      {"files", files.size()}, {"added", added}, {"removed", removed}};
}

// The branch and how many files differ from it, where the folder is a
// repository: "## main...origin/main" heads the porcelain status.
void AddGit(const std::string& folder, json& facts) {
  const CapturedProcess status =
      HostGit(folder, {"status", "--porcelain", "--branch"});
  const std::string& out = status.output;
  if (!status.Ok() || !out.starts_with("## ")) return;
  std::string head = out.substr(3, out.find('\n') - 3);
  // One without commits yet: "## No commits yet on main".
  constexpr std::string_view kUnborn = "No commits yet on ";
  if (head.starts_with(kUnborn)) head.erase(0, kUnborn.size());
  facts["git"] = {
      {"branch", head.substr(0, head.find("..."))},
      {"uncommitted", std::max<int64_t>(0, std::ranges::count(out, '\n') - 1)}};
}
}  // namespace

json FolderFacts(const std::string& folder) {
  struct Held {
    std::chrono::steady_clock::time_point at;
    json facts;
  };
  static std::mutex mutex;
  static std::map<std::string, Held> held UAGENT_GUARDED_BY(mutex);
  {
    std::lock_guard lock(mutex);
    const auto found = held.find(folder);
    if (found != held.end() &&
        std::chrono::steady_clock::now() - found->second.at < kFresh) {
      return found->second.facts;
    }
  }
  json facts = json::object();
  AddTurns(Conversations(folder), facts);
  AddGit(folder, facts);
  std::lock_guard lock(mutex);
  held[folder] = {std::chrono::steady_clock::now(), facts};
  return facts;
}

std::string FolderOverviewText(const std::string& folder) {
  const json facts = FolderFacts(folder);
  std::string text = folder;
  if (const json* git = JsonObject(facts, "git")) {
    text +=
        " · " + JsonValue(*git, "branch", "") + " · " +
        Plural(JsonValue(*git, "uncommitted", int64_t{0}), "uncommitted file");
  }
  const std::vector<SessionInfo> sessions = Conversations(folder);
  int64_t working = 0;
  std::string waiting;
  for (const SessionInfo& info : sessions) {
    const std::string status = RuntimeStatus(info.path);
    if (status == "working") ++working;
    if (status == "needs you") {
      waiting +=
          "  " + OneLine(info.title) + " (" + SessionIdFor(info.path) + ")\n";
    }
  }
  const json spend = JsonValue(facts, "spend", json::object());
  const json changed = JsonValue(facts, "changed", json::object());
  text +=
      "\n" +
      JoinDot({Plural(static_cast<int64_t>(sessions.size()), "conversation"),
               std::to_string(working) + " working"}) +
      "\nSpent " + FmtCost(JsonValue(spend, "today", 0.0)) + " today · " +
      FmtCost(JsonValue(spend, "week", 0.0)) + " in 7 days · changed " +
      Plural(JsonValue(changed, "files", int64_t{0}), "file") + " today (+" +
      std::to_string(JsonValue(changed, "added", int64_t{0})) + " −" +
      std::to_string(JsonValue(changed, "removed", int64_t{0})) + ")\n";
  return text +
         (waiting.empty() ? "Nothing needs you.\n" : "Needs you:\n" + waiting);
}
}  // namespace uagent
