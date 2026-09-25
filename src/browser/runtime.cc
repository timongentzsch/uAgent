// Copyright 2026 Timon Gentzsch

#include "include/browser/runtime.h"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/app/session.h"
#include "include/browser/browser.h"
#include "include/core/fs.h"
#include "include/core/platform.h"
#include "include/core/time.h"
#include "include/tools/files.h"

namespace uagent::browser {
namespace {
constexpr size_t kProfilesBytes = size_t{64} * 1024;

pid_t Launch(const std::vector<std::string>& arguments,
             posix_spawn_file_actions_t* actions = nullptr) {
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = -1;
  return posix_spawnp(&pid, argv[0], actions, nullptr, argv.data(),
                      ProcessEnvironment()) == 0
             ? pid
             : -1;
}

void Terminate(pid_t& pid) {
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  if (!ReapPidFor(pid, nullptr,
                  std::chrono::milliseconds(kChildShutdownGraceMs))) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
  pid = -1;
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

bool Coordinate(const json& command, const char* name, int& result) {
  result = JsonValue(command, name, -1);
  return result >= 0 && result <= 10000;
}

std::string CdError(const json& value) {
  if (value.contains("error") && value["error"].is_string()) {
    return value["error"].get<std::string>();
  }
  const json* object = JsonObject(value, "error");
  return object ? JsonValue(*object, "message", "Chrome rejected action") : "";
}

bool CloseOnExec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
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
}  // namespace

Runtime::~Runtime() { Stop(); }
void Runtime::Shutdown() { Stop(); }

Runtime::Runtime() {
  std::string bytes, error;
  const std::string profiles_path = DataDirectory() + "/profiles.json";
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
  if (ReadRegularFile(DataDirectory() + "/handover.json", 4096, bytes, error)) {
    json saved = json::parse(bytes, nullptr, false);
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
         ToolWritePrivateFile(DataDirectory() + "/profiles.json", bytes).Ok();
}

std::string Runtime::ProfilePath() const {
  std::string base = DataDirectory();
  return selected_profile_ == "default"
             ? base + "/profile"
             : base + "/profiles/" + selected_profile_;
}

bool Runtime::SaveHandover() const {
  std::string path = DataDirectory() + "/handover.json";
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
  unlink(RfbPath().c_str());
  target_.clear();
  page_session_.clear();
  observation_.clear();
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
  if (!profile_error_.empty()) {
    error = profile_error_;
    return false;
  }
  if (Alive(chrome_pid_) && Alive(vnc_pid_)) return true;
  Stop(true);
  std::string base = DataDirectory();
  std::string profile = ProfilePath();
  if (!EnsureDataDirectory(profile)) {
    error = "cannot create persistent Chrome profile";
    return false;
  }
  profile_lock_.Reset(open((base + "/profile.lock").c_str(),
                           O_CREAT | O_RDWR | O_CLOEXEC, 0600));
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
  std::string cookie = session::RandomToken(16);
  if (cookie.empty()) {
    error = "cannot generate X authorization";
    return false;
  }
  pid_t xauth = Launch({"xauth", "-f", authority, "add", ":99", ".", cookie});
  int xauth_status = 0;
  if (xauth < 0 || waitpid(xauth, &xauth_status, 0) < 0 ||
      !WIFEXITED(xauth_status) || WEXITSTATUS(xauth_status) != 0) {
    error = "cannot create X authorization";
    return false;
  }
  chmod(authority.c_str(), 0600);
  setenv("DISPLAY", ":99", 1);
  setenv("XAUTHORITY", authority.c_str(), 1);
  unlink(RfbPath().c_str());
  vnc_pid_ = Launch({"Xtigervnc",
                     ":99",
                     "-geometry",
                     "1280x800",
                     "-depth",
                     "24",
                     "-auth",
                     authority,
                     "-rfbunixpath",
                     RfbPath(),
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
  for (int attempt = 0; attempt < 100 && access(RfbPath().c_str(), F_OK);
       ++attempt) {
    poll(nullptr, 0, 30);
  }
  if (access(RfbPath().c_str(), F_OK)) {
    error = "Xtigervnc did not create its Unix display socket";
    Stop(true);
    return false;
  }
  // Manual profile setup uses ordinary Chrome, without a debugging endpoint.
  // Both modes own the same profile exclusively and restore its saved tabs.
  std::vector<std::string> chrome = {
      "google-chrome-stable",   "--user-data-dir=" + profile,
      "--no-first-run",         "--no-default-browser-check",
      "--window-size=1280,800", "--ozone-platform=x11",
      "--password-store=basic", "--restore-last-session"};
  if (profile_setup) {
    chrome_pid_ = Launch(chrome);
    if (chrome_pid_ < 0) {
      error = "cannot launch Google Chrome Stable";
      Stop(true);
      return false;
    }
    profile_setup_ = true;
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
  chrome_pid_ = Launch(chrome, &actions);
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
  return true;
}

json Runtime::Call(const std::string& method, const json& parameters,
                   const std::string& session) {
  if (!cdp_in_ || !cdp_out_) return {{"error", "Chrome is stopped"}};
  const int64_t id = ++next_id_;
  json command = {{"id", id}, {"method", method}, {"params", parameters}};
  if (!session.empty()) command["sessionId"] = session;
  std::string packet = JsonDump(command);
  packet.push_back('\0');
  for (size_t sent = 0; sent < packet.size();) {
    pollfd ready{cdp_in_.Get(), POLLOUT, 0};
    if (poll(&ready, 1, 10000) <= 0) {
      return {{"error", "Chrome write timed out"}};
    }
    ssize_t n =
        write(cdp_in_.Get(), packet.data() + sent, packet.size() - sent);
    if (n <= 0) return {{"error", "Chrome control pipe closed"}};
    sent += static_cast<size_t>(n);
  }
  for (;;) {
    size_t end = cdp_buffer_.find('\0');
    if (end != std::string::npos) {
      json response = json::parse(cdp_buffer_.substr(0, end), nullptr, false);
      cdp_buffer_.erase(0, end + 1);
      if (JsonValue(response, "id", int64_t{-1}) == id) return response;
      continue;  // Unsolicited events have no reply id.
    }
    if (cdp_buffer_.size() > size_t{16} * 1024 * 1024) {
      return {{"error", "Chrome response exceeded limit"}};
    }
    pollfd ready{cdp_out_.Get(), POLLIN, 0};
    if (poll(&ready, 1, 10000) <= 0) {
      return {{"error", "Chrome read timed out"}};
    }
    char bytes[16384];
    ssize_t n = read(cdp_out_.Get(), bytes, sizeof(bytes));
    if (n <= 0) return {{"error", "Chrome control pipe closed"}};
    cdp_buffer_.append(bytes, static_cast<size_t>(n));
  }
}

bool Runtime::SelectPage(std::string& error) {
  json targets = Call("Target.getTargets");
  if (const json* result = JsonObject(targets, "result")) {
    if (const json* infos = JsonArray(*result, "targetInfos")) {
      for (const auto& item : *infos) {
        if (JsonValue(item, "type", "") == "page") {
          target_ = JsonValue(item, "targetId", "");
          break;
        }
      }
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

bool Runtime::AttachPage(const std::string& target, std::string& error) {
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
  if (auto enabled =
          CdError(Call("Page.enable", json::object(), attached_session));
      !enabled.empty()) {
    error = enabled;
    return false;
  }
  page_session_ = std::move(attached_session);
  target_ = target;
  observation_.clear();
  view_width_ = view_height_ = 0;
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
                 {"waiting",
                  mode_ == "human" && NowMillis() < agent_waiting_until_ms_},
                 {"session_id", agent_session_},
                 {"interaction_id", interaction_},
                 {"viewer", viewer_},
                 {"profile_id", selected_profile_},
                 {"profile_setup", profile_setup_},
                 {"profiles", profiles},
                 {"generation", generation_}};
  if (!profile_error_.empty()) result["error"] = profile_error_;
  if (running && include_page && !profile_setup_) {
    json targets = Call("Target.getTargets");
    if (const json* response = JsonObject(targets, "result")) {
      if (const json* infos = JsonArray(*response, "targetInfos")) {
        for (const auto& info : *infos) {
          if (JsonValue(info, "targetId", "") == target_) {
            std::string url = JsonValue(info, "url", "");
            result["url"] = url.substr(0, url.find_first_of("?#"));
            result["title"] = JsonValue(info, "title", "");
            break;
          }
        }
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
    error = "human controls the browser; wait for Done";
    return false;
  }
  if (!agent_session_.empty() && agent_session_ != session) {
    error = "browser is leased to another conversation";
    return false;
  }
  agent_session_ = session;
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
  if (op == "create_profile" || op == "select_profile") {
    if (!profile_error_.empty()) return {{"error", profile_error_}};
    std::string device = JsonValue(command, "device", "");
    if (!session::OpaqueId(device)) return {{"error", "invalid device"}};
    if (!(mode_ == "human" && viewer_ == device) &&
        !(mode_ == "idle" && viewer_.empty() && chrome_pid_ <= 0 &&
          vnc_pid_ <= 0)) {
      return {{"error", "take control before changing Chrome profiles"}};
    }
    if (op == "create_profile") {
      std::string name = ProfileName(JsonValue(command, "name", ""));
      if (name.empty()) return {{"error", "invalid Chrome profile name"}};
      for (const auto& profile : profiles_) {
        if (profile.name == name) {
          return {{"error", "Chrome profile name already exists"}};
        }
      }
      std::string id = session::RandomToken(16);
      if (id.empty()) return {{"error", "cannot create Chrome profile ID"}};
      profiles_.push_back({id, name});
      if (!SaveProfiles()) {
        profiles_.pop_back();
        return {{"error", "cannot save Chrome profile"}};
      }
      json result = Status();
      result["created_profile_id"] = id;
      return result;
    }
    std::string id = JsonValue(command, "profile_id", "");
    if (std::none_of(
            profiles_.begin(), profiles_.end(),
            [&](const Profile& profile) { return profile.id == id; })) {
      return {{"error", "Chrome profile does not exist"}};
    }
    return SwitchProfile(id);
  }
  if (!profile_error_.empty() && op != "stop") {
    return {{"error", profile_error_}};
  }
  if (op == "agent_status") {
    std::string session = JsonValue(command, "session_id", "");
    if (!session::OpaqueId(session)) return {{"error", "invalid session"}};
    if (mode_ == "human") {
      agent_waiting_until_ms_ = NowMillis() + 2000;
      return {{"error", "human controls the browser; wait for Done"}};
    }
    if (!agent_session_.empty() && agent_session_ != session) {
      return {{"error", "browser is leased to another conversation"}};
    }
    return Status();
  }
  if (op == "stop") {
    if (mode_ == "human" && !interaction_.empty()) {
      return {{"error", "finish or cancel the browser interaction first"}};
    }
    if (!viewer_.empty() && viewer_ != JsonValue(command, "device", "")) {
      return {{"error", "another device controls the browser"}};
    }
    Stop();
    SaveHandover();
    return Status();
  }
  if (op == "takeover" || op == "setup_profile") {
    std::string device = JsonValue(command, "device", "");
    if (!session::OpaqueId(device)) return {{"error", "invalid device"}};
    if (!viewer_.empty() && viewer_ != device) {
      return {{"error", "another device controls the browser"}};
    }
    if (op == "setup_profile" && (mode_ != "human" || viewer_ != device)) {
      return {{"error", "take control before signing in to a profile"}};
    }
    const bool setup = profile_setup_ || op == "setup_profile";
    if (setup && !profile_setup_) Stop(true);
    std::string error;
    if (!Start(error, setup)) {
      viewer_ = device;
      mode_ = "human";
      return {{"error", error}};
    }
    viewer_ = device;
    mode_ = "human";
    observation_.clear();
    ++generation_;
    return Status();
  }
  if (op == "viewer") {
    const std::string role = JsonValue(command, "role", "control");
    if (role == "observe") {
      if (!Alive(chrome_pid_) || !Alive(vnc_pid_)) {
        return {{"error", "the browser is not running"}};
      }
      return Status(false);
    }
    if (role != "control" || mode_ != "human" ||
        viewer_ != JsonValue(command, "device", "")) {
      return {{"error", "this device does not control the browser"}};
    }
    return Status(false);
  }
  if (op == "viewer_disconnected") {
    if (viewer_ == JsonValue(command, "device", "") &&
        generation_ == JsonValue(command, "generation", uint64_t{0})) {
      viewer_.clear();
      // Closing the viewer hands control back, unless a login/MFA request
      // or profile sign-in is still waiting on the human's Done.
      if (mode_ == "human" && interaction_.empty() && !profile_setup_) {
        mode_ = agent_session_.empty() ? "idle" : "agent";
        observation_.clear();
        SaveHandover();
      }
      ++generation_;
    }
    return Status();
  }
  if (op == "prepare_done" || op == "done") {
    if (mode_ != "human" || viewer_ != JsonValue(command, "device", "") ||
        interaction_ != JsonValue(command, "interaction_id", "")) {
      return {{"error", "browser interaction changed; refresh"}};
    }
    if (profile_setup_) {
      const std::string controller = viewer_;
      Stop(true);
      std::string error;
      const bool started = Start(error);
      viewer_ = controller;
      mode_ = "human";
      if (!started) {
        profile_setup_ = true;
        return {{"error", error}};
      }
    }
    if (op == "prepare_done") return Status();
  }
  if (op == "done") {
    std::string previous_viewer = viewer_;
    std::string previous_interaction = interaction_;
    std::string previous_mode = mode_;
    viewer_.clear();
    interaction_.clear();
    mode_ = agent_session_.empty() ? "idle" : "agent";
    if (!SaveHandover()) {
      viewer_ = std::move(previous_viewer);
      interaction_ = std::move(previous_interaction);
      mode_ = std::move(previous_mode);
      return {{"error", "cannot persist browser handback"}};
    }
    observation_.clear();
    ++generation_;
    return Status();
  }
  if (op == "release") {
    if (agent_session_ != JsonValue(command, "session_id", "")) {
      return {{"error", "browser lease belongs to another conversation"}};
    }
    if (mode_ == "human") return {{"error", "human controls the browser"}};
    agent_session_.clear();
    mode_ = "idle";
    observation_.clear();
    return Status();
  }
  if (op == "cancel_handover") {
    if (agent_session_ != JsonValue(command, "session_id", "") ||
        interaction_ != JsonValue(command, "interaction_id", "") ||
        interaction_.empty()) {
      return {{"error", "browser handover changed"}};
    }
    std::string previous_interaction = interaction_;
    std::string previous_session = agent_session_;
    interaction_.clear();
    agent_session_.clear();
    observation_.clear();
    mode_ = viewer_.empty() ? "idle" : "human";
    if (!SaveHandover()) {
      interaction_ = std::move(previous_interaction);
      agent_session_ = std::move(previous_session);
      mode_ = "human";
      return {{"error", "cannot clear browser handover"}};
    }
    return Status();
  }
  if (op == "request_human" && mode_ == "human" && interaction_.empty()) {
    std::string session = JsonValue(command, "session_id", "");
    std::string interaction = JsonValue(command, "interaction_id", "");
    if (!session::OpaqueId(session) || !session::OpaqueId(interaction) ||
        (!agent_session_.empty() && agent_session_ != session)) {
      return {{"error", "browser handover belongs to another conversation"}};
    }
    std::string previous_session = agent_session_;
    agent_session_ = session;
    interaction_ = interaction;
    observation_.clear();
    if (!SaveHandover()) {
      agent_session_ = std::move(previous_session);
      interaction_.clear();
      return {{"error", "cannot persist browser handover"}};
    }
    return Status();
  }
  std::string error;
  if (mode_ == "human") {
    agent_waiting_until_ms_ = NowMillis() + 2000;
    return {{"error", "human controls the browser; wait for Done"}};
  }
  if (!Start(error)) return {{"error", error}};
  if (!Agent(command, error)) return {{"error", error}};
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
  if (op == "tabs") {
    json targets = Call("Target.getTargets");
    if (auto reason = CdError(targets); !reason.empty()) {
      return {{"error", reason}};
    }
    json pages = json::array();
    if (const json* result = JsonObject(targets, "result")) {
      if (const json* infos = JsonArray(*result, "targetInfos")) {
        for (const auto& info : *infos) {
          if (pages.size() >= 16) break;
          std::string url = JsonValue(info, "url", "");
          if (JsonValue(info, "type", "") != "page" ||
              !(url.starts_with("https://") || url.starts_with("http://") ||
                url == "about:blank")) {
            continue;
          }
          pages.push_back(
              {{"id", JsonValue(info, "targetId", "")},
               {"url", url.substr(0, url.find_first_of("?#"))},
               {"title", JsonValue(info, "title", "")},
               {"selected", JsonValue(info, "targetId", "") == target_}});
        }
      }
    }
    std::string requested = JsonValue(command, "target_id", "");
    if (!requested.empty()) {
      bool found = false;
      for (const auto& page : pages) {
        found |= JsonValue(page, "id", "") == requested;
      }
      if (!found) return {{"error", "tab is unavailable"}};
      if (requested != target_) {
        json activated =
            Call("Target.activateTarget", {{"targetId", requested}});
        if (auto reason = CdError(activated); !reason.empty()) {
          return {{"error", reason}};
        }
        if (!AttachPage(requested, error)) return {{"error", error}};
      }
    }
    return {{"ok", true}, {"tabs", pages}, {"selected", target_}};
  }
  json reply;
  if (op == "open") {
    std::string url = JsonValue(command, "url", "");
    if (!(url.starts_with("https://") || url.starts_with("http://")) ||
        url.size() > 4096) {
      return {{"error", "URL must be HTTP(S)"}};
    }
    observation_.clear();
    reply = Call("Page.navigate", {{"url", url}}, page_session_);
  } else if (op == "click") {
    if (observation_.empty() ||
        observation_ != JsonValue(command, "view_id", "")) {
      return {{"error", "observe the current tab before clicking"}};
    }
    int x, y;
    if (!Coordinate(command, "x", x) || !Coordinate(command, "y", y)) {
      return {{"error", "click requires x and y"}};
    }
    if (x >= view_width_ || y >= view_height_) {
      return {{"error", "coordinates exceed the observed viewport"}};
    }
    observation_.clear();
    json pressed = Call("Input.dispatchMouseEvent",
                        {{"type", "mousePressed"},
                         {"x", x},
                         {"y", y},
                         {"button", "left"},
                         {"clickCount", 1}},
                        page_session_);
    if (auto reason = CdError(pressed); !reason.empty()) {
      return {{"error", reason}};
    }
    reply = Call("Input.dispatchMouseEvent",
                 {{"type", "mouseReleased"},
                  {"x", x},
                  {"y", y},
                  {"button", "left"},
                  {"clickCount", 1}},
                 page_session_);
  } else if (op == "type") {
    std::string value = JsonValue(command, "text", "");
    if (value.size() > 32768) return {{"error", "text exceeds limit"}};
    observation_.clear();
    reply = Call("Input.insertText", {{"text", value}}, page_session_);
  } else if (op == "press") {
    std::string key = JsonValue(command, "key", "");
    if (key != "Enter" && key != "Tab" && key != "Escape" &&
        key != "Backspace") {
      return {{"error", "unsupported key"}};
    }
    observation_.clear();
    reply = Call("Input.dispatchKeyEvent", {{"type", "keyDown"}, {"key", key}},
                 page_session_);
    if (CdError(reply).empty()) {
      reply = Call("Input.dispatchKeyEvent", {{"type", "keyUp"}, {"key", key}},
                   page_session_);
    }
  } else if (op == "scroll") {
    if (observation_.empty() ||
        observation_ != JsonValue(command, "view_id", "")) {
      return {{"error", "observe the current tab before scrolling"}};
    }
    int x = JsonValue(command, "x", 640), y = JsonValue(command, "y", 400);
    int delta = JsonValue(command, "delta_y", 0);
    if (delta < -3000 || delta > 3000) {
      return {{"error", "invalid scroll amount"}};
    }
    if (x < 0 || y < 0 || x >= view_width_ || y >= view_height_) {
      return {{"error", "coordinates exceed the observed viewport"}};
    }
    observation_.clear();
    reply = Call("Input.dispatchMouseEvent",
                 {{"type", "mouseWheel"},
                  {"x", x},
                  {"y", y},
                  {"deltaX", 0},
                  {"deltaY", delta}},
                 page_session_);
  } else if (op == "observe") {
    json metrics = Call("Page.getLayoutMetrics", json::object(), page_session_);
    if (auto reason = CdError(metrics); !reason.empty()) {
      return {{"error", reason}};
    }
    const json* result = JsonObject(metrics, "result");
    const json* viewport =
        result ? JsonObject(*result, "cssVisualViewport") : nullptr;
    double width = viewport ? JsonValue(*viewport, "clientWidth", 0.0) : 0;
    double height = viewport ? JsonValue(*viewport, "clientHeight", 0.0) : 0;
    if (width < 1 || width > 10000 || height < 1 || height > 10000) {
      return {{"error", "Chrome did not report its viewport"}};
    }
    json shot = Call(
        "Page.captureScreenshot",
        {{"format", "jpeg"}, {"quality", 70}, {"captureBeyondViewport", false}},
        page_session_);
    if (const json* image_result = JsonObject(shot, "result")) {
      json page = Call("Runtime.evaluate",
                       {{"expression",
                         "({url:location.href.split(/[?#]/"
                         ")[0],title:document.title,text:(document.body?."
                         "innerText||'').slice(0,12000)})"},
                        {"returnByValue", true}},
                       page_session_);
      json details = json::object();
      if (const json* response = JsonObject(page, "result")) {
        if (const json* remote = JsonObject(*response, "result")) {
          if (const json* value = JsonObject(*remote, "value")) {
            details = *value;
          }
        }
      }
      observation_ = session::RandomToken(12);
      view_width_ = static_cast<int>(width);
      view_height_ = static_cast<int>(height);
      return {{"ok", true},
              {"image", JsonValue(*image_result, "data", "")},
              {"page", details},
              {"generation", generation_},
              {"view_id", observation_},
              {"width", view_width_},
              {"height", view_height_}};
    }
    return {
        {"error", CdError(shot).empty() ? "screenshot failed" : CdError(shot)}};
  } else {
    return {{"error", "unsupported browser action"}};
  }
  error = CdError(reply);
  if (!error.empty()) return {{"error", error}};
  return {{"ok", true}};
}
}  // namespace uagent::browser
