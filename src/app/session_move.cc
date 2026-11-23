// Copyright 2026 Timon Gentzsch
// Moving a conversation and its agents to another folder: each file stays
// where it is and is told its new folder.

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/protocol.h"
#include "include/agent/session_store.h"
#include "include/app/session.h"
#include "include/app/session_host.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent::session {
namespace {
namespace fs = std::filesystem;

constexpr size_t kMoveFiles = 256;

struct SessionMove {
  std::string from_cwd, to_cwd;
  // What the conversation is told, and shows as a row.
  std::string note;
};

// Adds the note once, as a message for the next request and as a row; nothing
// wakes. False, with `failed` set, when the transcript cannot take it.
bool AddNote(SessionRecord& record, const SessionMove& move,
             std::string& failed) {
  const std::string& note = move.note;
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
      {{"view", {{"verb", {"Moving", "Moved"}}, {"target", move.to_cwd}}},
       {"moved", {{"from", move.from_cwd}}},
       {"state", "ok"},
       {"body_kind", "text"},
       {"text", note},
       {"turn_root", ""}});
  state.messages = conversation.Messages();
  state.message_kinds = conversation.Kinds();
  state.display = conversation.DisplayMetadata();
  return true;
}

// Agents of the old folder work in the new one, others keep theirs; the
// conversation itself is told. Doing it again changes nothing.
std::string Rewrite(const SessionMove& move, const std::string& path,
                    bool top) {
  std::string failed;
  const SessionStoreStatus status =
      SessionStore::Edit(path, [&](SessionRecord& record) {
        bool changed = false;
        SessionMetadata& metadata = record.metadata;
        if (CanonicalAccessPath(metadata.cwd) ==
            CanonicalAccessPath(move.from_cwd)) {
          metadata.cwd = move.to_cwd;
          metadata.suggestion = nullptr;
          changed = true;
        }
        if (top && AddNote(record, move, failed)) changed = true;
        return changed && failed.empty();
      });
  if (!failed.empty()) return failed;
  return status.Ok() ? "" : "cannot update " + path + ": " + status.message;
}

// The conversation at `path` and the agents started by it or by those, it
// first.
std::vector<SessionInfo> FamilyIn(std::vector<SessionInfo> listed,
                                  const std::string& path) {
  std::erase_if(listed,
                [](const SessionInfo& info) { return !info.error.empty(); });
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

bool Busy(const HostSession& session) {
  return session.turn_active || session.command_busy ||
         !session.pending.is_null();
}
}  // namespace

std::vector<SessionInfo> SessionHost::FamilyOf(const std::string& path) {
  return FamilyIn(catalogue_.List(SessionScope::kAll), path);
}

// Closes what is idle, then rewrites under the lock that keeps the catalogue
// still: the family is read from disk once more, so an agent saved a moment
// ago moves with the rest.
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
      return top->side.is_null()
                 ? "this is an agent; move the conversation that started it"
                 : "this is a side chat; move its conversation";
    }
    if (Busy(*top)) {
      return "\"" + top->title + "\" is running; wait for it or stop it";
    }
    from_cwd = top->cwd;
  }
  if (CanonicalAccessPath(from_cwd) == CanonicalAccessPath(to_cwd)) {
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
  family = FamilyOf(top->path);
  if (family.empty() || family.size() > kMoveFiles) {
    return family.empty() ? "the conversation is not saved"
                          : "too many agents to move at once";
  }
  const SessionMove move{from_cwd, to_cwd,
                         "Moved from " + from_cwd + " to " + to_cwd +
                             ". Files stayed in " + from_cwd + "."};
  std::vector<std::pair<std::shared_ptr<HostSession>, SessionStatus>> flagged;
  UniqueLock lock(mutex_);
  std::string error;
  for (const SessionInfo& info : family) {
    const auto member = sessions_.find(SessionIdFor(info.path));
    if (member != sessions_.end()) {
      HostSession& session = *member->second;
      if (session.LifecycleBusy() || session.connecting) {
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
  }
  if (error.empty()) {
    for (auto& [session, prior] : flagged) {
      session->status = SessionStatus::kUpdating;
    }
  }
  lock.unlock();
  // The rest takes no host lock but the scan's: a session it touches is
  // marked updating, so no command or runtime starts on it. A conversation
  // open elsewhere is found before anything changes; each rewrite then takes
  // its lease again.
  for (const SessionInfo& info : family) {
    FileLease lease;
    if (std::string held;
        error.empty() && !lease.Acquire(SessionLockPath(info.path), held)) {
      error = "a conversation is open elsewhere: " + held;
    }
  }
  // Agents first: one that stopped halfway is finished by moving again.
  for (auto member = family.rbegin(); error.empty() && member != family.rend();
       ++member) {
    error = Rewrite(move, member->path, member == family.rend() - 1);
  }
  // A conversation made in the web keeps the record it had as a draft; its
  // folder is the file's now.
  for (const SessionInfo& info : family) {
    std::error_code ignored;
    if (!error.empty()) break;
    fs::remove(
        directory_ + "/" + kDraftsDir + "/" + SessionIdFor(info.path) + ".json",
        ignored);
  }
  lock.lock();
  for (auto& [session, prior] : flagged) {
    session->status = prior;
  }
  if (!error.empty()) return error;
  lock.unlock();
  scan.unlock();
  RefreshCatalogue(true);
  lock.lock();
  // Its id stays; what a page holds of its transcript does not. Its agents'
  // new folder reaches pages as their metadata.
  const std::string id = SessionIdFor(family[0].path);
  const auto member = sessions_.find(id);
  Record(id, "", {{"kind", "moved"}},
         member != sessions_.end() && !member->second->run_id.empty());
  moved = {{"moved", true},
           {"id", id},
           {"path", family[0].path},
           {"cwd", to_cwd},
           {"title", family[0].title}};
  return "";
}

}  // namespace uagent::session
