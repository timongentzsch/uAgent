// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_HOST_H_
#define UAGENT_INCLUDE_APP_SESSION_HOST_H_
// The host that runs conversations for the web: its sessions, their state
// and events, schedules and the snapshots clients read.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/transcript.h"
#include "include/app/asset_store.h"
#include "include/app/outcome_store.h"
#include "include/app/replay_log.h"
#include "include/app/session.h"
#include "include/app/session_move.h"
#include "include/cli/options.h"
#include "include/core/events.h"
#include "include/core/file_watch.h"
#include "include/core/json.h"
#include "include/core/thread_annotations.h"
#include "include/core/workspace.h"

namespace uagent::session {

// Where a hosted session stands; only its name reaches the wire.
enum class SessionStatus {
  kDraft,
  kSaved,
  kStarting,
  kInterrupted,
  kFailed,
  kClosing,
  kUpdating,
  kDeleting,
  kWaiting,
  kRunning,
  kProcessing,
  kIdle,
};

inline const char* SessionStatusName(SessionStatus status) {
  switch (status) {
    case SessionStatus::kDraft:
      return "draft";
    case SessionStatus::kSaved:
      return "saved";
    case SessionStatus::kStarting:
      return "starting";
    case SessionStatus::kInterrupted:
      return "interrupted";
    case SessionStatus::kFailed:
      return "failed";
    case SessionStatus::kClosing:
      return "closing";
    case SessionStatus::kUpdating:
      return "updating";
    case SessionStatus::kDeleting:
      return "deleting";
    case SessionStatus::kWaiting:
      return "waiting";
    case SessionStatus::kRunning:
      return "running";
    case SessionStatus::kProcessing:
      return "processing";
    case SessionStatus::kIdle:
      return "idle";
  }
  return "saved";
}

// One attached conversation runtime. Socket and reader lifetime stay together;
// destroying the record wakes and joins its reader before dependent state dies.
struct HostSession {
  std::string run_id, task_id, launch_prompt, run_result;
  bool run_checkpoint = false, stop_sent = false;
  json launch = json::object();
  std::string host;
  std::string id, path, cwd, title, draft_title, generation, error, binary;
  SessionStatus status = SessionStatus::kSaved;
  // `parent` is the session that started an agent, `model` the file's.
  std::string parent, model;
  json suggestion = nullptr;
  // Spend, turns and to-do progress as last saved, until its runtime says.
  double cost = 0;
  int64_t turns = 0;
  json todo = nullptr;
  int64_t activated = 0;  // wall-clock ms of the last successful activation
  json state = json::object(), pending = nullptr;
  std::map<std::string, json> active_exchanges;
  // Diffs stored whole, by node id, until a checkpoint saves their paths.
  std::map<std::string, std::string> change_paths;
  int64_t updated = 0;
  bool live_truncated = false;
  json published = nullptr;
  bool turn_active = false, command_busy = false;
  uint64_t guidance = 0, incoming = 0, runtime_sequence = 0;
  pid_t pid = -1;
  Fd socket;
  Pipe stop;
  std::thread reader;
  std::mutex send_mutex;
  std::vector<std::string> awaiting;
  std::atomic<bool> exited{false};
  bool connecting = false, closing = false;
  // Settings that need a restart changed: recycle this runtime once idle.
  bool restart = false;

  bool RestartBusy() const {
    if (const json* activities = JsonArray(state, "activities")) {
      for (const json& activity : *activities) {
        if (ActivityRuns(activity)) return true;
      }
    }
    return turn_active || command_busy || !pending.is_null() ||
           !active_exchanges.empty() || LifecycleBusy();
  }

  // A lifecycle operation owns the session. Callers hold the host mutex.
  bool LifecycleBusy() const {
    return closing || status == SessionStatus::kUpdating ||
           status == SessionStatus::kDeleting;
  }

  ~HostSession();
  bool Send(json frame);
};

struct ScheduledCommand {
  std::shared_ptr<HostSession> session;
  json command;
};

struct ScheduleTick {
  std::vector<json> prepare;
  std::vector<ScheduledCommand> commands;
  std::vector<std::shared_ptr<HostSession>> activate;
};

struct SnapshotQuery {
  bool has_before = false, has_detail = false, raw = false, has_http = false;
  std::string before{}, detail{}, http{}, part{}, offset{};
};

struct SnapshotResult {
  json value;
  int status = 200;
};

// Folders µAgent keeps for conversations of no user folder: [{path, name}].
json Spaces();
// Whether `pins` is a list of at most kMaxPins {kind: session|folder, id}.
bool ValidPins(const json& pins);
// The folder of the space `name`, made when missing. Empty with `error`.
std::string SpaceFolder(const std::string& name, std::string& error);

struct SessionCommandResult {
  json outcome;
  std::string error, worker_request;
  bool wake = false;
};

struct HostWaitState {
  std::vector<std::string> paths;
  std::map<std::string, FileStamp> observed;
  std::chrono::steady_clock::time_point deadline;
  bool wake = false;
};

class SessionHost {
 public:
  SessionHost(std::string epoch, size_t byte_limit, std::string executable = {},
              std::string directory = {}, size_t event_limit = 2048);

