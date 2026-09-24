// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_HOST_H_
#define UAGENT_INCLUDE_APP_SESSION_HOST_H_
// Transport-independent host event ordering and bounded reconnect replay.
// The HTTP adapter serializes these frames but does not own their sequence.

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
#include <string>
#include <thread>
#include <vector>

#include "include/agent/session_store.h"
#include "include/app/asset_store.h"
#include "include/app/options.h"
#include "include/app/outcome_store.h"
#include "include/app/replay_log.h"
#include "include/app/session.h"
#include "include/core/events.h"
#include "include/core/file_watch.h"
#include "include/core/json.h"

namespace uagent::session {

// One attached conversation runtime. Socket and reader lifetime stay together;
// destroying the record wakes and joins its reader before dependent state dies.
struct HostSession {
  std::string run_id, task_id, launch_prompt, run_result;
  bool run_checkpoint = false, stop_sent = false;
  json launch = json::object();
  std::string id, path, cwd, title, draft_title, generation, status = "saved",
                                                             error, binary;
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

  ~HostSession();
  bool Send(json frame);
};

struct ScheduledCommand {
  std::shared_ptr<HostSession> session;
  json command;
};

struct ScheduleTick {
  std::vector<ScheduledCommand> commands;
  std::vector<std::shared_ptr<HostSession>> activate;
};

struct SnapshotQuery {
  bool has_before = false, has_detail = false, raw = false, artifact = false,
       has_http = false;
  std::string before, detail, http, part, offset;
};

struct SnapshotResult {
  json value;
  int status = 200;
};

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

  HostReplay Publish(const std::string& session, const std::string& generation,
                     json value);
  uint64_t Cursor() const;
  const std::string& Epoch() const { return epoch_; }
  ReplayBatch ReadReplay(uint64_t next, bool valid, uint64_t watermark) const;
  void WaitForReplay(uint64_t cursor, std::chrono::seconds timeout);
  std::vector<HostNotice> WaitForNotices();
  void Stop();
  void LoadDrafts();
  bool RefreshCatalogue(bool force = false);
  void RefreshPresence();
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
  std::string AssetPath(const std::string& id) const;
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
  std::condition_variable changed_;
  std::atomic<bool> stopping_{false};
  std::map<std::string, std::shared_ptr<HostSession>> sessions_;
  std::mutex scan_mutex_;
  std::mutex history_mutex_;
  std::chrono::steady_clock::time_point scanned_{};
  OutcomeStore outcomes_;
  FileStamp library_stamp_, schedule_stamp_;
  std::map<std::string, FileStamp> prompt_stamps_;
  json schedule_state_ = json::object();
  json scheduled_view_ = {{"tasks", json::array()}, {"runs", json::array()}};
  struct RunUpdate {
    std::string id, status, error;
  };
  std::vector<RunUpdate> run_updates_;
  void RecordRun(const RunUpdate& update);
  std::vector<SessionInfo> DiscoverSessions() const;
  bool PublishMetadata(const std::string& id, HostSession& session);
  std::shared_ptr<HostSession> CreateSession(const std::string& cwd,
                                             const std::string& path,
                                             const std::string& title,
                                             std::string& error);
  Connection OpenRuntime(const HostSession& session, bool create,
                         std::string& error) const;
  void ApplyRuntimeFrame(HostSession& session, json& frame);
  // Outcome labels shared by live frames and the schedule supervisor.
  static std::string RunResultFor(const std::string& outcome);
  std::chrono::steady_clock::time_point NextScheduleDeadline() const;
  bool RecoverSchedules(std::vector<std::shared_ptr<HostSession>>& activate);
  // Stamp-checked re-read of the schedule store. External writers (CLI,
  // other hosts) change the file under us; the tick must see their runs.
  // Callers hold mutex_.
  bool RefreshScheduleCacheLocked();
  ScheduleTick TickSchedules();
  std::vector<std::string> InvalidationPaths(
      const std::vector<std::string>& projects) const;
  json Metadata(const HostSession& session) const;
  json LiveSnapshot(const HostSession& session) const;
  std::shared_ptr<HostSession> StartScheduledRun(const json& run);
  bool ActivateLocked(const std::shared_ptr<HostSession>& session,
                      std::string& error, std::unique_lock<std::mutex>& lock,
                      bool create);
  // Graceful binary-upgrade recycle: when the executable on disk is newer
  // than the attached worker's spawn record, close it and report true so
  // the caller spawns fresh. False keeps the worker: fresh record, or a
  // worker that ignores close (fail open, retried on the next touch).
  bool RecycleStaleWorkerLocked(const std::shared_ptr<HostSession>& session,
                                std::unique_lock<std::mutex>& lock);
  void Received(HostSession* session, json frame);
  void DeactivateLocked(HostSession& session);
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_HOST_H_
