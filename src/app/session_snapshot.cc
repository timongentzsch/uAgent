// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_view.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/app/session_host.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/project.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
namespace {
// Negative or unparsable offsets read from the start; callers pass paths, not
// positions, so a clamp is the whole policy.
size_t ClampedOffset(const std::string& text) {
  int64_t offset = 0;
  ParseInt64(text.c_str(), offset);
  return static_cast<size_t>(std::max(int64_t{0}, offset));
}

// Attachment display names travel in frames and land on disk-adjacent
// records: no path separators, no control bytes, bounded length.
// The instruction files a person edits, whose changes the web shows live.
std::vector<std::string> PromptPaths(const std::vector<std::string>& projects) {
  std::vector<std::string> paths;
  for (const std::string& project : projects) {
    for (bool coordinator : {false, true}) {
      for (bool in_project : {false, true}) {
        paths.push_back(
            InstructionPath(coordinator, in_project, project).string());
      }
    }
  }
  return paths;
}
}  // namespace
std::shared_ptr<const SessionHost::SavedHistory> SessionHost::ReadHistory(
    const std::string& path) {
  const FileStamp stamp = SnapshotFile(path);
  std::error_code error;
  const bool regular = std::filesystem::is_regular_file(
      std::filesystem::symlink_status(path, error));
  {
    std::lock_guard lock(history_mutex_);
    if (regular && history_ && history_->path == path &&
        history_->stamp == stamp) {
      return history_;
    }
  }
  auto history = std::make_shared<SavedHistory>();
  history->path = path;
  history->stamp = stamp;
  history->loaded = SessionStore::Inspect(path);
  auto& loaded = history->loaded;
  if (loaded.record && !std::move(loaded.record->state)
                            .RestoreConversation(history->conversation)) {
    loaded = {{SessionStoreError::kCorrupt, "invalid conversation metadata"},
              std::nullopt};
  }
  if (regular && loaded.record && SnapshotFile(path) == stamp) {
    std::lock_guard lock(history_mutex_);
    history_ = history;
  }
  return history;
}

std::vector<json> SessionHost::RefreshInvalidations(
    const std::vector<std::string>& projects) {
  std::vector<json> events;
  std::map<std::string, FileStamp> prompts;
  for (const std::string& path : PromptPaths(projects)) {
    prompts[path] = {};
  }
  for (auto& [path, stamp] : prompts) stamp = SnapshotFile(path);
  if (prompts != prompt_stamps_) {
    prompt_stamps_ = std::move(prompts);
    events.push_back({{"kind", "management.changed"}});
  }
  const FileStamp library = SnapshotFile(LibraryChangePath());
  if (library != library_stamp_) {
    library_stamp_ = library;
    events.push_back({{"kind", "management.changed"}});
  }
  if (RefreshScheduleCacheLocked()) {
    events.push_back(
        {{"kind", "scheduled.changed"}, {"scheduled", scheduled_view_}});
  }
  return events;
}

