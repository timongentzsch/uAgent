// Copyright 2026 Timon Gentzsch

#include "include/browser/runtime.h"

#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/browser/browser.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/private_store.h"
#include "include/tools/files.h"
#include "include/transport/session.h"
#include "src/browser/runtime_internal.h"

namespace uagent::browser {
namespace {
constexpr size_t kProfilesBytes = size_t{64} * 1024;
pid_t Launch(const std::vector<std::string>& arguments,
             posix_spawn_file_actions_t* actions = nullptr,
             const std::vector<std::string>& environment = {}) {
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  // Its own process group: Chrome's renderers and helpers stop with it.
  posix_spawnattr_t group;
  posix_spawnattr_init(&group);
  posix_spawnattr_setflags(&group, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&group, 0);
  pid_t pid = -1;
  std::vector<std::string> values;
  for (char** at = ProcessEnvironment(); *at; ++at) {
    std::string value(*at);
    bool replaced = false;
    for (const auto& setting : environment) {
      if (value.starts_with(setting.substr(0, setting.find('=') + 1))) {
        replaced = true;
        break;
      }
    }
    if (!replaced) values.push_back(std::move(value));
  }
  values.insert(values.end(), environment.begin(), environment.end());
  std::vector<char*> envp;
  envp.reserve(values.size() + 1);
  for (auto& value : values) envp.push_back(value.data());
  envp.push_back(nullptr);
  const int status =
      posix_spawnp(&pid, argv[0], actions, &group, argv.data(), envp.data());
  posix_spawnattr_destroy(&group);
  return status == 0 ? pid : -1;
}
void Terminate(pid_t& pid) {
  if (pid <= 0) return;
  kill(-pid, SIGTERM);
  if (!ReapPidFor(pid, nullptr,
                  std::chrono::milliseconds(kChildShutdownGraceMs))) {
    kill(-pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
  // Whatever the leader left behind.
  kill(-pid, SIGKILL);
  pid = -1;
}
constexpr auto kLeaseQuiet = std::chrono::minutes(3);
// The first X display no server holds, as ":N": other browser services on this
// machine run their own, and a second server on a held number fails to start.
std::string FreeDisplay() {
  for (int number = 99;; ++number) {
    const std::string name = std::to_string(number);
    if (access(("/tmp/.X" + name + "-lock").c_str(), F_OK) != 0 &&
        access(("/tmp/.X11-unix/X" + name).c_str(), F_OK) != 0) {
      return ":" + name;
    }
  }
}
// Where the sessions keep their leases. The service runs with its own HOME,
// so the host that started it names the directory.
std::string SessionRuntimeDir() {
  const char* named = getenv("UAGENT_INTERNAL_RUNTIME_DIR");
  return named && *named ? named : RuntimeDir();
}
}  // namespace

std::string CdError(const json& value) {
  const json* object = JsonObject(value, "error");
  return object ? JsonValue(*object, "message", "Chrome rejected action")
                : JsonValue(value, "error", "");
}

bool Alive(pid_t& pid) {
  if (pid <= 0) return false;
  pid_t done = waitpid(pid, nullptr, WNOHANG);
  if (done == pid || (done < 0 && errno == ECHILD)) {
    pid = -1;
    return false;
  }
  return kill(pid, 0) == 0;
}

std::string ProfileName(std::string name) {
  auto space = [](char ch) {
    return std::isspace(static_cast<unsigned char>(ch));
  };
  size_t first = 0, last = name.size();
  while (first < last && space(name[first])) ++first;
  while (last > first && space(name[last - 1])) --last;
  if (last == first || last - first > 256) return {};
  name = name.substr(first, last - first);
  if (std::any_of(name.begin(), name.end(), [](char value) {
        unsigned char ch = static_cast<unsigned char>(value);
        return ch < 32 || ch == 127;
      })) {
    return {};
  }
  return name;
}

std::string Runtime::RfbSocket() const { return directory_ + "/display.sock"; }

Runtime::~Runtime() { Stop(); }
void Runtime::Shutdown() { Stop(); }

void Runtime::StopIfIdle(std::chrono::minutes limit) {
  // A handover whose conversation no longer runs has nobody to hand back to.
  if (!interaction_.empty() &&
      FileLease::OwnerGone(
          session::SocketPathForId(agent_session_, SessionRuntimeDir()) +
          ".lock")) {
    ClearHandover();
  }
  if (chrome_pid_ <= 0 || mode_ == "human" || !interaction_.empty() ||
      std::chrono::steady_clock::now() - used_ < limit) {
    return;
  }
  Stop(true);
}

bool Runtime::ClearHandover() {
  std::string previous_interaction = std::exchange(interaction_, "");
  std::string previous_session = std::exchange(agent_session_, "");
  const std::string previous_mode =
      std::exchange(mode_, viewer_.empty() ? "idle" : "human");
  observation_.clear();
  if (SaveHandover()) return true;
  interaction_ = std::move(previous_interaction);
  agent_session_ = std::move(previous_session);
  mode_ = previous_mode;
  return false;
}

// Chrome's targets, or none when it does not answer.
json Runtime::Targets() {
  const json response = Call("Target.getTargets");
  const json* result = JsonObject(response, "result");
  const json* infos = result ? JsonArray(*result, "targetInfos") : nullptr;
  return infos ? *infos : json::array();
}

Runtime::Runtime(std::string directory, uint64_t incarnation)
    : directory_(directory.empty() ? DataDirectory() : std::move(directory)) {
  display_ = generation_ = incarnation;
  std::string bytes, error;
  const std::string profiles_path = directory_ + "/profiles.json";
  struct stat info{};
  if (lstat(profiles_path.c_str(), &info) == 0) {
    if (!ReadRegularFile(profiles_path, kProfilesBytes, bytes, error)) {
      profile_error_ = "cannot read Chrome profiles";
    } else {
      json saved = json::parse(bytes, nullptr, false);
      const json* entries = JsonArray(saved, "profiles");
      std::string selected = JsonValue(saved, "selected", "");
      std::vector<Profile> loaded;
      std::set<std::string> ids;
      bool valid = entries != nullptr;
      if (entries) {
        for (const auto& entry : *entries) {
          std::string id = JsonValue(entry, "id", "");
          std::string name = JsonValue(entry, "name", "");
          if ((id != "default" && !session::OpaqueId(id)) ||
              ProfileName(name) != name || !ids.insert(id).second) {
            valid = false;
            break;
          }
          loaded.push_back({id, name});
        }
      }
      if (!valid || loaded.empty() || loaded.front().id != "default" ||
          ids.count(selected) == 0) {
        profile_error_ = "invalid Chrome profiles; restore profiles.json";
      } else {
        profiles_ = std::move(loaded);
        selected_profile_ = std::move(selected);
      }
    }
  } else if (errno != ENOENT) {
    profile_error_ = "cannot inspect Chrome profiles";
  }
  {
    const json saved = ReadJsonFile(directory_ + "/handover.json", 4096);
    std::string session = JsonValue(saved, "session_id", "");
    std::string interaction = JsonValue(saved, "interaction_id", "");
    if (session::OpaqueId(session) && session::OpaqueId(interaction)) {
      agent_session_ = session;
      interaction_ = interaction;
      mode_ = "human";
    }
  }
}

bool Runtime::SaveProfiles() const {
  json entries = json::array();
  for (const auto& profile : profiles_) {
    entries.push_back({{"id", profile.id}, {"name", profile.name}});
  }
  std::string bytes =
      JsonDump({{"selected", selected_profile_}, {"profiles", entries}});
  return bytes.size() <= kProfilesBytes &&
         ToolWritePrivateFile(directory_ + "/profiles.json", bytes).Ok();
}

std::string Runtime::ProfilePath() const {
  std::string base = directory_;
  return selected_profile_ == "default"
             ? base + "/profile"
             : base + "/profiles/" + selected_profile_;
}

bool Runtime::SaveHandover() const {
  std::string path = directory_ + "/handover.json";
  if (interaction_.empty()) return unlink(path.c_str()) == 0 || errno == ENOENT;
  return ToolWritePrivateFile(path,
                              JsonDump({{"session_id", agent_session_},
                                        {"interaction_id", interaction_}}))
      .Ok();
}

void Runtime::Stop(bool preserve_lease) {
  std::string session = preserve_lease ? agent_session_ : "";
  std::string interaction = preserve_lease ? interaction_ : "";
  cdp_in_.Reset();
  cdp_out_.Reset();
  Terminate(chrome_pid_);
  Terminate(vnc_pid_);
  profile_lock_.Reset();
  unlink(RfbSocket().c_str());
  target_.clear();
  page_session_.clear();
  sessions_.clear();
  dialogs_.clear();
  observation_.clear();
  looks_.clear();
  known_targets_.clear();
  action_target_.clear();
  view_width_ = view_height_ = 0;
  cdp_buffer_.clear();
  profile_setup_ = false;
  agent_session_ = std::move(session);
  interaction_ = std::move(interaction);
  viewer_.clear();
  mode_ = !interaction_.empty()     ? "human"
          : !agent_session_.empty() ? "agent"
                                    : "idle";
  ++generation_;
}

bool Runtime::Start(std::string& error, bool profile_setup) {
  // Display allocation and binding must be one operation across workers.
  static std::mutex launch_mutex;
  std::lock_guard launch_lock(launch_mutex);
  if (!profile_error_.empty()) {
    error = profile_error_;
    return false;
  }
  if (Alive(chrome_pid_) && Alive(vnc_pid_)) return true;
  Stop(true);
  std::string base = directory_;
  std::string profile = ProfilePath();
  if (!EnsureDataDirectory(profile)) {
    error = "cannot create persistent Chrome profile";
    return false;
  }
  profile_lock_.Reset(open((base + "/profile.lock").c_str(),
                           O_CREAT | O_RDWR | O_CLOEXEC, kPrivateFileMode));
  if (!profile_lock_ || flock(profile_lock_.Get(), LOCK_EX | LOCK_NB) != 0) {
    error = "Chrome profile is in use";
    profile_lock_.Reset();
    return false;
  }
  // Docker recreates the container with a new hostname. Chrome's singleton
  // symlinks retain the old hostname and reject the otherwise valid profile.
  // Only the lock holder may clear them, after its own Chrome has exited.
  for (const char* name :
       {"SingletonLock", "SingletonCookie", "SingletonSocket"}) {
    unlink((profile + "/" + name).c_str());
  }
  std::string authority = base + "/Xauthority";
  std::string cookie = RandomToken(16);
  if (cookie.empty()) {
    error = "cannot generate X authorization";
    return false;
  }
  const std::string display = FreeDisplay();
  // Entries of displays that earlier runs used are of no use.
  unlink(authority.c_str());
  pid_t xauth = Launch({"xauth", "-f", authority, "add", display, ".", cookie});
  int xauth_status = 0;
  if (xauth < 0 || waitpid(xauth, &xauth_status, 0) < 0 ||
      !WIFEXITED(xauth_status) || WEXITSTATUS(xauth_status) != 0) {
    error = "cannot create X authorization";
    return false;
  }
  chmod(authority.c_str(), kPrivateFileMode);
  const std::vector<std::string> environment = {
      "DISPLAY=" + display, "XAUTHORITY=" + authority, "HOME=" + directory_};
  unlink(RfbSocket().c_str());
  vnc_pid_ = Launch({"Xtigervnc",
                     display,
                     "-geometry",
                     "1280x800",
                     "-depth",
                     "24",
                     "-auth",
                     authority,
                     "-rfbunixpath",
                     RfbSocket(),
                     "-rfbunixmode",
                     "0600",
                     "-rfbport",
                     "-1",
                     "-SecurityTypes",
                     "None",
                     "-FrameRate",
                     "15",
                     "-AcceptCutText=1",
                     "-SendCutText=1",
                     "-SendPrimary=0",
                     "-nolisten",
                     "tcp"});
  if (vnc_pid_ < 0) {
    error = "cannot launch Xtigervnc";
    return false;
  }
  for (int waited = 0;
       waited < kRfbSocketWaitMs && access(RfbSocket().c_str(), F_OK);
       waited += kPollTickMs) {
    poll(nullptr, 0, kPollTickMs);
  }
  if (access(RfbSocket().c_str(), F_OK)) {
    error = "Xtigervnc did not create its Unix display socket";
    Stop(true);
    return false;
  }
  // Manual profile setup uses ordinary Chrome, without a debugging endpoint.
  // Both modes own the same profile exclusively and restore its saved tabs.
  const std::string no_downloads =
      "--disable-features=OptimizationGuideModelDownloading,"
      "OptimizationHintsFetching";
  std::vector<std::string> chrome = {
      "google-chrome-stable", "--user-data-dir=" + profile, "--no-first-run",
      "--no-default-browser-check",
      // Cover the whole 1280x800 display. With no window manager Chrome
      // restores its saved placement unless pinned, and it shrinks a window
      // that would exactly fill the screen by one pixel (1279x799), leaving
      // a black line; one pixel past the edge is kept as asked.
      "--window-position=0,0", "--window-size=1281,801", "--ozone-platform=x11",
      "--password-store=basic", "--restore-last-session",
      // A restart of the host stops Chrome without a goodbye; its offer to
      // restore would cover the page.
      "--hide-crash-restore-bubble",
      // Keep the profile small: no downloaded components or on-device
      // models (most of a 258 MB profile), and a bounded page cache.
      "--disable-component-update", no_downloads, "--disk-cache-size=67108864",
      // A person's browser, not a test runner: the debugging pipe would
      // mark every page navigator.webdriver, and with no GPU Chrome offers
      // pages no WebGL. Sites read both as a bot and refuse the user's
      // own errands (searches, bookings).
      "--disable-blink-features=AutomationControlled",
      "--enable-unsafe-swiftshader"};
  if (profile_setup) {
    chrome_pid_ = Launch(chrome, nullptr, environment);
    if (chrome_pid_ < 0) {
      error = "cannot launch Google Chrome Stable";
      Stop(true);
      return false;
    }
    profile_setup_ = true;
    ++display_;
    return true;
  }
  int to_chrome[2], from_chrome[2];
  if (pipe(to_chrome) != 0) {
    error = "cannot create Chrome control pipes";
    Stop(true);
    return false;
  }
  if (pipe(from_chrome) != 0) {
    close(to_chrome[0]);
    close(to_chrome[1]);
    error = "cannot create Chrome control pipes";
    Stop(true);
    return false;
  }
  Fd parent_write(to_chrome[1]), chrome_read(to_chrome[0]);
  Fd parent_read(from_chrome[0]), chrome_write(from_chrome[1]);
  if (!CloseOnExec(parent_write.Get()) || !CloseOnExec(chrome_read.Get()) ||
      !CloseOnExec(parent_read.Get()) || !CloseOnExec(chrome_write.Get())) {
    error = "cannot isolate Chrome control pipes";
    Stop(true);
    return false;
  }
  Fd child_read(fcntl(chrome_read.Get(), F_DUPFD_CLOEXEC, 5));
  Fd child_write(fcntl(chrome_write.Get(), F_DUPFD_CLOEXEC, 5));
  if (!child_read || !child_write) {
    error = "cannot duplicate Chrome control pipes";
    Stop(true);
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, child_read.Get(), 3);
  posix_spawn_file_actions_adddup2(&actions, child_write.Get(), 4);
  posix_spawn_file_actions_addclose(&actions, child_read.Get());
  posix_spawn_file_actions_addclose(&actions, child_write.Get());
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  // Chrome's sandbox stays enabled. This is a private profile.
  chrome.emplace_back("--remote-debugging-pipe");
  chrome_pid_ = Launch(chrome, &actions, environment);
  posix_spawn_file_actions_destroy(&actions);
  chrome_read.Reset();
  chrome_write.Reset();
  if (chrome_pid_ < 0) {
    error = "cannot launch Google Chrome Stable";
    Stop(true);
    return false;
  }
  cdp_in_ = std::move(parent_write);
  cdp_out_ = std::move(parent_read);
  if (!SelectPage(error)) {
    if (!Alive(chrome_pid_)) {
      error =
          "Chrome exited during startup; check its sandbox and container logs";
    }
    Stop(true);
    return false;
  }
  ++display_;
  return true;
}

int64_t Runtime::Send(const std::string& method, const json& parameters,
                      const std::string& session) {
  const int64_t id = ++next_id_;
  json command = {{"id", id}, {"method", method}, {"params", parameters}};
  if (!session.empty()) command["sessionId"] = session;
  std::string packet = JsonDump(command);
  packet.push_back('\0');
  return WriteAllWithin(cdp_in_.Get(), packet, kCdpIoMs) ? id : -1;
}

json Runtime::Call(const std::string& method, const json& parameters,
                   const std::string& session) {
  if (!cdp_in_ || !cdp_out_) return {{"error", "Chrome is stopped"}};
  // A page held by a dialog answers nothing but the dialog's own answer.
  const bool held = !session.empty() && session == page_session_ &&
                    method != "Page.handleJavaScriptDialog";
  if (held && OpenDialog()) return {{"dialog", true}};
  const int64_t id = Send(method, parameters, session);
  if (id < 0) return {{"error", "Chrome control pipe is stuck or closed"}};
  for (;;) {
    size_t end = cdp_buffer_.find('\0');
    if (end != std::string::npos) {
      json response = json::parse(
          cdp_buffer_.begin(),
          cdp_buffer_.begin() + static_cast<ptrdiff_t>(end), nullptr, false);
      cdp_buffer_.erase(0, end + 1);
      if (JsonValue(response, "id", int64_t{-1}) == id) return response;
      // Events have no reply id. Only the main frame's loading state is
      // kept: it spans a click's pending request, before the old document
      // is replaced. A page's main frame id is its target id.
      const std::string event = JsonValue(response, "method", "");
      const json* params = JsonObject(response, "params");
      if (params && JsonValue(*params, "frameId", "") == target_) {
        if (event == "Page.frameStartedLoading" ||
            event == "Page.frameStartedNavigating") {
          loading_ = true;
        }
        if (event == "Page.frameStoppedLoading") loading_ = false;
      }
      // Its tab was closed by hand, or Chrome replaced it: nothing is
      // attached until a page is chosen again.
      if (event == "Target.detachedFromTarget" && params) {
        const std::string gone = JsonValue(*params, "sessionId", "");
        std::erase_if(sessions_,
                      [&](const auto& tab) { return tab.second == gone; });
        dialogs_.erase(gone);
        if (gone == page_session_) page_session_.clear();
      }
      const std::string from = JsonValue(response, "sessionId", "");
      if (params && event == "Page.javascriptDialogOpening") {
        // The page's plea to stay while the agent leaves it is answered at
        // once; its reply is dropped like any other not waited for.
        if (leaving_ && from == page_session_ &&
            JsonValue(*params, "type", "") == "beforeunload") {
          Send("Page.handleJavaScriptDialog", {{"accept", true}}, from);
        } else {
          dialogs_[from] = {{"type", JsonValue(*params, "type", "")},
                            {"message", JsonValue(*params, "message", "")},
                            {"url", JsonValue(*params, "url", "")}};
        }
      }
      if (event == "Page.javascriptDialogClosed") dialogs_.erase(from);
      if (params && event == "Page.fileChooserOpened" &&
          from == page_session_) {
        chooser_ = JsonValue(*params, "backendNodeId", int64_t{0});
      }
      if (params && event == "Browser.downloadWillBegin") {
        downloads_[JsonValue(*params, "guid", "")] = {
            {"name", JsonValue(*params, "suggestedFilename", "")},
            {"state", "inProgress"}};
      }
      if (params && event == "Browser.downloadProgress") {
        const std::string guid = JsonValue(*params, "guid", "");
        if (downloads_.contains(guid)) {
          downloads_[guid]["state"] = JsonValue(*params, "state", "");
        }
      }
      // What the page owed is never coming while the dialog is open; an
      // input it cut short has still happened.
      if (held && OpenDialog()) return {{"dialog", true}};
      continue;
    }
    if (cdp_buffer_.size() > size_t{16} * 1024 * 1024) {
      return {{"error", "Chrome response exceeded limit"}};
    }
    pollfd ready{cdp_out_.Get(), POLLIN, 0};
    if (poll(&ready, 1, kCdpIoMs) <= 0) {
      return {{"error", "Chrome read timed out"}};
    }
    char bytes[16384];
    ssize_t n = read(cdp_out_.Get(), bytes, sizeof(bytes));
    if (n <= 0) return {{"error", "Chrome control pipe closed"}};
    cdp_buffer_.append(bytes, static_cast<size_t>(n));
  }
}

bool Runtime::SelectPage(std::string& error) {
  for (const json& item : Targets()) {
    if (JsonValue(item, "type", "") == "page") {
      target_ = JsonValue(item, "targetId", "");
      break;
    }
  }
  if (target_.empty()) {
    json created = Call("Target.createTarget", {{"url", "about:blank"}});
    if (const json* result = JsonObject(created, "result")) {
      target_ = JsonValue(*result, "targetId", "");
    }
  }
  if (target_.empty()) {
    error = "Chrome did not expose a page";
    return false;
  }
  return AttachPage(target_, error);
}

json* Runtime::OpenDialog() {
  auto open = dialogs_.find(page_session_);
  return open == dialogs_.end() ? nullptr : &open->second;
}

// `target` by value: SelectPage passes target_ itself, which is replaced here.
bool Runtime::AttachPage(std::string target, std::string& error) {
  observation_.clear();
  looks_.clear();
  view_width_ = view_height_ = 0;
  loading_ = false;
  if (auto known = sessions_.find(target); known != sessions_.end()) {
    target_ = std::move(target);
    page_session_ = known->second;
    return true;
  }
  json attached =
      Call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}});
  std::string attached_session;
  if (const json* result = JsonObject(attached, "result")) {
    attached_session = JsonValue(*result, "sessionId", "");
  }
  if (attached_session.empty()) {
    error = CdError(attached).empty() ? "Chrome did not attach the page"
                                      : CdError(attached);
    return false;
  }
  // Adopt the target before enabling Page events, so a navigation already
  // pending in a fresh popup is reported against it and counts as loading.
  const std::string previous = std::exchange(target_, target);
  if (auto enabled =
          CdError(Call("Page.enable", json::object(), attached_session));
      !enabled.empty()) {
    target_ = previous;
    error = enabled;
    return false;
  }
  if (auto installed =
          CdError(Call("Page.addScriptToEvaluateOnNewDocument",
                       {{"source", kSecretScript}, {"runImmediately", true}},
                       attached_session));
      !installed.empty()) {
    target_ = previous;
    error = installed;
    return false;
  }
  page_session_ = sessions_[target_] = std::move(attached_session);
  return true;
}

