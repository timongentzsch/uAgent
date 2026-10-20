// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

#include "include/agent/session_role.h"
#include "include/agent/session_view.h"
#include "include/app/chat.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/launch.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/app/session_command.h"
#include "include/app/session_host.h"
#include "include/cli/cli.h"
#include "include/core/capture.h"
#include "include/core/effective_config.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::session {
namespace {
namespace fs = std::filesystem;

// Where a workspace keeps the name it goes by; its folder's own name is only
// what it was first called, made safe for a path.
fs::path LabelFile(const fs::path& space) {
  return space / kWorkspaceDir / "name";
}

std::string Label(const fs::path& space) {
  const std::string label =
      Trim(ReadFile(LabelFile(space).string(), 256).value_or(""));
  return label.empty() ? space.filename().string() : label;
}

std::string InvalidSpaceNameMessage() {
  return "a workspace's name is 1 to " + std::to_string(kSpaceNameChars) +
         " characters without /";
}

bool ValidSpaceName(const std::string& name) {
  return ValidSessionTitle(name) && name.size() <= kSpaceNameChars &&
         name.find('/') == std::string::npos;
}

// `path` as a workspace's folder, or nothing when it is not one. The folder
// that holds the workspaces is µAgent's own: one that is a link leads to
// directories that are not workspaces, and holds none.
std::optional<fs::path> SpaceAt(const std::string& path) {
  std::error_code error;
  if (fs::is_symlink(UagentDir(kSpacesDir), error)) return std::nullopt;
  const auto root = CanonicalDirectory(UagentDir(kSpacesDir));
  const auto space = CanonicalDirectory(path);
  return root && space && space->parent_path() == *root ? space : std::nullopt;
}
}  // namespace

json Spaces() {
  json spaces = json::array();
  std::error_code error;
  for (const auto& entry :
       fs::directory_iterator(UagentDir(kSpacesDir), error)) {
    if (const auto space = SpaceAt(entry.path().string())) {
      spaces.push_back({{"path", space->string()}, {"name", Label(*space)}});
    }
  }
  std::sort(spaces.begin(), spaces.end(), [](const json& a, const json& b) {
    return AsciiLower(JsonValue(a, "name", "")) <
           AsciiLower(JsonValue(b, "name", ""));
  });
  return spaces;
}

std::string SpaceFolder(const std::string& name, std::string& error) {
  if (!ValidSpaceName(name)) {
    error = InvalidSpaceNameMessage();
    return "";
  }
  for (const json& space : Spaces()) {
    if (AsciiLower(JsonValue(space, "name", "")) == AsciiLower(name)) {
      return JsonValue(space, "path", "");
    }
  }
  // A folder of its own: the name made safe, numbered when that is taken.
  const std::string safe = SafeFileComponent(name);
  std::string folder = UagentDir(kSpacesDir) + "/" + safe;
  for (int n = 2; PathExists(folder); ++n) {
    folder = UagentDir(kSpacesDir) + "/" + safe + "-" + std::to_string(n);
  }
  std::string ignored;
  if (!EnsurePrivateDirectory(folder + "/" + kWorkspaceDir) ||
      (safe != name &&
       !AtomicWriteFile(LabelFile(folder).string(), name + "\n",
                        kPrivateFileMode, /*preserve_mode=*/false, ignored))) {
    error = "the workspace could not be created";
    return "";
  }
  return folder;
}

json SessionHost::Manage(const json& command) {
  json result = ControlProcess(command);
  const std::string action = JsonValue(command, "action", "list");
  if (JsonValue(result, "error", "").empty() && action != "list" &&
      action != "get" && action != "show" && action != "preview") {
    Publish("", "", {{"kind", "management.changed"}});
  }
  return result;
}

json SessionHost::SpaceControl(const json& command, const std::string& device,
                               std::string& error) {
  const std::string path = JsonValue(command, "cwd", "");
  error = JsonValue(command, "action", "") == "delete"
              ? DeleteSpace(path, device)
              : RenameSpace(path, Trim(JsonValue(command, "name", "")));
  const json spaces = Spaces();
  if (error.empty()) {
    Publish("", "", {{"kind", "spaces.changed"}, {"spaces", spaces}});
  }
  return {{"spaces", spaces}};
}

