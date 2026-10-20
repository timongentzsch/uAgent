// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_HOST_H_
#define UAGENT_INCLUDE_APP_SESSION_HOST_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/asset_store.h"
#include "include/app/outcome_store.h"
#include "include/app/replay_log.h"
#include "include/app/session.h"
#include "include/cli/options.h"
#include "include/core/events.h"
#include "include/core/file_watch.h"
#include "include/core/json.h"
#include "include/core/thread_annotations.h"

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
  std::string id, path, cwd, title, draft_title, generation, error, binary;
  SessionStatus status = SessionStatus::kSaved;
  // "panel", "thread" or empty; a thread names its project `folder`,
  // and itself as a `member` of its panel's chat when it is one, with the
  // persona and skills it was given. `model` is the one the file names.
  std::string kind, folder, member, persona, skills, model;
  int64_t activated = 0;  // wall-clock ms of the last successful activation
  json state = json::object(), pending = nullptr;
  std::map<std::string, json> active_exchanges;
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
  bool has_before = false, has_detail = false, raw = false, artifact = false,
       has_http = false;
  std::string before{}, detail{}, http{}, part{}, offset{};
};

struct SnapshotResult {
  json value;
  int status = 200;
};

// The spaces: folders µAgent keeps for conversations that belong to no
// project, each under the name its user gave it. [{path, name}], by name.
json Spaces();
// The folder of the space called `name`, made when there is none. Empty
// with `error` when the name cannot be one or the folder cannot be made.
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
  // Follows the history on disk: sessions created elsewhere (a panel's
  // threads) appear, and those deleted elsewhere go, each published. A scan
  // within a second of the last is deferred to RescanDue(), never dropped.
  bool RefreshCatalogue(bool force = false);
  std::optional<std::chrono::steady_clock::time_point> RescanDue() const;
  void RefreshPresence();
  // Marks running runtimes (all, those in `cwd`, or the one named `only`)
  // for a fresh start that keeps their history: idle ones now, busy ones
  // when their turn ends.
  json RestartRunning(const std::string& cwd, const std::string& only = "");
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
  // A space is renamed by its label, its folder and so every conversation
  // in it staying what they are. Deleting one closes and removes its
  // conversations, then the folder with everything in it. Each returns why
  // it could not, or nothing.
  std::string RenameSpace(const std::string& path, const std::string& name);
  std::string DeleteSpace(const std::string& path, const std::string& device);
  std::string AssetPath(const std::string& id) const;
  // The folder a conversation works in: a thread's project, else its own.
  std::string Folder(const std::string& id) const;
  AssetStoreResult StoreAsset(const std::string& session_id,
                              const std::string& bytes, std::string name);
  SnapshotResult Snapshot(const std::string& id, const SnapshotQuery& query);
  bool Contains(const std::string& id) const;
  SessionCommandResult ExecuteCommand(json command, const std::string& device,
                                      const std::string& request_id);

 private:
  std::string epoch_;
  std::string executable_, directory_;
  // Ordered event log and attachment quotas live in their own components;
  // the host keeps the lock, the wakeup signal and session identity.
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
  };
  // Up to three immutable transcripts, most recently used first. The 32 MiB
  // serialized-file budget permits one oversized entry, as before; it is not
  // a process-memory bound. Readers can finish after an entry is evicted.
  // Loading and rendering never hold the cache or host mutex.
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
  json scheduled_view_ UAGENT_GUARDED_BY(mutex_) = {{"tasks", json::array()},
                                                    {"runs", json::array()}};
  struct RunUpdate {
    std::string id, status, error;
  };
  std::vector<RunUpdate> run_updates_ UAGENT_GUARDED_BY(mutex_);
  void RecordRun(const RunUpdate& update) UAGENT_REQUIRES(mutex_);
  bool PublishMetadata(const std::string& id, HostSession& session);
  std::shared_ptr<HostSession> CreateSession(const std::string& cwd,
                                             const std::string& path,
                                             const std::string& title,
                                             std::string& error)
      UAGENT_REQUIRES(mutex_);
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
  // Stamp-checked re-read of the schedule store. External writers (CLI,
  // other hosts) change the file under us; the tick must see their runs.
  // Callers hold mutex_.
  bool RefreshScheduleCacheLocked() UAGENT_REQUIRES(mutex_);
  std::vector<json> RefreshInvalidationsLocked(
      const std::vector<std::string>& projects) UAGENT_REQUIRES(mutex_);
  ScheduleTick TickSchedules() UAGENT_REQUIRES(mutex_);
  std::vector<std::string> InvalidationPaths(
      const std::vector<std::string>& projects) const;
  json Metadata(const HostSession& session) const;
  // What GET /api/sessions/{id} answers: `state` with the session's identity
  // and the replay cursor it is current to.
  json SnapshotBody(const HostSession& session, json state) const;
  json LiveSnapshot(const HostSession& session) const;
  // Snapshot's three answers: an HTTP exchange's stored body, a row's detail
  // the live view still holds (none when it holds none), and what the saved
  // history has.
  SnapshotResult HttpSnapshot(HostSession& session, const SnapshotQuery& query);
  std::optional<SnapshotResult> LiveDetail(HostSession& session,
                                           const SnapshotQuery& query);
  SnapshotResult SavedSnapshot(std::shared_ptr<HostSession> session,
                               const SnapshotQuery& query);
  std::shared_ptr<HostSession> StartScheduledRun(const json& run)
      UAGENT_REQUIRES(mutex_);
  // `lock` is the caller's lock on mutex_, passed only for condition-variable
  // waits. These helpers release and retake mutex_ itself, so the analysis
  // sees the unlocked interval; they hand it back held, as `lock` expects.
  bool ActivateLocked(const std::shared_ptr<HostSession>& session,
                      std::string& error, UniqueLock& lock, bool create)
      UAGENT_REQUIRES(mutex_);
  // Graceful binary-upgrade recycle: when the executable on disk is newer
  // than the attached worker's spawn record, close it and report true so
  // the caller spawns fresh. False keeps the worker: fresh record, or a
  // worker that ignores close (fail open, retried on the next touch).
  bool RecycleStaleWorkerLocked(const std::shared_ptr<HostSession>& session,
                                UniqueLock& lock) UAGENT_REQUIRES(mutex_);
  void Received(HostSession* session, json frame);
  void DeactivateLocked(HostSession& session) UAGENT_REQUIRES(mutex_);
  // Whether `session` is still the catalogue's entry for its id. Callers
  // hold mutex_.
  bool IsCurrentLocked(const HostSession* session) const
      UAGENT_REQUIRES(mutex_);
  // Publishes a lifecycle frame ({"kind", ...}) with the session's metadata.
  void PublishLifecycle(const HostSession& session, json frame);
  // Persists a draft's identity, so it outlives the host until its first
  // save. False when it cannot be written.
  bool WriteDraft(const HostSession& session, const std::string& title,
                  int64_t updated) const;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_HOST_H_