json Runtime::Status(bool include_page) {
  bool running = Alive(chrome_pid_) && Alive(vnc_pid_);
  json profiles = json::array();
  for (const auto& profile : profiles_) {
    profiles.push_back({{"id", profile.id}, {"name", profile.name}});
  }
  json result = {{"ok", true},
                 {"running", running},
                 {"mode", mode_},
                 {"waiting", mode_ == "human" && !interaction_.empty()},
                 {"session_id", agent_session_},
                 {"interaction_id", interaction_},
                 {"viewer", viewer_},
                 {"profile_id", selected_profile_},
                 {"profile_setup", profile_setup_},
                 {"profiles", profiles},
                 {"generation", generation_},
                 {"display", display_}};
  if (!profile_error_.empty()) result["error"] = profile_error_;
  if (running && include_page && !profile_setup_) {
    for (const json& info : Targets()) {
      if (JsonValue(info, "targetId", "") == target_) {
        std::string url = JsonValue(info, "url", "");
        result["url"] = url.substr(0, url.find_first_of("?#"));
        result["title"] = JsonValue(info, "title", "");
        break;
      }
    }
  }
  return result;
}

bool Runtime::Agent(const json& command, std::string& error) {
  std::string session = JsonValue(command, "session_id", "");
  if (!session::OpaqueId(session)) {
    error = "browser needs a valid session";
    return false;
  }
  if (mode_ == "human") {
    error = kHumanControls;
    return false;
  }
  // A holder that has asked nothing of the browser for a while has ended
  // without giving it back, or waits on something else: it is the next
  // conversation's.
  const auto now = std::chrono::steady_clock::now();
  if (!agent_session_.empty() && agent_session_ != session &&
      now - agent_used_ < kLeaseQuiet) {
    error = kLeasedElsewhere;
    return false;
  }
  if (last_holder_ != session) {
    observation_.clear();
    moved_ = true;
    last_holder_ = session;
  }
  agent_session_ = session;
  agent_used_ = now;
  mode_ = "agent";
  return true;
}

