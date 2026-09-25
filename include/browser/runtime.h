// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_BROWSER_RUNTIME_H_
#define UAGENT_INCLUDE_BROWSER_RUNTIME_H_

#include <sys/types.h>

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

 private:
  bool Start(std::string& error, bool profile_setup = false);
  void Stop(bool preserve_lease = false);
  json Call(const std::string& method, const json& parameters = json::object(),
            const std::string& session = {});
  bool SelectPage(std::string& error);
  bool AttachPage(const std::string& target, std::string& error);
  bool Agent(const json& command, std::string& error);
  json Status(bool include_page = true);
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
  uint64_t generation_ = 0;
  // An agent call retries while the human drives; each retry extends this.
  int64_t agent_waiting_until_ms_ = 0;
};

}  // namespace uagent::browser
#endif
