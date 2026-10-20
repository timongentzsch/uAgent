// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_WEB_MASTER_INTERNAL_H_
#define UAGENT_SRC_WEB_MASTER_INTERNAL_H_
// Module-private split of the web host. The Master class is declared here and
// defined across master.cc (startup, routes, settings), master_auth.cc
// (pairing and devices), master_command.cc (commands and receipts),
// master_stream.cc (snapshots, events, uploads, assets) and master_files.cc
// (the files in a conversation's folder).

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT

#include <httplib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "include/app/session_host.h"
#include "include/browser/browser.h"
#include "include/core/file_watch.h"
#include "include/core/settings_store.h"
#include "include/core/thread_annotations.h"
#include "include/core/time.h"
#include "include/web/protocol.h"
#include "include/web/push.h"

// The web host's private names live in uagent::web::master, so the helpers
// below (Reply, Error, ...) cannot clash with any other symbol in the binary.
namespace uagent::web::master {

using Clock = std::chrono::steady_clock;
using Request = httplib::Request;
using Response = httplib::Response;

// Read by the signal handler and by the delayed restart thread, written by
// Run(): a lock-free atomic is safe in both, where sig_atomic_t is not
// across threads.
extern std::atomic<int> shutdown_fd;

void Reply(Response& response, const json& value, int status = 200);
void Error(Response& response, const std::string& error, int status = 400);
bool EqualSecret(std::string_view a, std::string_view b);
// A file's bytes under a policy that lets none of it act on this origin:
// shown in the browser when `inline_view`, else saved as `name`.
void ServeFile(Response& response, std::string bytes, const std::string& mime,
               const std::string& name, bool inline_view);

// Browser instances "0" to "4", each watched by at most one viewer.
inline constexpr size_t kViewerInstances = 5;

struct Device {
  std::string id, token, name;
  int64_t expires = 0;
  bool Active() const { return expires > NowMillis(); }
};
class Master {
 public:
  Master(WebOptions options, std::string executable, std::string directory);

  bool Initialize(std::string& error);
  int Run();
  bool Reexec() const { return reexec_; }

 private:
  const std::string* ExpectedOrigin(const Request& request) const;
  static std::string ConfiguredVerbosity();
  void PublishSettings();
  std::string Pair() UAGENT_REQUIRES(mutex_);
  json PublicDevices() const UAGENT_REQUIRES(mutex_);
  void LoadDevices();
  bool SaveDevices() const UAGENT_REQUIRES(mutex_);
  std::string DeviceId(const Request& request) const UAGENT_REQUIRES(mutex_);
  void Authenticate(const Request& request, Response& response);
  void Snapshot(const Request&, Response&);
  void Command(const Request&, Response&);
  void Events(const Request&, Response&);
  void Upload(const Request&, Response&, const httplib::ContentReader&);
  void AssetRead(const Request&, Response&);
  void Files(const Request&, Response&);

  WebOptions options_;
  std::string executable_, directory_, local_, local_authority_, origin_,
      authority_, epoch_, secret_;
  std::string pair_ UAGENT_GUARDED_BY(mutex_);
  session::SessionHost host_;
  Clock::time_point pair_deadline_ UAGENT_GUARDED_BY(mutex_){};
  Clock::time_point auth_window_ UAGENT_GUARDED_BY(mutex_){};
  int auth_attempts_ UAGENT_GUARDED_BY(mutex_) = 0;
  httplib::Server server_;
  std::mutex mutex_;
  std::mutex settings_mutex_;
  FileStamp settings_ UAGENT_GUARDED_BY(settings_mutex_) =
      SnapshotFile(SettingsPath());
  Pipe host_wake_;
  std::vector<Device> devices_ UAGENT_GUARDED_BY(mutex_);
  size_t sse_count_ UAGENT_GUARDED_BY(mutex_) = 0;
  struct Receipt {
    std::string fingerprint;
    json outcome;
    bool handling = true;
    std::string worker_request;
  };
  // A receipt still pending asks the host whether its worker has answered.
  void RefreshReceipt(Receipt& receipt, const std::string& request_id)
      UAGENT_REQUIRES(mutex_);
  json BrowserControl(const json& command, const std::string& device,
                      const std::string& request_id, std::string& error);
  std::map<std::string, Receipt> requests_ UAGENT_GUARDED_BY(mutex_);
  std::deque<std::string> request_order_ UAGENT_GUARDED_BY(mutex_);
  std::atomic<bool> stopping_{false};
  // Set by a restart from the settings: MasterMain re-execs after shutdown.
  std::atomic<bool> reexec_{false};
  std::unique_ptr<PushSender> push_;
  browser::ServiceProcess browser_process_;
  std::array<std::atomic<bool>, kViewerInstances> viewer_active_{};
};

}  // namespace uagent::web::master

#endif  // UAGENT_SRC_WEB_MASTER_INTERNAL_H_