json Runtime::SwitchProfile(const std::string& id) {
  if (id == selected_profile_) return Status();
  const std::string previous = selected_profile_;
  const std::string controller = viewer_;
  const bool profile_setup = profile_setup_;
  const bool restart = !controller.empty();
  if (restart) Stop(true);
  selected_profile_ = id;
  if (!SaveProfiles()) {
    selected_profile_ = previous;
    if (restart) {
      std::string restart_error;
      if (Start(restart_error, profile_setup)) {
        viewer_ = controller;
        mode_ = "human";
        ++generation_;
      } else {
        return {{"error",
                 "cannot save Chrome profile selection; previous "
                 "profile could not restart: " +
                     restart_error}};
      }
    }
    return {{"error", "cannot save Chrome profile selection"}};
  }
  std::string error;
  if (restart && !Start(error, profile_setup)) {
    selected_profile_ = previous;
    bool selection_restored = SaveProfiles();
    std::string rollback_error;
    if (Start(rollback_error, profile_setup)) {
      viewer_ = controller;
      mode_ = "human";
      ++generation_;
    } else {
      error += "; previous profile could not restart: " + rollback_error;
    }
    if (!selection_restored) {
      error += "; previous profile selection could not be restored";
    }
    return {{"error", error}};
  }
  if (restart) {
    viewer_ = controller;
    mode_ = "human";
    ++generation_;
  }
  return Status();
}

