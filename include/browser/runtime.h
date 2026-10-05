// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_BROWSER_RUNTIME_H_
#define UAGENT_INCLUDE_BROWSER_RUNTIME_H_

#include <sys/types.h>

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent::browser {

// Owned by the browser service process. Every operation and lease transition
// is serialized, so a human takeover cannot race an agent input command.
class Runtime {
 public:
  Runtime();
  ~Runtime();
  json Execute(const json& command);
  void Shutdown();
  // Stops Chrome and Xvnc after `limit` without browser work, unless a human
  // holds or is asked for the browser. The next action starts them again.
  void StopIfIdle(std::chrono::minutes limit);

 private:
  bool Start(std::string& error, bool profile_setup = false);
  void Stop(bool preserve_lease = false);
  json Call(const std::string& method, const json& parameters = json::object(),
            const std::string& session = {});
  bool SelectPage(std::string& error);
  bool AttachPage(std::string target, std::string& error);
  bool Agent(const json& command, std::string& error);
  json Status(bool include_page = true);
  json Targets();
  json PageTargets();
  bool ClearHandover();
  json Probe();
  json Observe();
  bool SaveHandover() const;
  bool SaveProfiles() const;
  std::string ProfilePath() const;
  json SwitchProfile(const std::string& id);

  struct Profile {
    std::string id;
    std::string name;
  };

  pid_t vnc_pid_ = -1;
  pid_t chrome_pid_ = -1;
  Fd profile_lock_, cdp_in_, cdp_out_;
  int64_t next_id_ = 0;
  int view_width_ = 0, view_height_ = 0;
  std::string cdp_buffer_;
  std::string target_, page_session_, observation_;
  std::string agent_session_, interaction_, viewer_;
  std::vector<Profile> profiles_{{"default", "Default"}};
  std::string selected_profile_ = "default";
  std::string profile_error_;
  std::string mode_ = "idle";
  bool profile_setup_ = false;
  bool loading_ = false;
  std::chrono::steady_clock::time_point used_ =
      std::chrono::steady_clock::now();
  // When the conversation holding the browser last asked something of it.
  std::chrono::steady_clock::time_point agent_used_ = used_;
  // Bumps on every lease or tab change; guards observations.
  uint64_t generation_ = 0;
  // Bumps only when Chrome and Xvnc launch; a viewer stays connected across
  // control changes and reconnects only for a new display.
  uint64_t display_ = 0;
  // Page targets before the latest action and the tab that performed it, so
  // a probe can follow the one tab that action opened.
  std::set<std::string> known_targets_;
  std::string action_target_;
};

}  // namespace uagent::browser
#endif