  void Publish(const std::string& session, const std::string& generation,
               json value);
  uint64_t Cursor() const;
  ReplayBatch ReadReplay(uint64_t next, bool valid, uint64_t watermark,
                         size_t byte_budget) const;
  void WaitForReplay(uint64_t cursor, std::chrono::seconds timeout);
  std::vector<HostNotice> WaitForNotices();
  void Stop();
  void LoadDrafts();
  void LoadPins();
  // Finishes the moves a stopped host left unfinished, before anything reads
  // the catalogue. Returns why any could not be finished.
  std::vector<std::string> RecoverMoves();
  // Rescans the history on disk; a scan within a second of the last waits.
  bool RefreshCatalogue(bool force = false);
  std::optional<std::chrono::steady_clock::time_point> RescanDue() const;
  void RefreshPresence();
  // Restarts runtimes (all, in `cwd`, or `only`): idle now, busy at turn end.
  json RestartRunning(const std::string& cwd, const std::string& only = "",
                      const std::string& host = "");
  json CommandOutcome(const std::string& worker_request,
                      const std::string& client_request) const {
    return outcomes_.CommandOutcome(worker_request, client_request);
  }
  std::vector<json> RefreshInvalidations(
      const std::vector<std::string>& projects);
  std::vector<std::string> PresencePaths() const;
  HostWaitState RunSchedules(bool& recovered);
  void Shutdown();
  json Catalogue() const;
  // The settings screens' requests that the host answers itself.
  json Manage(const json& command);
  json SpaceControl(const json& command, const std::string& device,
                    std::string& error);
  json Configure(const json& command);
  // Where the web host serves a session's folder; unset outside the web host.
  void SetPreview(std::function<std::string(const std::string&)> preview) {
    preview_ = std::move(preview);
  }
  // Replaces the pinned conversations and folders, announced as pins.changed.
  json SetPins(const json& command, std::string& error);
  std::string AssetPath(const std::string& id) const;
  // The folder a conversation works in.
  std::string Folder(const std::string& id) const;
  WorkspaceIdentity WorkspaceOf(const std::string& id) const;
  json Execution(const std::string& id) const;
  using RemoteTarget = std::function<json(
      const std::string&, const std::string&, const std::string&)>;
  void SetRemoteTarget(RemoteTarget target) {
    remote_target_ = std::move(target);
  }
  AssetStoreResult StoreAsset(const std::string& session_id,
                              const std::string& bytes, std::string name);
  // Deletes a file the agent shared, record and bytes. Returns the error.
  std::string DeleteShared(const std::string& session_id,
                           const std::string& asset);
  SnapshotResult Snapshot(const std::string& id, const SnapshotQuery& query);
  bool Contains(const std::string& id) const;
  SessionCommandResult ExecuteCommand(json command, const std::string& device,
                                      const std::string& request_id);