json Runtime::Execute(const json& command) {
  const std::string op = JsonValue(command, "op", "");
  if (op == "ping") return {{"ok", true}};
  if (op == "status") return Status();
  if (op == "snapshot") return Status(false);
  // Watching is not use: only actions keep an idle Chrome running.
  if (op != "agent_status" && op != "viewer") {
    used_ = std::chrono::steady_clock::now();
  }
  if (json done = Control(op, command); !done.is_null()) {
    return done;
  }
  std::string error;
  if (mode_ == "human") return {{"error", kHumanControls}};
  if (!Start(error)) return {{"error", error}};
  if (!Agent(command, error)) return {{"error", error}};
  // Chrome's word that the attached tab is gone (closed by hand, or
  // replaced) is read with the next reply: ask for the tabs, then attach a
  // page again if none is.
  Targets();
  if (page_session_.empty()) {
    target_.clear();
    if (!SelectPage(error)) return {{"error", error}};
  }
  if (op == "request_human") {
    std::string interaction = JsonValue(command, "interaction_id", "");
    if (!session::OpaqueId(interaction)) {
      return {{"error", "invalid interaction"}};
    }
    interaction_ = interaction;
    std::string previous_viewer = viewer_;
    viewer_.clear();
    mode_ = "human";
    observation_.clear();
    if (!SaveHandover()) {
      interaction_.clear();
      viewer_ = std::move(previous_viewer);
      mode_ = "agent";
      return {{"error", "cannot persist browser handover"}};
    }
    ++generation_;
    return Status();
  }
  // A dialog that is open, or opens meanwhile, is what there is to report.
  if (OpenDialog() && op != "tabs") return Dialog(op, command);
  // Whatever failed meanwhile failed because the page could not answer.
  const auto reported = [&](json result) {
    if (const json* open = OpenDialog()) {
      result.erase("error");
      result["ok"] = true;
      result["dialog"] = *open;
    }
    return result;
  };
  if (op == "probe") return reported(Probe());
  // Whoever takes the browser after someone else starts from the page as it
  // is, not as it remembers it: keys and text go nowhere before that.
  // Only a look or an open that worked counts.
  if (moved_ && op != "observe" && op != "read" && op != "tabs" &&
      op != "open") {
    return {{"error",
             "another conversation used the browser since; open your page "
             "or observe before acting"}};
  }
  if (op == "observe") {
    json seen = Observe(JsonValue(command, "image", true));
    if (!seen.contains("error")) moved_ = false;
    return reported(seen);
  }
  if (op == "read") return reported(Read(command));
  if (op == "tabs") {
    json pages = PageTargets();
    if (pages.contains("error")) return pages;
    for (auto& page : pages) {
      page["selected"] = JsonValue(page, "id", "") == target_;
      page.erase("opener");
    }
    std::string requested = JsonValue(command, "target_id", "");
    if (!requested.empty()) {
      bool found = false;
      for (const auto& page : pages) {
        found |= JsonValue(page, "id", "") == requested;
      }
      if (!found) return {{"error", "tab is unavailable"}};
      if (JsonValue(command, "close", false)) {
        if (requested == target_) {
          return {{"error", "switch to another tab before closing this one"}};
        }
        json closed = Call("Target.closeTarget", {{"targetId", requested}});
        if (auto reason = CdError(closed); !reason.empty()) {
          return {{"error", reason}};
        }
        moved_ = false;
        return {{"ok", true}, {"closed", requested}, {"selected", target_}};
      } else if (requested != target_) {
        json activated =
            Call("Target.activateTarget", {{"targetId", requested}});
        if (auto reason = CdError(activated); !reason.empty()) {
          return {{"error", reason}};
        }
        if (!AttachPage(requested, error)) return {{"error", error}};
      }
    }
    moved_ = false;
    return {{"ok", true}, {"tabs", pages}, {"selected", target_}};
  }
  return reported(Act(op, command));
}

}  // namespace uagent::browser
