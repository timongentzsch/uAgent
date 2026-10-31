// Copyright 2026 Timon Gentzsch

#include "include/app/session_move.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/protocol.h"
#include "include/agent/session_store.h"
#include "include/app/launch.h"
#include "include/app/schedule.h"
#include "include/app/session.h"
#include "include/app/session_host.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/mailbox.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent::session {
namespace {
namespace fs = std::filesystem;

constexpr int64_t kMoveFormat = 1;
constexpr size_t kMoveFiles = 256;
constexpr size_t kMoveJournalBytes = KiB(256);

// What sits beside a saved session: everything named <file>.* (its lease,
// journal, assets, edits and plan), and its shared copy.
std::vector<fs::path> Sidecars(const fs::path& file) {
  std::vector<fs::path> found;
  const std::string prefix = file.filename().string() + ".";
  const std::string shared = file.stem().string() + ".share.md";
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(file.parent_path(), error)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with(prefix) || name == shared) {
      found.push_back(entry.path());
    }
  }
  return found;
}

// Never over something: a name taken is a conflict, not a replacement.
std::string Rename(const fs::path& from, const fs::path& to) {
  if (PathExists(to)) return to.string() + " exists already";
  std::error_code error;
  fs::rename(from, to, error);
  return error ? "cannot move " + from.string() + ": " + error.message() : "";
}

// The session file moves last, so while it is at `from` the move of what sits
// beside it is not done; once it is gone from `from` there is nothing left.
std::string MoveFiles(const MovedFile& file) {
  const fs::path from(file.from), to(file.to);
  if (!PathExists(from)) {
    return PathExists(to) ? "" : file.from + " is gone, and not where it moved";
  }
  if (!EnsurePrivateDirectory(to.parent_path().string())) {
    return "cannot create " + to.parent_path().string();
  }
  for (const fs::path& side : Sidecars(from)) {
    if (std::string error = Rename(side, to.parent_path() / side.filename());
        !error.empty()) {
      return error;
    }
  }
  return Rename(from, to);
}

// The note, once: as a message the model reads in its next request, and as a
// row. Nothing runs, so nothing wakes. False, with `failed` set, when the
// transcript cannot take it.
bool AddNote(SessionRecord& record, const std::string& note,
             const std::string& to_cwd, std::string& failed) {
  SessionState& state = record.state;
  if (state.message_kinds.back() == MessageKind::kInternal &&
      JsonValue(state.messages.back(), "content", "") == note) {
    return false;
  }
  Conversation conversation;
  if (!conversation.Restore(state.messages, state.message_kinds, state.archive,
                            state.archive_dropped_segments, state.display)) {
    failed = "the conversation's transcript is invalid";
    return false;
  }
  conversation.Push(HarnessMessage(note), MessageKind::kInternal);
  conversation.RecordEntry(
      {{"view", {{"verb", {"Moving", "Moved"}}, {"target", to_cwd}}},
       {"state", "ok"},
       {"body_kind", "text"},
       {"text", note},
       {"turn_root", ""}});
  state.messages = conversation.Messages();
  state.message_kinds = conversation.Kinds();
  state.display = conversation.DisplayMetadata();
  return true;
}

// Makes the moved session at `move.files[index].to` agree with where it is.
// An agent that worked in the old folder works in the new one; one that
// worked elsewhere (its own worktree) keeps its folder. Asset and edit paths
// follow their files, and the ids of the moved sessions, which the page and
// the model name agents by, follow their new paths.
std::string Rewrite(const SessionMove& move, size_t index,
                    const std::map<std::string, std::string>& ids) {
  const MovedFile& file = move.files[index];
  std::vector<std::pair<std::string, std::string>> renames = {
      {file.from + kSessionAssetsSuffix + "/",
       file.to + kSessionAssetsSuffix + "/"},
      {file.from + kSessionEditsSuffix + "/",
       file.to + kSessionEditsSuffix + "/"}};
  renames.insert(renames.end(), ids.begin(), ids.end());
  std::string failed;
  const SessionStoreStatus status =
      SessionStore::Edit(file.to, [&](SessionRecord& record) {
        bool changed = false;
        SessionMetadata& metadata = record.metadata;
        if (CanonicalAccessPath(metadata.cwd) ==
            CanonicalAccessPath(move.from_cwd)) {
          metadata.cwd = move.to_cwd;
          changed = true;
        }
        if (const auto parent =
                ids.find(JsonValue(metadata.agent, "parent", ""));
            parent != ids.end()) {
          metadata.agent["parent"] = parent->second;
          changed = true;
        }
        SessionStore::VisitStrings(record.state, [&](json& value) {
          auto& text = value.get_ref<std::string&>();
          for (const auto& [from, to] : renames) {
            if (text.find(from) != std::string::npos) {
              ReplaceAll(text, from, to);
              changed = true;
            }
          }
        });
        if (index == 0 && AddNote(record, move.note, move.to_cwd, failed)) {
          changed = true;
        }
        return changed && failed.empty();
      });
  if (!failed.empty()) return failed;
  return status.Ok() ? "" : "cannot update " + file.to + ": " + status.message;
}