 private:
  // Changes the label only: folder and conversations stay. Returns the error.
  std::string RenameSpace(const std::string& path, const std::string& name);
  // The folder a command names by `cwd` or `space` (made when missing).
  std::optional<std::filesystem::path> NamedFolder(const json& command,
                                                   std::string& error)
      UAGENT_REQUIRES(mutex_);
  // Moves a conversation and its agents to the history of `folder`.
  std::string MoveConversation(const std::shared_ptr<HostSession>& top,
                               const std::filesystem::path& folder,
                               const std::string& device, json& moved);
  std::string FinishMove(const SessionMove& move, MoveLeases& held);
  // Pinned conversations follow their move; a pinned folder stays.
  std::string FollowPins(const std::map<std::string, std::string>& ids);
  std::vector<SessionInfo> FamilyOf(const std::string& path)
      UAGENT_REQUIRES(scan_mutex_);
  std::string DeleteSpace(const std::string& path, const std::string& device);
  std::string epoch_;
  std::function<std::string(const std::string&)> preview_;
  std::string executable_, directory_;
  // replay_ runs under mutex_; assets_ has a lock of its own.
  ReplayLog replay_;
  AssetStore assets_;
  mutable std::mutex mutex_;
  // One scheduler owns preparation, including its intervals outside mutex_.
  std::mutex schedule_mutex_;
  std::condition_variable changed_;
  std::atomic<bool> stopping_{false};
  std::map<std::string, std::shared_ptr<HostSession>> sessions_
      UAGENT_GUARDED_BY(mutex_);
  mutable std::mutex scan_mutex_;
  SessionCatalogue catalogue_ UAGENT_GUARDED_BY(scan_mutex_);
  std::mutex history_mutex_;
  struct SavedHistory {
    std::string path;
    FileStamp stamp;
    SessionLoadResult loaded;
    Conversation conversation;
    TranscriptView view{conversation};
    size_t parsed_bytes = 0;
  };
  // Up to three immutable transcripts within 32 MiB of parsed size, most
  // recent first. Loading and rendering never hold the cache or host mutex.
  std::vector<std::shared_ptr<const SavedHistory>> history_
      UAGENT_GUARDED_BY(history_mutex_);
  std::shared_ptr<const SavedHistory> ReadHistory(const std::string& path);
  std::chrono::steady_clock::time_point scanned_
      UAGENT_GUARDED_BY(scan_mutex_){};
  bool rescan_ UAGENT_GUARDED_BY(scan_mutex_) =
      false;  // a scan was deferred by the one-second throttle
  OutcomeStore outcomes_;
  FileStamp library_stamp_ UAGENT_GUARDED_BY(mutex_);
  FileStamp schedule_stamp_ UAGENT_GUARDED_BY(mutex_);
  std::map<std::string, FileStamp> prompt_stamps_ UAGENT_GUARDED_BY(mutex_);
  json schedule_state_ UAGENT_GUARDED_BY(mutex_) = json::object();
  json pins_ UAGENT_GUARDED_BY(mutex_) = json::array();
  json scheduled_view_ UAGENT_GUARDED_BY(mutex_) = {{"tasks", json::array()},
                                                    {"runs", json::array()}};
  struct RunUpdate {
    std::string id, status, error;
  };
  std::vector<RunUpdate> run_updates_ UAGENT_GUARDED_BY(mutex_);
  void RecordRun(const RunUpdate& update) UAGENT_REQUIRES(mutex_);
  // Appends to the log and wakes its waiters. Callers hold mutex_.
  void Record(const std::string& session, const std::string& generation,
              json value, bool run_owned);
  bool PublishMetadata(const std::string& id, HostSession& session);
  std::shared_ptr<HostSession> CreateSession(
      const std::string& cwd, const std::string& path, const std::string& title,
      std::string& error, const std::string& host = {}) UAGENT_REQUIRES(mutex_);
  RemoteTarget remote_target_;
  Connection OpenRuntime(const HostSession& session, bool create,
                         std::string& error) const;
  // False when the frame changed nothing a browser shows.
  bool ApplyRuntimeFrame(HostSession& session, json& frame);
  // The parts of a runtime frame that change a session.
  void ApplyStateFrame(HostSession& session, json& frame);
  static void RecordRunEvent(HostSession& session, const std::string& type,
                             json& frame);
  // Outcome labels shared by live frames and the schedule supervisor.
  static std::string RunResultFor(const std::string& outcome);
  std::chrono::steady_clock::time_point NextScheduleDeadline() const
      UAGENT_REQUIRES(mutex_);
  bool RecoverSchedules(std::vector<std::shared_ptr<HostSession>>& activate)
      UAGENT_REQUIRES(mutex_);
  // Stamp-checked: the CLI and other hosts write the schedule store too.
  bool RefreshScheduleCacheLocked() UAGENT_REQUIRES(mutex_);
  std::vector<json> RefreshInvalidationsLocked(
      const std::vector<std::string>& projects) UAGENT_REQUIRES(mutex_);
  ScheduleTick TickSchedules() UAGENT_REQUIRES(mutex_);
  std::vector<std::string> InvalidationPaths(
      const std::vector<std::string>& projects) const;
  json Metadata(const HostSession& session) const;
  // GET /api/sessions/{id}: `state` with identity and replay cursor.
  json SnapshotBody(const HostSession& session, json state) const;
  json LiveSnapshot(const HostSession& session) const;
  // Snapshot's answers: a stored HTTP body, a live row's detail, saved history.
  SnapshotResult HttpSnapshot(HostSession& session, const SnapshotQuery& query);
  std::optional<SnapshotResult> LiveDetail(HostSession& session,
                                           const SnapshotQuery& query);
  SnapshotResult SavedSnapshot(std::shared_ptr<HostSession> session,
                               const SnapshotQuery& query);
  std::shared_ptr<HostSession> StartScheduledRun(const json& run,
                                                 const std::string& checked)
      UAGENT_REQUIRES(mutex_);
  // `lock` is the caller's lock on mutex_. These helpers release and retake
  // mutex_ itself, so the analysis sees the unlocked interval.
  bool ActivateLocked(const std::shared_ptr<HostSession>& session,
                      std::string& error, UniqueLock& lock, bool create)
      UAGENT_REQUIRES(mutex_);
  // True after closing a worker older than the executable on disk, so the
  // caller spawns fresh. A worker that ignores close is kept.
  bool RecycleStaleWorkerLocked(const std::shared_ptr<HostSession>& session,
                                UniqueLock& lock) UAGENT_REQUIRES(mutex_);
  void Received(HostSession* session, json frame);
  void DeactivateLocked(HostSession& session) UAGENT_REQUIRES(mutex_);
  // Whether `session` is still the catalogue's entry for its id.
  bool IsCurrentLocked(const HostSession* session) const
      UAGENT_REQUIRES(mutex_);
  // Publishes a lifecycle frame ({"kind", ...}) with the session's metadata.
  void PublishLifecycle(const HostSession& session, json frame);
  // Persists a draft's identity so it outlives the host; false on failure.
  bool WriteDraft(const HostSession& session, const std::string& title,
                  int64_t updated) const;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_HOST_H_
