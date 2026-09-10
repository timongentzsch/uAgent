// Copyright 2026 Timon Gentzsch
#include "include/app/schedule.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>
#include <string>

#include "include/app/control.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/signals.h"

namespace uagent {
namespace {
constexpr size_t kStoreBytes = size_t{4} * 1024 * 1024;
constexpr size_t kTasks = 64, kRuns = 128;
int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
json Empty() {
  return {{"v", 1},
          {"revision", ""},
          {"tasks", json::array()},
          {"runs", json::array()}};
}
json Public(json store) {
  if (store.contains("runs")) {
    for (auto& run : store["runs"]) {
      run.erase("definition");
      run["session_available"] = PathExists(JsonValue(run, "session_path", ""));
    }
  }
  return store;
}
json Mutate(const std::function<json(json&)>& change) {
  std::string error;
  FileLease lease;
  if (!lease.Acquire(UagentDir("scheduled") + "/write.lock", error)) {
    return {{"error", error}};
  }
  auto store = ReadSchedules();
  if (store.contains("error")) return store;
  auto before = store;
  json result = change(store);
  if (result.contains("error") || store == before) return result;
  store["revision"] = HashHex(MakeSessionId());
  while (store["runs"].size() > kRuns) {
    auto old = std::find_if(
        store["runs"].begin(), store["runs"].end(), [](const json& run) {
          return !ScheduledRunActive(JsonValue(run, "status", ""));
        });
    if (old == store["runs"].end()) {
      return {{"error", "scheduled run limit reached"}};
    }
    store["runs"].erase(old);
  }
  auto bytes = JsonDump(store);
  if (bytes.size() > kStoreBytes) {
    return {{"error", "scheduled task storage is full"}};
  }
  if (!AtomicWriteFile(SchedulePath(), bytes, kPrivateFileMode, false, error)) {
    return {{"error", error}};
  }
  return result;
}
json Run(const json& task, int64_t at, const std::string& status) {
  auto id = HashHex(MakeSessionId());
  const auto project = JsonValue(task, "cwd", "");
  const auto cwd = JsonValue(task, "environment", "local") == "worktree"
                       ? UagentDir("worktrees") + "/" + id
                       : project;
  const auto folder = UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd);
  const auto path = folder + "/scheduled-" + id + ".json";
  return {{"id", id},
          {"task_id", task["id"]},
          {"task_revision", task["revision"]},
          {"title", task["name"]},
          {"scheduled_for", at},
          {"updated", Now()},
          {"status", status},
          {"cwd", cwd},
          {"project", project},
          {"session_path", path},
          {"session_id", HashHex(path)},
          {"definition", task}};
}
bool Busy(const json& store, const std::string& task) {
  return std::any_of(store["runs"].begin(), store["runs"].end(),
                     [&](const json& run) {
                       return JsonValue(run, "task_id", "") == task &&
                              ScheduledRunActive(JsonValue(run, "status", ""));
                     });
}
}  // namespace
std::string SchedulePath() { return UagentDir("scheduled") + "/state.json"; }
bool ScheduledRunActive(const std::string& state) {
  return state == "queued" || state == "starting" || state == "running" ||
         state == "waiting" || state == "stopping";
}
json ReadSchedules() {
  const auto path = SchedulePath();
  if (!PathExists(path)) return Empty();
  std::string bytes, error;
  if (!ReadRegularFile(path, kStoreBytes, bytes, error)) {
    return {{"error", error}};
  }
  auto store = json::parse(bytes, nullptr, false);
  if (!store.is_object() || JsonValue(store, "v", 0) != 1 ||
      !JsonArray(store, "tasks") || !JsonArray(store, "runs") ||
      store["tasks"].size() > kTasks || store["runs"].size() > kRuns) {
    return {
        {"error", "scheduled task store is invalid; it has been preserved"}};
  }
  std::set<std::string> tasks, runs;
  for (const auto& task : store["tasks"]) {
    if (JsonValue(task, "id", "").empty() ||
        !tasks.insert(JsonValue(task, "id", "")).second ||
        JsonValue(task, "revision", "").empty() ||
        JsonValue(task, "name", "").empty() ||
        JsonValue(task, "cwd", "").empty() ||
        JsonValue(task, "prompt", "").empty() ||
        !JsonObject(task, "schedule")) {
      return {{"error",
               "scheduled task store contains an invalid task; it has been "
               "preserved"}};
    }
  }
  for (const auto& run : store["runs"]) {
    if (JsonValue(run, "id", "").empty() ||
        !runs.insert(JsonValue(run, "id", "")).second ||
        JsonValue(run, "task_id", "").empty() ||
        JsonValue(run, "status", "").empty() ||
        JsonValue(run, "session_path", "").empty() ||
        JsonValue(run, "cwd", "").empty() || !JsonObject(run, "definition")) {
      return {{"error",
               "scheduled task store contains an invalid run; it has been "
               "preserved"}};
    }
  }
  return store;
}
json ScheduleCalendar(const json& request) {
  const auto schedule = JsonValue(request, "schedule", json::object());
  const auto type = JsonValue(schedule, "type", "");
  int64_t after = JsonValue(request, "after", Now());
  if (after < 0 || after > 4102444800LL) {
    return {{"error", "schedule date is outside 1970–2100"}};
  }
  json times = json::array();
  if (type == "once") {
    int64_t at = JsonValue(schedule, "at", int64_t{0});
    if (at > after && at < 4102444800LL) times.push_back(at);
  } else if (type == "interval") {
    int64_t interval = JsonValue(schedule, "seconds", int64_t{0});
    int64_t start = JsonValue(schedule, "start", int64_t{0});
    if (interval < 60 || interval > 31536000 || start < 0 ||
        start > 4102444800LL) {
      return {{"error", "interval must be between one minute and one year"}};
    }
    int64_t next = start > after
                       ? start
                       : start + ((after - start) / interval + 1) * interval;
    for (int i = 0; i < 3; ++i) times.push_back(next + i * interval);
  } else if (type == "weekly") {
    const auto zone = JsonValue(schedule, "timezone", "");
    if (zone.empty() || zone.size() > 100 || zone.front() == '/' ||
        zone.find("..") != std::string::npos ||
        !std::all_of(zone.begin(), zone.end(),
                     [](unsigned char c) {
                       return std::isalnum(c) || c == '/' || c == '_' ||
                              c == '-' || c == '+';
                     }) ||
        !PathExists("/usr/share/zoneinfo/" + zone)) {
      return {{"error",
               "choose an installed IANA timezone, for example Europe/Zurich"}};
    }
    const auto time = JsonValue(schedule, "time", "");
    int64_t hour = 0, minute = 0;
    if (time.size() != 5 || time[2] != ':' ||
        !ParseInt64(time.substr(0, 2).c_str(), hour) ||
        !ParseInt64(time.substr(3).c_str(), minute) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59) {
      return {{"error", "time must be HH:MM"}};
    }
    std::set<int> days;
    if (const json* selected = JsonArray(schedule, "days")) {
      for (const auto& day : *selected) {
        if (!day.is_number_integer()) {
          return {{"error", "weekdays must be integers 0–6"}};
        }
        auto value = day.get<int64_t>();
        if (value < 0 || value > 6) {
          return {{"error", "weekdays must be integers 0–6"}};
        }
        days.insert(static_cast<int>(value));
      }
    }
    if (days.empty()) return {{"error", "choose at least one weekday"}};
    setenv("TZ", zone.c_str(), 1);
    tzset();
    time_t instant = static_cast<time_t>(after);
    tm today{};
    if (!localtime_r(&instant, &today)) {
      return {{"error", "cannot resolve local time"}};
    }
    for (int offset = 0; offset < 29 && times.size() < 3; ++offset) {
      tm date = today;
      date.tm_hour = 12;
      date.tm_min = 0;
      date.tm_sec = 0;
      date.tm_mday += offset;
      date.tm_isdst = -1;
      time_t noon = mktime(&date);
      if (noon < 0 || !days.contains(date.tm_wday)) continue;
      // Gaps are skipped; repeated local times run at their first occurrence.
      time_t candidate = -1;
      for (int dst : {0, 1}) {
        tm local = date;
        local.tm_hour = static_cast<int>(hour);
        local.tm_min = static_cast<int>(minute);
        local.tm_sec = 0;
        local.tm_isdst = dst;
        time_t at = mktime(&local);
        tm actual{};
        if (at >= 0 && localtime_r(&at, &actual) &&
            actual.tm_year == date.tm_year && actual.tm_yday == date.tm_yday &&
            actual.tm_hour == hour && actual.tm_min == minute &&
            (candidate < 0 || at < candidate)) {
          candidate = at;
        }
      }
      if (candidate > after) times.push_back(static_cast<int64_t>(candidate));
    }
  } else {
    return {{"error", "choose once, interval or weekly"}};
  }
  return {{"times", times}};
}
json ScheduleTimes(const json& schedule, int64_t after) {
  json request = {
      {"kind", "calendar"}, {"schedule", schedule}, {"after", after}};
  return JsonValue(schedule, "type", "") == "weekly"
             ? ControlProcess(request)
             : ScheduleCalendar(request);
}
json ScheduleControl(const json& request) {
  const auto action = JsonValue(request, "action", "list");
  if (action == "list") {
    auto result = Public(ReadSchedules());
    result["runner_active"] =
        FileLease::HasLiveOwner(UagentDir("web") + "/master.lock");
    return result;
  }
  if (action == "preview") {
    return ScheduleTimes(JsonValue(request, "schedule", json::object()),
                         JsonValue(request, "after", Now()));
  }
  if (action == "get") {
    auto store = Public(ReadSchedules());
    if (store.contains("error")) return store;
    for (const auto& task : store["tasks"]) {
      if (JsonValue(task, "id", "") == JsonValue(request, "key", "")) {
        return {{"item", task}};
      }
    }
    return {{"error", "scheduled task not found"}};
  }
  return Mutate([&](json& store) -> json {
    const auto id = JsonValue(request, "key", "");
    auto found = std::find_if(
        store["tasks"].begin(), store["tasks"].end(),
        [&](const json& task) { return JsonValue(task, "id", "") == id; });
    if (action == "stop") {
      for (auto& run : store["runs"]) {
        if (JsonValue(run, "id", "") == id &&
            ScheduledRunActive(JsonValue(run, "status", ""))) {
          run["status"] = JsonValue(run, "status", "") == "queued"
                              ? "interrupted"
                              : "stopping";
          run["updated"] = Now();
          return {{"stopped", id}};
        }
      }
      return {{"error", "active run not found"}};
    }
    if (action == "save") {
      json task = JsonValue(request, "task", json::object());
      const std::string name = Trim(JsonValue(task, "name", ""));
      const std::string prompt = Trim(JsonValue(task, "prompt", ""));
      const auto environment = JsonValue(task, "environment", "worktree");
      const auto permissions = JsonValue(task, "permissions", "prompt");
      if (name.empty() || name.size() > 256 || prompt.empty() ||
          prompt.size() > 8192 ||
          (environment != "local" && environment != "worktree") ||
          (permissions != "prompt" && permissions != "yolo")) {
        return {{"error",
                 "provide a name, instructions, execution environment and "
                 "permissions"}};
      }
      std::error_code ec;
      auto cwd = std::filesystem::canonical(JsonValue(task, "cwd", ""), ec);
      if (ec || !std::filesystem::is_directory(cwd, ec)) {
        return {{"error", "project is unavailable"}};
      }
      if (found == store["tasks"].end() &&
          (!id.empty() || store["tasks"].size() >= kTasks)) {
        return {{"error", "task not found or task limit reached"}};
      }
      if (!request.contains("revision") ||
          JsonValue(request, "revision", "") !=
              (found == store["tasks"].end()
                   ? ""
                   : JsonValue(*found, "revision", ""))) {
        return {{"error", "This task changed. Reload it before saving."},
                {"conflict", true}};
      }
      auto schedule = JsonValue(task, "schedule", json::object());
      if (JsonValue(schedule, "type", "") == "interval" &&
          !schedule.contains("start")) {
        schedule["start"] = Now();
      }
      auto preview = ScheduleTimes(schedule, Now());
      if (preview.contains("error")) return preview;
      if (preview["times"].empty()) {
        return {{"error", "choose a future run time"}};
      }
      json saved = {{"id", id.empty() ? HashHex(MakeSessionId()) : id},
                    {"revision", HashHex(MakeSessionId())},
                    {"name", name},
                    {"prompt", prompt},
                    {"cwd", cwd.string()},
                    {"environment", environment},
                    {"permissions", permissions},
                    {"model", JsonValue(task, "model", "")},
                    {"schedule", schedule},
                    {"enabled", JsonValue(task, "enabled", true)},
                    {"next", preview["times"][0]},
                    {"upcoming", preview["times"]}};
      if (found == store["tasks"].end()) {
        store["tasks"].push_back(saved);
      } else {
        *found = saved;
      }
      return {{"item", saved}};
    }
    if (found == store["tasks"].end()) {
      return {{"error", "scheduled task not found"}};
    }
    if (action == "run") {
      if (!FileLease::HasLiveOwner(UagentDir("web") + "/master.lock")) {
        return {{"error", "Start uagent --web to run scheduled tasks."}};
      }
      if (Busy(store, id)) {
        return {{"error", "this task already has an active run"}};
      }
      auto run = Run(*found, Now(), "queued");
      store["runs"].push_back(run);
      return {{"run", Public({{"runs", json::array({run})}})["runs"][0]}};
    }
    if (action == "pause" || action == "resume" || action == "forget") {
      if (JsonValue(request, "revision", "") !=
          JsonValue(*found, "revision", "")) {
        return {{"error", "This task changed. Reload it before changing it."},
                {"conflict", true}};
      }
      if (action != "resume") {
        for (auto& run : store["runs"]) {
          if (JsonValue(run, "task_id", "") == id &&
              JsonValue(run, "status", "") == "queued") {
            run["status"] = "interrupted";
            run["updated"] = Now();
          }
        }
      }
    }
    if (action == "pause" || action == "resume") {
      (*found)["enabled"] = action == "resume";
      if (action == "resume") {
        auto preview = ScheduleTimes((*found)["schedule"], Now());
        if (preview.contains("error") || preview["times"].empty()) {
          return {{"error", "edit the task to choose a future run time"}};
        }
        (*found)["next"] = preview["times"][0];
        (*found)["upcoming"] = preview["times"];
      }
      (*found)["revision"] = HashHex(MakeSessionId());
      return {{"item", *found}};
    }
    if (action == "forget") {
      store["tasks"].erase(found);
      return {{"deleted", id}};
    }
    return {{"error", "unknown schedule action"}};
  });
}
json ClaimScheduledRuns(int64_t now, size_t slots) {
  return Mutate([&](json& store) -> json {
    for (auto& task : store["tasks"]) {
      int64_t due = JsonValue(task, "next", int64_t{0});
      if (!JsonValue(task, "enabled", false) || due == 0 || due > now) continue;
      auto preview = ScheduleTimes(task["schedule"], now);
      if (preview.contains("error")) {
        task["enabled"] = false;
        task["error"] = preview["error"];
        continue;
      }
      task["next"] = preview["times"].empty() ? json(0) : preview["times"][0];
      task["upcoming"] = preview["times"];
      if (preview["times"].empty()) task["enabled"] = false;
      const std::string status = now - due > 60            ? "missed"
                                 : Busy(store, task["id"]) ? "skipped"
                                                           : "queued";
      store["runs"].push_back(Run(task, due, status));
    }
    json claimed = json::array();
    for (auto& run : store["runs"]) {
      if (claimed.size() >= slots) break;
      if (JsonValue(run, "status", "") != "queued") continue;
      run["status"] = "starting";
      run["updated"] = now;
      claimed.push_back(run);
    }
    return {{"runs", claimed}};
  });
}
json UpdateScheduledRun(const std::string& id, const std::string& status,
                        const std::string& error) {
  return Mutate([&](json& store) -> json {
    for (auto& run : store["runs"]) {
      if (JsonValue(run, "id", "") == id &&
          (JsonValue(run, "status", "") != status ||
           JsonValue(run, "error", "") != error)) {
        auto prior = JsonValue(run, "status", "");
        if (!ScheduledRunActive(prior) ||
            (prior == "stopping" && ScheduledRunActive(status))) {
          continue;
        }
        run["status"] = status;
        run["updated"] = Now();
        if (!error.empty()) run["error"] = error;
      }
    }
    return json::object();
  });
}
json RecoverScheduledRuns() {
  return Mutate([](json& store) -> json {
    for (auto& run : store["runs"]) {
      auto state = JsonValue(run, "status", "");
      if (ScheduledRunActive(state) && state != "queued") {
        run["status"] = "interrupted";
        run["error"] = "Host restarted. Inspect this run before retrying.";
        run["updated"] = Now();
      }
    }
    return json::object();
  });
}
}  // namespace uagent
