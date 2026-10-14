// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_WEB_MASTER_INTERNAL_H_
#define UAGENT_SRC_WEB_MASTER_INTERNAL_H_
// Module-private split of the web host. The Master class is declared here and
// defined across master.cc (startup, routes, settings), master_auth.cc
// (pairing and devices), master_command.cc (commands and receipts) and
// master_stream.cc (snapshots, events, uploads, assets).

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT

#include <httplib.h>

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

struct Device {
  std::string id, token, name;
  int64_t expires = 0;
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
  std::string Pair();
  json PublicDevices() const;
  void LoadDevices();
  bool SaveDevices() const;
  std::string DeviceId(const Request& request) const;
  void Authenticate(const Request& request, Response& response);
  void Snapshot(const Request&, Response&);
  void Command(const Request&, Response&);
  void Events(const Request&, Response&);
  void Upload(const Request&, Response&, const httplib::ContentReader&);
  void AssetRead(const Request&, Response&);

  WebOptions options_;
  std::string executable_, directory_, local_, local_authority_, origin_,
      authority_, epoch_, secret_, pair_;
  session::SessionHost host_;
  Clock::time_point pair_deadline_{}, auth_window_{};
  int auth_attempts_ = 0;
  httplib::Server server_;
  std::mutex mutex_;
  std::mutex settings_mutex_;
  FileStamp settings_ = SnapshotFile(SettingsPath());
  Pipe host_wake_;
  std::vector<Device> devices_;
  size_t sse_count_ = 0;
  struct Receipt {
    std::string fingerprint;
    json outcome;
    bool handling = true;
    std::string worker_request;
  };
  // A receipt still pending asks the host whether its worker has answered.
  void RefreshReceipt(Receipt& receipt, const std::string& request_id);
  json BrowserControl(const json& command, const std::string& device,
                      const std::string& request_id, std::string& error);
  std::map<std::string, Receipt> requests_;
  std::deque<std::string> request_order_;
  std::atomic<bool> stopping_{false};
  // Set by a restart from the settings: MasterMain re-execs after shutdown.
  std::atomic<bool> reexec_{false};
  std::unique_ptr<PushSender> push_;
  browser::ServiceProcess browser_process_;
  std::atomic<bool> viewer_active_{false};
};

}  // namespace uagent::web::master

#endif  // UAGENT_SRC_WEB_MASTER_INTERNAL_H_
