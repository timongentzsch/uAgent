// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_WEB_MASTER_INTERNAL_H_
#define UAGENT_SRC_WEB_MASTER_INTERNAL_H_
// Module-private split of the web host. The Master class is declared here and
// defined across master.cc (startup, routes, settings), master_auth.cc
// (pairing and devices), master_command.cc (commands and receipts),
// master_stream.cc (snapshots, events, uploads, assets), master_files.cc
// (the files in a conversation's folder) and master_terminal.cc (terminals).

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
#include "include/web/terminal.h"

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
  void RegisterApiRoutes();
  void RegisterBrowserRoutes();
  void RegisterTerminalRoutes();
  // The folders' shells, as every device lists them; null with the terminal
  // turned off.
  json TerminalList() const;
  void TerminalSocket(const Request& request, httplib::ws::WebSocket& socket);
  bool RegisterAssetRoutes(std::string& error);
  const std::string* ExpectedOrigin(const Request& request) const;
  static std::string ConfiguredDetail();
  void PublishSettings();
  std::string Pair() UAGENT_REQUIRES(mutex_);
  json PublicDevices() const UAGENT_REQUIRES(mutex_);
  void LoadDevices();
  bool SaveDevices() const UAGENT_REQUIRES(mutex_);
  std::string DeviceId(const Request& request) const UAGENT_REQUIRES(mutex_);
  void Authenticate(const Request& request, Response& response);
  void Snapshot(const Request&, Response&);
  void Search(const Request&, Response&);
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
  // One command as its handler sees it, and what the handler answers.
  struct CommandCall {
    json command;
    const std::string& device;
    const std::string& request_id;
    Response& response;
    json outcome;
    std::string error{};
    // The runtime request that will settle a receipt left pending.
    std::string worker_request{};
    // What to answer instead when the device no longer authenticates
    // after a handler that ran without mutex_.
    const char* revoked = nullptr;
  };
  // Handlers of Command's table. Only those that change the devices run
  // under mutex_; the rest run without it, and Command takes it back.
  void RevokeDevice(CommandCall& call) UAGENT_REQUIRES(mutex_);
  void TestNotification(CommandCall& call);
  void BrowserCommand(CommandCall& call);
  void ManagementCommand(CommandCall& call);
  void SpaceCommand(CommandCall& call);
  void PinsCommand(CommandCall& call);
  void RestartConversations(CommandCall& call);
  void RestartHost(CommandCall& call);
  void ConfigCommand(CommandCall& call);
  void TerminalCommand(CommandCall& call);
  void SessionCommand(CommandCall& call);
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
  // Null while web.terminal is off.
  std::unique_ptr<Terminals> terminals_;
  std::atomic<size_t> terminal_sockets_{0};
};

}  // namespace uagent::web::master

#endif  // UAGENT_SRC_WEB_MASTER_INTERNAL_H_