// Its token usage is kept under the session's id, which the move changes.
void MoveLedger(const std::string& from_id, const std::string& to_id) {
  const std::string from = UsageLedger(from_id), to = UsageLedger(to_id);
  if (PathExists(from) && !PathExists(to)) {
    std::error_code error;
    fs::rename(from, to, error);
  }
}
}  // namespace

json MoveJson(const SessionMove& move) {
  json files = json::array();
  for (const MovedFile& file : move.files) {
    files.push_back({{"from", file.from}, {"to", file.to}});
  }
  return {{"format", kMoveFormat},
          {"from", move.from_cwd},
          {"to", move.to_cwd},
          {"note", move.note},
          {"files", std::move(files)}};
}

// A move is only what it says: every file a session file of some folder's
// history, going to the history of the new folder under the same name.
bool ValidMove(const SessionMove& move) {
  if (move.from_cwd.empty() || move.to_cwd.empty() ||
      move.from_cwd == move.to_cwd || move.note.empty() || move.files.empty() ||
      move.files.size() > kMoveFiles) {
    return false;
  }
  const fs::path history = UagentDir(kHistoryDir);
  const fs::path target = history / WorkspaceId(move.to_cwd);
  return std::ranges::all_of(move.files, [&](const MovedFile& file) {
    const fs::path from(file.from), to(file.to);
    return from.lexically_normal() == from && to.lexically_normal() == to &&
           from.extension() == ".json" &&
           from.parent_path().parent_path() == history &&
           from.parent_path() != target && to.parent_path() == target &&
           to.filename() == from.filename();
  });
}

// The journal drives renames, so it is trusted no further than ValidMove.
bool MoveFromJson(const json& value, SessionMove& move) {
  SessionMove parsed{JsonValue(value, "from", ""),
                     JsonValue(value, "to", ""),
                     JsonValue(value, "note", ""),
                     {}};
  const json* files = JsonArray(value, "files");
  if (JsonValue(value, "format", int64_t{0}) != kMoveFormat || !files) {
    return false;
  }
  for (const json& item : *files) {
    parsed.files.push_back(
        {JsonValue(item, "from", ""), JsonValue(item, "to", "")});
  }
  if (!ValidMove(parsed)) return false;
  move = std::move(parsed);
  return true;
}

std::map<std::string, std::string> MovedIds(const SessionMove& move) {
  std::map<std::string, std::string> ids;
  for (const MovedFile& file : move.files) {
    ids[SessionIdFor(file.from)] = SessionIdFor(file.to);
  }
  return ids;
}

std::string HoldMove(const SessionMove& move, MoveLeases& held) {
  held = MoveLeases(move.files.size());
  for (size_t i = 0; i < move.files.size(); ++i) {
    std::string error;
    if (PathExists(move.files[i].from) &&
        !held[i].Acquire(SessionLockPath(move.files[i].from), error)) {
      return "a conversation is open elsewhere: " + error;
    }
  }
  return "";
}

std::string CarryOutMove(const SessionMove& move, MoveLeases& held) {
  for (const MovedFile& file : move.files) {
    if (std::string error = MoveFiles(file); !error.empty()) {
      return error;
    }
  }
  held.clear();
  const auto ids = MovedIds(move);
  for (size_t i = 0; i < move.files.size(); ++i) {
    if (std::string error = Rewrite(move, i, ids); !error.empty()) {
      return error;
    }
  }
  for (const auto& [from, to] : ids) MoveLedger(from, to);
  std::map<std::string, std::string> paths;
  for (const MovedFile& file : move.files) paths[file.from] = file.to;
  // A run's record of where its conversation is cannot stop a move that is
  // done.
  if (const json runs = MoveScheduledSessions(paths); runs.contains("error")) {
    DebugLog("move_schedule_failed", {{"error", runs["error"]}});
  }
  return "";
}

