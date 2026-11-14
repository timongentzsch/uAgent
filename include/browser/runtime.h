// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_BROWSER_RUNTIME_H_
#define UAGENT_INCLUDE_BROWSER_RUNTIME_H_
// The browser service's runtime: Chrome and its virtual display, the page
// operations, and who holds control.

#include <sys/types.h>

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent::browser {

// Owned by the browser service process. Every operation and lease transition
// is serialized, so a human takeover cannot race an agent input command.
class Runtime {
 public:
  explicit Runtime(std::string directory = {}, uint64_t incarnation = 0);
  ~Runtime();
  json Execute(const json& command);
  void Shutdown();
  // Stops Chrome and Xvnc after `limit` idle, unless a human holds the browser.
  void StopIfIdle(std::chrono::minutes limit);

 private:
  bool Start(std::string& error, bool profile_setup = false);
  void Stop(bool preserve_lease = false);
  json Call(const std::string& method, const json& parameters = json::object(),
            const std::string& session = {});
  // Runtime.evaluate on the attached page, in the isolated world kWorld.
  json Evaluate(json parameters);
  // Writes a command without waiting for its reply; its id, or -1.
  int64_t Send(const std::string& method, const json& parameters,
               const std::string& session);
  // Why an action approved for one site may not run on the page now, or null.
  json Elsewhere(const json& command, std::string url);
  // What an operation does while the page shows a dialog.
  json Dialog(const std::string& op, const json& command);
  // The attached page's open dialog ({type, message, answer}), or null.
  json* OpenDialog();
  bool SelectPage(std::string& error);
  bool AttachPage(std::string target, std::string& error);
  // Draws a tab at the viewer's chosen page size, as Chrome's device mode.
  void Emulate(const std::string& session);
  bool Agent(const json& command, std::string& error);
  json Status(bool include_page = true);
  json Targets();
  json PageTargets();
  bool ClearHandover();
  json Control(const std::string& op, const json& command);
  json Act(const std::string& op, const json& command);
  json Probe();
  // The page as text, with the screenshot when `image` asks for it.
  json Observe(bool image);
  // A page of the host's preview (`url`) drawn at a size in a throwaway
  // context: no profile, no cookies, no agent's tab. `read` is given the
  // tab's session, and its answer is the answer.
  json Apart(const json& command,
             const std::function<json(const std::string&)>& read);
  // What `promise` comes to on a session's page once it has loaded, or null
  // when it has not within `ms`.
  json Awaited(const std::string& session, const std::string& promise, int ms);
  // Such a page as a PNG or PDF.
  json Capture(const json& command);
  // What such a page says is wrong with it, drawn at the artboard it states.
  json Problems(const json& command);
  // More of the page's text than a look carries, or the lines that match.
  json Read(const json& command);
  // What a session's page logged and threw, and what the attached one
  // asked of the network.
  json Console(const std::string& session, const std::string& find);
  json Network(const json& command);
  // Keeps a tab's request as Chrome tells of it.
  void Requested(const std::string& session, const std::string& event,
                 const json& params);
  // The downloads that ended since the last time they were told.
  json Finished();
  bool SaveHandover() const;
  bool SaveProfiles() const;
  std::string ProfilePath() const;
  json SwitchProfile(const std::string& id);

  struct Profile {
    std::string id;
    std::string name;
  };

  std::string directory_;
  std::string RfbSocket() const;

  pid_t vnc_pid_ = -1;
  pid_t chrome_pid_ = -1;
  Fd profile_lock_, cdp_in_, cdp_out_;
  int64_t next_id_ = 0;
  int view_width_ = 0, view_height_ = 0;
  std::string cdp_buffer_;
  std::string target_, page_session_, observation_;
  // The looks at this tab's document, the latest last. A number of any of
  // them names what that look numbered, so one look serves several actions.
  std::vector<std::string> looks_;
  std::string agent_session_, interaction_, viewer_;
  std::vector<Profile> profiles_{{"default", "Default"}};
  std::string selected_profile_ = "default";
  std::string profile_error_;
  std::string mode_ = "idle";
  // The page size the viewer chose ("" is the window's own), and the
  // window's page area, measured once, that a larger size is fitted into.
  std::string viewport_;
  int window_width_ = 0, window_height_ = 0;
  // How much smaller than its size that page is drawn.
  double input_scale_ = 1;
  bool profile_setup_ = false;
  bool loading_ = false;
  // Each tab's one attachment, and the dialog each session's page shows. A
  // dialog stops its page until it closes, and Chrome tells of it once.
  std::map<std::string, std::string> sessions_;
  std::map<std::string, json> dialogs_;
  // The agent's own navigation is under way: the page's plea to stay is
  // what the agent already chose against.
  bool leaving_ = false;
  // The file field whose held-back chooser a click opened, the downloads by
  // id until a probe reports them finished, and the folder they go to.
  int64_t chooser_ = 0;
  json downloads_ = json::object();
  std::string download_path_;
  // A session and what its page logged, while a console read gathers it.
  std::pair<std::string, std::vector<std::string>>* logged_ = nullptr;
  // The throwaway tab's session while one is open.
  std::string apart_;
  // Each tab's requests since its document began, by session, latest last.
  std::map<std::string, std::vector<json>> requests_;
  // A drag Chrome held back from the page, to be dropped where it ends.
  json dragged_;
  std::chrono::steady_clock::time_point used_ =
      std::chrono::steady_clock::now();
  // The browser changed hands and its holder has not yet opened or looked
  // at a page: what it remembers of the page is another conversation's.
  bool moved_ = false;
  std::string last_holder_;
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