// The host belongs to no project: a request names the folder it is about,
// and without one only what is saved for all is in reach.
json SessionHost::Configure(const json& command) {
  const auto folder = CanonicalDirectory(JsonValue(command, "cwd", ""));
  auto manager =
      ConfigManager::Capture({}, folder ? folder->string() : std::string());
  return ConfigurationControl(command, manager);
}

std::string SessionHost::RenameSpace(const std::string& path,
                                     const std::string& name) {
  const auto space = SpaceAt(path);
  if (!space) return "no such workspace";
  if (!ValidSpaceName(name)) {
    return InvalidSpaceNameMessage();
  }
  for (const json& other : Spaces()) {
    if (JsonValue(other, "path", "") != space->string() &&
        AsciiLower(JsonValue(other, "name", "")) == AsciiLower(name)) {
      return "another workspace is called that";
    }
  }
  std::string error;
  // As µAgent spells its own folder, which a link may lead to.
  const fs::path kept = fs::path(UagentDir(kSpacesDir)) / space->filename();
  if (!EnsurePrivateDirectory((kept / kWorkspaceDir).string()) ||
      !AtomicWriteFile(LabelFile(kept).string(), name + "\n", kPrivateFileMode,
                       /*preserve_mode=*/false, error)) {
    return error.empty() ? "the workspace could not be renamed" : error;
  }
  return "";
}