std::vector<std::string> SessionHost::InvalidationPaths(
    const std::vector<std::string>& projects) const {
  const std::string schedule = SchedulePath();
  // External schedule writers replace the file atomically. Watch its parent
  // too so a rename cannot strand the scheduler on the replaced inode.
  std::vector<std::string> paths{
      schedule, std::filesystem::path(schedule).parent_path().string(),
      LibraryChangePath()};
  for (const std::string& path : PromptPaths(projects)) {
    paths.push_back(path);
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::vector<std::string> SessionHost::PresencePaths() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> paths{UagentDir(kHistoryDir)};
  for (const auto& [id, session] : sessions_) {
    paths.push_back(
        std::filesystem::path(session->path).parent_path().string());
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

SnapshotResult SessionHost::Snapshot(const std::string& id,
                                     const SnapshotQuery& query) {
  std::shared_ptr<HostSession> session;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(id);
    if (found == sessions_.end()) return {{{"error", "unknown session"}}, 404};
    session = found->second;
    if (session->pid > 0 && !query.has_before && !query.has_detail &&
        !query.has_http) {
      return {LiveSnapshot(*session), 200};
    }
  }
  if (query.has_http) {
    json exchange;
    {
      std::lock_guard lock(mutex_);
      exchange = FindHttpExchange(session->state, query.http);
    }
    if (exchange.is_null()) {
      auto history = ReadHistory(session->path);
      const auto& loaded = history->loaded;
      if (loaded.record) {
        const json& display = loaded.record->state.display;
        exchange =
            query.http == "latest"
                ? FindHttpExchange(
                      JsonValue(JsonValue(display, "facts", json::object()),
                                "http-latest", json::object()),
                      query.http)
                : FindHttpExchange(display, query.http);
      }
    }
    if (exchange.is_null()) {
      return {{{"error", "HTTP exchange was not captured"}}, 404};
    }
    if (query.part != "request" && query.part != "response") {
      return {{{"error", "choose request or response"}}, 400};
    }
    json body = ReadPrivateArtifact(
        JsonValue(exchange, (query.part + "_path").c_str(), ""),
        ClampedOffset(query.offset));
    body["exchange"] = exchange;
    const int status = body.contains("error") ? 404 : 200;
    return {std::move(body), status};
  }
  if (query.raw && query.has_detail) {
    std::string path;
    json inline_exchange = nullptr;
    bool turn_active = false;
    {
      std::lock_guard lock(mutex_);
      turn_active = session->turn_active;
      auto active = session->active_exchanges.find(query.detail);
      if (active != session->active_exchanges.end()) {
        inline_exchange = active->second;
        turn_active = true;
      }
      const json* view = JsonObject(session->state, "view");
      const json* blocks = view ? JsonArray(*view, "blocks") : nullptr;
      if (blocks) {
        for (const json& block : *blocks) {
          if (JsonValue(block, "detail_id", "") == query.detail) {
            path = JsonValue(block, "exchange_path", "");
            break;
          }
          if (const json* tools = JsonArray(block, "tools")) {
            auto found = std::find_if(
                tools->begin(), tools->end(), [&](const json& tool) {
                  return JsonValue(tool, "detail_id", "") == query.detail;
                });
            if (found != tools->end()) {
              path = JsonValue(*found, "exchange_path", "");
              const json retained_request = {
                  {"name", JsonValue(*found, "name", "")},
                  {"arguments", JsonValue(*found, "arguments", "")}};
              if (!inline_exchange.is_null()) {
                inline_exchange["request"] = retained_request;
              } else if (path.empty()) {
                inline_exchange = {{"request", retained_request},
                                   {"response", ""},
                                   {"complete", false}};
              }
              if (!path.empty()) break;
            }
          }
        }
      }
    }
    const size_t start = ClampedOffset(query.offset);
    if (!path.empty()) {
      json body = ReadPrivateArtifact(path, start);
      const int status = body.contains("error") ? 404 : 200;
      return {std::move(body), status};
    }
    if (turn_active && !inline_exchange.is_null()) {
      std::string text = JsonDump(inline_exchange);
      return {{{"text", text.substr(std::min(text.size(), start))},
               {"next", text.size()},
               {"bytes", text.size()},
               {"more", false}},
              200};
    }
  }
  auto history = ReadHistory(session->path);
  const auto& loaded = history->loaded;
  json state = json::object();
  if (loaded.record) {
    const auto& record = *loaded.record;
    const auto& conversation = history->conversation;
    if (query.has_detail) {
      size_t offset = ClampedOffset(query.offset);
      json detail =
          query.raw ? history->view.Exchange(query.detail, offset)
          : query.artifact
              ? ReadPrivateArtifact(
                    JsonValue(JsonValue(conversation.DisplayFacts(),
                                        query.detail.c_str(), json::object()),
                              "artifact", ""),
                    offset)
              : history->view.Detail(query.detail, offset);
      const int status = detail.contains("error") ? 404 : 200;
      return {std::move(detail), status};
    }
    state = {{"view", history->view.Page(
                          static_cast<uint64_t>(ClampedOffset(query.before)))},
             {"usage", UsageJson(record.state.usage)},
             {"context_tokens", record.state.context_tokens},
             {"context_window", record.state.context_window},
             {"model", record.metadata.model},
             {"turns", record.metadata.turns},
             {"statistics", conversation.Statistics()},
             {"http", JsonValue(JsonValue(conversation.DisplayFacts(),
                                          "http-latest", json::object()),
                                "http", json::array())}};
  } else if (PathExists(session->path)) {
    return {{{"error", loaded.status.message}}, 422};
  }
  std::lock_guard lock(mutex_);
  auto owner = sessions_.find(session->id);
  if (owner != sessions_.end()) session = owner->second;
  if (session->pid > 0 && !query.has_before) {
    return {LiveSnapshot(*session), 200};
  }
  return {{{"v", kProtocol},
           {"epoch", epoch_},
           {"cursor", replay_.Cursor()},
           {"metadata", Metadata(*session)},
           {"state", std::move(state)},
           {"pending", session->pending},
           {"live", json::array()}},
          200};
}

}  // namespace uagent::session