namespace {
// The conversation at `path` and the agents saved beside it, started by it or
// by those, it first.
std::vector<SessionInfo> FamilyIn(std::vector<SessionInfo> listed,
                                  const std::string& path) {
  const fs::path directory = fs::path(path).parent_path();
  std::erase_if(listed, [&](const SessionInfo& info) {
    return !info.error.empty() ||
           fs::path(info.path).parent_path() != directory;
  });
  std::vector<SessionInfo> family;
  std::set<std::string> ids;
  const auto top = std::ranges::find(listed, path, &SessionInfo::path);
  if (top == listed.end()) return family;
  family.push_back(*top);
  ids.insert(SessionIdFor(path));
  for (bool grew = true; grew;) {
    grew = false;
    for (const SessionInfo& info : listed) {
      if (!ids.contains(info.parent) ||
          !ids.insert(SessionIdFor(info.path)).second) {
        continue;
      }
      family.push_back(info);
      grew = true;
    }
  }
  return family;
}

// Whether a turn, or an answer to a person, keeps its runtime busy.
bool Busy(const HostSession& session) {
  return session.turn_active || session.command_busy ||
         !session.pending.is_null();
}

bool Occupied(const fs::path& file) {
  return PathExists(file) || !Sidecars(file).empty();
}
}  // namespace

std::vector<SessionInfo> SessionHost::FamilyOf(const std::string& path) {
  return FamilyIn(catalogue_.List(SessionScope::kAll), path);
}

std::string SessionHost::FollowPins(
    const std::map<std::string, std::string>& ids) {
  std::lock_guard lock(mutex_);
  json pins = pins_;
  bool changed = false;
  for (json& pin : pins) {
    const auto moved = ids.find(JsonValue(pin, "id", ""));
    if (JsonValue(pin, "kind", "") == "session" && moved != ids.end()) {
      pin["id"] = moved->second;
      changed = true;
    }
  }
  std::string error;
  if (!changed ||
      !WritePrivateFile(directory_ + "/" + kPinsFile, JsonDump(pins), error)) {
    return error;
  }
  pins_ = std::move(pins);
  Record("", "", {{"kind", "pins.changed"}, {"pins", pins_}}, false);
  return "";
}

std::string SessionHost::FinishMove(const SessionMove& move, MoveLeases& held) {
  if (std::string error = CarryOutMove(move, held); !error.empty()) {
    return error;
  }
  // A conversation made in the web keeps a record of its identity until it
  // is deleted; left at the old id it would come back as a draft.
  const auto ids = MovedIds(move);
  std::error_code ignored;
  for (const auto& [from, to] : ids) {
    fs::remove(directory_ + "/" + kDraftsDir + "/" + from + ".json", ignored);
  }
  return FollowPins(ids);
}