std::string SessionHost::DeleteSpace(const std::string& path,
                                     const std::string& device) {
  const auto space = SpaceAt(path);
  if (!space) return "no such workspace";
  // Its conversations go the way one goes: closed when it runs, then
  // removed. Threads that work elsewhere belong to it by their folder.
  const auto next = [&]() -> json {
    std::lock_guard lock(mutex_);
    for (const auto& [id, session] : sessions_) {
      if (session->cwd == space->string() ||
          session->folder == space->string()) {
        return {{"session_id", id},
                {"generation", session->generation},
                {"kind",
                 session->pid > 0 && !session->exited ? "close" : "delete"}};
      }
    }
    return json();
  };
  std::set<std::string> closed;
  for (json command = next(); !command.is_null(); command = next()) {
    // A runtime that would not stop is not waited for twice.
    if (JsonValue(command, "kind", "") == "close" &&
        !closed.insert(JsonValue(command, "session_id", "")).second) {
      return "a conversation in the workspace did not stop; try again";
    }
    const SessionCommandResult result =
        ExecuteCommand(command, device, NewRequestId());
    if (!result.error.empty()) return result.error;
  }
  // Under the lock a conversation is created under: none starts in a
  // folder that is going.
  std::lock_guard lock(mutex_);
  if (std::ranges::any_of(sessions_, [&](const auto& held) {
        return held.second->cwd == space->string() ||
               held.second->folder == space->string();
      })) {
    return "a conversation started in the workspace meanwhile; try again";
  }
  std::error_code error;
  fs::remove_all(UagentDir(kHistoryDir) + "/" + WorkspaceId(space->string()),
                 error);
  fs::remove_all(*space, error);
  return error
             ? "the workspace's folder could not be removed: " + error.message()
             : "";
}
SessionCommandResult SessionHost::ExecuteCommand(
    json command, const std::string& device, const std::string& request_id) {
  UniqueLock lock(mutex_);
  SessionCommandResult result;
  result.outcome = {{"request_id", request_id}, {"accepted", true}};
  const SessionCommandKind kind =
      ParseSessionCommandKind(JsonValue(command, "kind", ""));
  if (kind == SessionCommandKind::kCreate) {
    // A workspace is a folder of µAgent's own, made by naming it: a
    // conversation that belongs to no project still gets a workspace commands
    // can write.
    std::string folder = JsonValue(command, "cwd", "");
    if (const std::string space = Trim(JsonValue(command, "space", ""));
        !space.empty()) {
      const size_t before = Spaces().size();
      folder = SpaceFolder(space, result.error);
      if (folder.empty()) return result;
      // A new one is listed wherever the workspaces are.
      if (const json spaces = Spaces(); spaces.size() != before) {
        replay_.Publish(epoch_, "", "",
                        {{"kind", "spaces.changed"}, {"spaces", spaces}},
                        false);
      }
    }
    const auto resolved = CanonicalDirectory(folder);
    if (!resolved) {
      result.error = "choose an accessible directory on the host";
      return result;
    }
    const std::filesystem::path& cwd = *resolved;
    // A folder has one panel: asking for it again returns it.
    const bool panel = JsonValue(command, "panel", false);
    auto path = panel ? PanelPath(cwd.string())
                      : HistoryPath(cwd.string(), "web-" + RandomToken(16));
    if (auto known = sessions_.find(HashHex(path)); known != sessions_.end()) {
      result.outcome["session"] = Metadata(*known->second);
      return result;
    }
    auto session = CreateSession(cwd.string(), path, "", result.error);
    if (session && panel) session->kind = kSessionKindPanel;
    if (session) {
      result.wake = true;
      result.outcome["session"] = Metadata(*session);
    }
    return result;
  }
  auto found = sessions_.find(JsonValue(command, "session_id", ""));
  if (found == sessions_.end()) {
    result.error = "unknown session";
    return result;
  }
  auto session = found->second;
  // A runtime another command is starting is waited for, not refused: the
  // settings of a stopped conversation are opened with several commands.
  changed_.wait(lock, [&] { return !session->connecting || stopping_; });
  if (stopping_ || !IsCurrentLocked(session.get())) {
    result.error = "unknown session";
    return result;
  }
  if (session->closing || session->status == SessionStatus::kUpdating ||
      session->status == SessionStatus::kDeleting) {
    result.error = "conversation update in progress";
    return result;
  }
  if (kind == SessionCommandKind::kFork && command.contains("argument")) {
    // Browser clients send the typed argument; the grammar lives natively.
    const ForkArgument parsed =
        ParseForkArgument(JsonValue(command, "argument", ""));
    command.erase("argument");
    command["title"] = parsed.title;
    command["turn"] = parsed.turn;
  }
  if (kind == SessionCommandKind::kFork && session->pid <= 0) {
    if (JsonValue(command, "generation", "") != session->generation) {
      result.error = "stale session; refresh before acting";
    } else if (sessions_.size() >= kMaxCatalogueEntries) {
      result.error = "session catalogue limit reached";
    } else {
      lock.unlock();
      json fork =
          SessionStore::Fork(session->path, JsonValue(command, "title", ""),
                             false, JsonValue(command, "turn", int64_t{0}),
                             JsonValue(command, "message_id", ""));
      RefreshCatalogue(true);
      lock.lock();
      result.outcome["result"] = fork;
      result.error = JsonValue(fork, "error", "");
    }
    return result;
  }
  if ((kind == SessionCommandKind::kRename ||
       kind == SessionCommandKind::kDelete) &&
      session->pid <= 0) {
    if (JsonValue(command, "generation", "") != session->generation) {
      result.error = "stale session; refresh before acting";
      return result;
    }
    std::string title = Trim(JsonValue(command, "title", ""));
    if (kind == SessionCommandKind::kRename && !ValidSessionTitle(title)) {
      result.error = InvalidSessionTitleMessage();
      return result;
    }
    // In a chat the title is the name a message opens with.
    const std::string chat = session->kind == kSessionKindPanel ? session->cwd
                             : session->member.empty() ? ""
                                                       : session->folder;
    std::string was;
    if (kind == SessionCommandKind::kRename && !chat.empty()) {
      result.error = ChatNameProblem(chat, title, session->path);
      if (!result.error.empty()) return result;
      was = ChatName(chat, session->path);
    }
    SessionStatus prior_status = session->status;
    session->status = kind == SessionCommandKind::kDelete
                          ? SessionStatus::kDeleting
                          : SessionStatus::kUpdating;
    lock.unlock();
    UniqueLock scan(scan_mutex_);
    std::unique_lock asset_lock = assets_.GuardMutation();
    std::string draft_path =
        directory_ + "/" + kDraftsDir + "/" + session->id + ".json";
    SessionStoreStatus stored;
    if (kind == SessionCommandKind::kDelete) {
      stored = SessionStore::Remove(session->path, draft_path);
    } else if (PathExists(session->path)) {
      stored = SessionStore::Rename(session->path, title);
    } else {
      if (!WriteDraft(*session, title, NowMillis())) {
        result.error = "cannot save conversation title";
      }
    }
    if (!stored.Ok()) result.error = stored.message;
    asset_lock.unlock();
    if (result.error.empty() && kind == SessionCommandKind::kDelete) {
      assets_.Invalidate();
    }
    lock.lock();
    session->status = prior_status;
    if (result.error.empty() && kind == SessionCommandKind::kDelete) {
      sessions_.erase(session->id);
      replay_.Publish(epoch_, session->id, "", {{"kind", "deleted"}},
                      !session->run_id.empty());
    } else if (result.error.empty()) {
      // Only a rename that was saved is one the chat hears of.
      ChatRenamed(chat, session->path, was, title);
      session->title = title;
      if (!chat.empty()) session->member = title;
      session->draft_title = title;
      session->updated = NowMillis();
      PublishMetadata(session->id, *session);
    }
    return result;
  }
  // A command that names no runtime is for the conversation, whichever
  // runtime serves it: a saved one's is started (it stopped for having
  // nothing to do), and one that started meanwhile is used as it is.
  if (ForwardsToWorker(kind) && JsonValue(command, "generation", "").empty()) {
    if (!ActivateLocked(session, result.error, lock, true)) return result;
    command["generation"] = session->generation;
    result.wake = true;
  }
  if (kind == SessionCommandKind::kDelete) {
    result.error = "stop and close this conversation before deleting it";
  } else if (kind == SessionCommandKind::kActivate) {
    if (ActivateLocked(session, result.error, lock, true)) {
      result.outcome["session"] = Metadata(*session);
    }
  } else if (JsonValue(command, "generation", "") != session->generation ||
             session->generation.empty()) {
    result.error = "stale worker generation; refresh before acting";
  } else if (kind == SessionCommandKind::kClose) {
    auto recorded = session->run_id.empty()
                        ? json::object()
                        : UpdateScheduledRun(session->run_id, "interrupted",
                                             "Session closed by user.");
    if (recorded.contains("error")) {
      result.error = JsonValue(recorded, "error", "cannot record interruption");
    } else if (session->exited) {
      DeactivateLocked(*session);
    } else {
      session->closing = true;
      session->status = SessionStatus::kClosing;
      if (PublishMetadata(session->id, *session)) changed_.notify_all();
      result.wake = true;
      lock.unlock();
      session->Send({{"kind", "close"}, {"request_id", request_id}});
      lock.lock();
      changed_.wait_for(lock, kWorkerShutdownTimeout,
                        [&] { return session->exited.load(); });
    }
  } else if (session->pid <= 0 || session->exited) {
    result.error = "session needs activation";
  } else if (ForwardsToWorker(kind)) {
    command["attachments"] = json::array();
    AssetClaim claim;
    if (outcomes_.PendingCommands(*session) >= kMaxPendingSessionCommands) {
      result.error = "too many unacknowledged worker commands";
    }
    if (const json* ids = JsonArray(command, "attachment_ids");
        result.error.empty() && ids && !ids->empty()) {
      lock.unlock();
      result.error = assets_.Claim(
          session->path, *ids, command,
          [&] {
            std::lock_guard guard(mutex_);
            if (!IsCurrentLocked(session.get()) || session->exited ||
                session->status == SessionStatus::kDeleting) {
              return std::string(
                  "session changed while claiming attachments; refresh");
            }
            return std::string();
          },
          claim);
      lock.lock();
    }
    if (result.error.empty() && JsonDump(command).size() > kCommandBytes) {
      result.error =
          "message and resolved attachments exceed the command limit";
    }
    if (result.error.empty()) {
      result.worker_request = HashHex(device) + HashHex(request_id);
      command["client_request_id"] = (kind == SessionCommandKind::kRecall ||
                                      kind == SessionCommandKind::kInsert)
                                         ? JsonValue(command, "target_id", "")
                                         : request_id;
      lock.unlock();
      bool dispatched = false;
      result.outcome =
          outcomes_.SendCommand(session, std::move(command),
                                result.worker_request, request_id, dispatched);
      lock.lock();
      if (!JsonValue(result.outcome, "accepted", false)) {
        result.error = JsonValue(result.outcome, "error", "command rejected");
        if (!dispatched && !claim.records.empty()) {
          lock.unlock();
          assets_.Unclaim(claim);
          lock.lock();
        }
      }
    }
  } else {
    result.error = "unsupported command";
  }
  return result;
}

}  // namespace uagent::session