// Closes what is idle, then moves under the locks that keep the catalogue
// and the attachments still: the family is read from disk once more, so an
// agent saved a moment ago moves with the rest.
std::string SessionHost::MoveConversation(
    const std::shared_ptr<HostSession>& top, const fs::path& folder,
    const std::string& device, json& moved) {
  const std::string to_cwd = folder.string();
  std::string from_cwd;
  {
    std::lock_guard lock(mutex_);
    if (top->status == SessionStatus::kDraft) {
      return "this conversation has nothing saved yet";
    }
    if (!top->parent.empty()) {
      return "this is an agent; move the conversation that started it";
    }
    if (Busy(*top)) {
      return "\"" + top->title + "\" is running; wait for it or stop it";
    }
    from_cwd = top->cwd;
  }
  const fs::path target = fs::path(HistoryPath(to_cwd, "x")).parent_path();
  if (fs::path(top->path).parent_path() == target) {
    return "it is in " + to_cwd + " already";
  }
  RefreshCatalogue(true);
  std::vector<SessionInfo> family;
  {
    std::lock_guard scan(scan_mutex_);
    family = FamilyOf(top->path);
  }
  for (const SessionInfo& info : family) {
    json close;
    {
      std::lock_guard lock(mutex_);
      const auto member = sessions_.find(SessionIdFor(info.path));
      if (member == sessions_.end() || member->second->pid <= 0 ||
          member->second->exited) {
        continue;
      }
      if (Busy(*member->second)) {
        return "\"" + info.title + "\" is running; wait for it or stop it";
      }
      close = {{"kind", "close"},
               {"session_id", member->first},
               {"generation", member->second->generation}};
    }
    if (const auto closed = ExecuteCommand(close, device, NewRequestId());
        !closed.error.empty()) {
      return closed.error;
    }
  }
  // A closed runtime is still closing for a moment after it answers.
  {
    UniqueLock lock(mutex_);
    changed_.wait_for(
        lock, kWorkerShutdownTimeout, [&]() UAGENT_REQUIRES(mutex_) {
          for (const SessionInfo& info : family) {
            const auto member = sessions_.find(SessionIdFor(info.path));
            if (member != sessions_.end() && member->second->closing) {
              return false;
            }
          }
          return true;
        });
  }

  UniqueLock scan(scan_mutex_);
  std::unique_lock asset_lock = assets_.GuardMutation();
  family = FamilyOf(top->path);
  if (family.empty() || family.size() > kMoveFiles) {
    return family.empty() ? "the conversation is not saved"
                          : "too many agents to move at once";
  }
  SessionMove move{from_cwd, to_cwd, "", {}};
  move.note = "Moved from " + from_cwd + " to " + to_cwd +
              ". Files stayed in " + from_cwd + ".";
  std::vector<std::pair<std::shared_ptr<HostSession>, SessionStatus>> flagged;
  UniqueLock lock(mutex_);
  std::string error;
  for (const SessionInfo& info : family) {
    const auto member = sessions_.find(SessionIdFor(info.path));
    if (member != sessions_.end()) {
      HostSession& session = *member->second;
      if (session.closing || session.connecting ||
          session.status == SessionStatus::kUpdating ||
          session.status == SessionStatus::kDeleting) {
        error = "\"" + info.title + "\" is busy; try again";
      } else if (session.pid > 0 && !session.exited) {
        error = Busy(session) ? "\"" + info.title + "\" is running"
                              : "\"" + info.title +
                                    "\" is still open, in a terminal maybe; "
                                    "close it first";
      }
      flagged.emplace_back(member->second, session.status);
    }
    if (!error.empty()) break;
    move.files.push_back(
        {info.path, HistoryPath(to_cwd, fs::path(info.path).stem().string())});
  }
  if (error.empty()) {
    for (auto& [session, prior] : flagged) {
      session->status = SessionStatus::kUpdating;
    }
  }
  lock.unlock();
  // The rest takes no host lock but the two held: a session it touches is
  // marked updating, so no command or runtime starts on it.
  const std::string journal =
      directory_ + "/move-" +
      SessionIdFor(move.files.empty() ? "" : move.files[0].from) + ".json";
  MoveLeases held;
  if (error.empty() && PathExists(journal)) {
    error =
        "an earlier move of this conversation is unfinished; restart the "
        "host to finish it";
  }
  for (size_t i = 0; error.empty() && i < family.size(); ++i) {
    const SessionInfo& info = family[i];
    if (!PendingMail(MailboxIdFor(info.path)).empty()) {
      error =
          "mail is waiting for \"" + info.title + "\"; let it read it first";
    } else if (Occupied(move.files[i].to)) {
      error = move.files[i].to + " exists already";
    } else if (const auto read = SessionStore::Inspect(info.path);
               !read.record) {
      error = "cannot read \"" + info.title + "\": " + read.status.message;
    }
  }
  if (error.empty() && !ValidMove(move)) {
    error = "this conversation is not kept in a folder's history";
  }
  if (error.empty()) error = HoldMove(move, held);
  // Nothing moves before the journal is durable.
  if (std::string written;
      error.empty() &&
      !WritePrivateFile(journal, JsonDump(MoveJson(move)), written)) {
    error = written;
  }
  if (error.empty()) {
    error = FinishMove(move, held);
    if (std::error_code ignored; error.empty()) {
      fs::remove(journal, ignored);
    }
  }

  lock.lock();
  for (auto& [session, prior] : flagged) {
    session->status = prior;
  }
  if (!error.empty()) return error;
  const auto ids = MovedIds(move);
  std::vector<std::pair<std::string, bool>> gone;
  for (const MovedFile& file : move.files) {
    const std::string id = SessionIdFor(file.from);
    const auto member = sessions_.find(id);
    gone.emplace_back(
        id, member != sessions_.end() && !member->second->run_id.empty());
    sessions_.erase(id);
  }
  lock.unlock();
  asset_lock.unlock();
  assets_.Invalidate();
  scan.unlock();
  // The new ids appear before the old ones go, so a page that follows
  // `moved_to` finds its conversation.
  RefreshCatalogue(true);
  lock.lock();
  for (const auto& [id, run_owned] : gone) {
    Record(id, "", {{"kind", "deleted"}, {"moved_to", ids.at(id)}}, run_owned);
  }
  moved = {{"moved", true},
           {"id", ids.at(gone[0].first)},
           {"path", move.files[0].to},
           {"cwd", to_cwd},
           {"title", family[0].title}};
  return "";
}

std::vector<std::string> SessionHost::RecoverMoves() {
  std::vector<std::string> failed;
  std::vector<fs::path> journals;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(directory_, error)) {
    if (entry.path().filename().string().starts_with("move-") &&
        entry.path().extension() == ".json") {
      journals.push_back(entry.path());
    }
  }
  for (const fs::path& journal : journals) {
    const std::string name = journal.filename().string();
    SessionMove move;
    if (!MoveFromJson(ReadJsonFile(journal.string(), kMoveJournalBytes),
                      move)) {
      failed.push_back(name +
                       " is not a move this host can finish; left as it is");
      continue;
    }
    MoveLeases held;
    std::string reason = HoldMove(move, held);
    if (reason.empty()) reason = FinishMove(move, held);
    if (!reason.empty()) {
      failed.push_back(name + ": " + reason);
      continue;
    }
    fs::remove(journal, error);
  }
  return failed;
}

}  // namespace uagent::session
