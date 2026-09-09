// Copyright 2026 Timon Gentzsch

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT

#include <curl/curl.h>
#include <fcntl.h>
#include <httplib.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/bootstrap.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/library.h"
#include "include/app/schedule.h"
#include "include/core/capture.h"
#include "include/core/child_env.h"
#include "include/core/effective_config.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/tools/files.h"
#include "include/web/assets.h"
#include "include/web/protocol.h"
#include "include/web/push.h"

namespace uagent::web {
namespace {
using Clock = std::chrono::steady_clock;
using Request = httplib::Request;
using Response = httplib::Response;
constexpr size_t kDeviceLimit = 16;
constexpr size_t kSseLimit = 4;
volatile sig_atomic_t shutdown_fd = -1;
void StopSignal(int) { WakeDescriptor(shutdown_fd); }

void Reply(Response& response, const json& value, int status = 200) {
  response.status = status;
  response.set_content(JsonDump(value), "application/json");
}
void Error(Response& response, const std::string& error, int status = 400) {
  Reply(response, {{"error", error}}, status);
}

bool EqualSecret(std::string_view a, std::string_view b) {
  if (a.size() != b.size() || a.empty()) {
    return false;
  }
  unsigned difference = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    difference |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return difference == 0;
}

bool PrivateDirectory(const std::string& path) {
  // The user-level state tree is never redirected through directory symlinks.
  std::filesystem::path current = GlobalBase();
  auto relative = std::filesystem::path(path).lexically_relative(current);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }
  for (const auto& part : relative) {
    if (part == "..") {
      return false;
    }
  }
  std::error_code ec;
  if (std::filesystem::is_symlink(
          std::filesystem::symlink_status(current, ec))) {
    return false;
  }
  for (const auto& part : relative) {
    current /= part;
    if (std::filesystem::is_symlink(
            std::filesystem::symlink_status(current, ec))) {
      return false;
    }
  }
  CreatePrivateDirectories(path);
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
         info.st_uid == geteuid() && (info.st_mode & 0077) == 0;
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct AssetUsage {
  size_t bytes = 0, count = 0;
  bool valid = true;
};
AssetUsage InspectAssets(const std::string& folder, bool cleanup) {
  AssetUsage usage;
  std::error_code ec;
  if (!std::filesystem::is_directory(
          std::filesystem::symlink_status(folder, ec))) {
    return usage;
  }
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!std::filesystem::is_regular_file(entry.symlink_status(ec))) {
      usage.valid = false;
      break;
    }
    if (entry.path().extension() == ".json") {
      continue;
    }
    if (cleanup && std::filesystem::file_time_type::clock::now() -
                           entry.last_write_time(ec) >
                       std::chrono::hours(24)) {
      auto metadata = entry.path();
      metadata.replace_extension(".json");
      std::string bytes, error;
      json record;
      if (ReadRegularFile(metadata.string(), 1024, bytes, error)) {
        record = json::parse(bytes, nullptr, false);
      }
      if (!JsonValue(record, "committed", false)) {
        std::filesystem::remove(entry.path(), ec);
        if (!ec) {
          std::filesystem::remove(metadata, ec);
          continue;
        }
      }
    }
    uintmax_t bytes = entry.file_size(ec);
    if (ec || bytes > kGlobalAssetBytes ||
        usage.bytes > kGlobalAssetBytes - bytes) {
      usage.valid = false;
      break;
    }
    usage.bytes += static_cast<size_t>(bytes);
    if (++usage.count > 64) {
      usage.valid = false;
      break;
    }
  }
  return usage;
}

struct Session {
  std::string run_id, task_id, launch_prompt, run_result;
  bool run_checkpoint = false, stop_sent = false;
  json launch = json::object();
  std::string id, path, cwd, title, draft_title, generation, status = "saved",
                                                             error;
  json state = json::object(), pending = nullptr;
  std::deque<json> live;
  size_t live_bytes = 0;
  int64_t updated = 0;
  bool live_truncated = false;
  bool turn_active = false;
  bool terminal_active = false;
  uint64_t incoming = 0;
  pid_t pid = -1;
  Pipe input, output, stop;
  std::thread reader;
  std::mutex send_mutex;
  std::map<std::string, std::pair<std::string, std::string>> awaiting;
  std::atomic<bool> exited{false};
  ~Session() {
    if (pid > 0) {
      {
        std::lock_guard lock(send_mutex);
        input.write.Reset();
      }
      int result = 0;
      if (!ReapPidFor(pid, &result, std::chrono::seconds(3))) {
        kill(pid, SIGTERM);
        if (!ReapPidFor(pid, &result, std::chrono::seconds(1))) {
          kill(pid, SIGKILL);
          ReapPidFor(pid, &result, std::chrono::seconds(1));
        }
      }
    }
    stop.Wake();
    if (reader.joinable()) {
      reader.join();
    }
  }
  bool Send(json frame) {
    frame["v"] = kProtocol;
    frame["session_id"] = id;
    frame["generation"] = generation;
    std::lock_guard lock(send_mutex);
    return input.write && WriteFrame(input.write.Get(), frame);
  }
};

std::shared_ptr<Session> SavedSession(const Session& session) {
  auto saved = std::make_shared<Session>();
  saved->id = session.id;
  saved->path = session.path;
  saved->cwd = session.cwd;
  saved->title = session.title;
  saved->draft_title = session.draft_title;
  saved->task_id = session.task_id;
  saved->run_id = session.run_id;
  saved->incoming = session.incoming;
  saved->updated = session.updated;
  return saved;
}

struct Device {
  std::string id, token, name;
  int64_t expires = 0;
};
struct Replay {
  uint64_t sequence;
  std::string frame;
};

class Master {
 public:
  Master(WebOptions options, std::string executable, std::string directory)
      : options_(std::move(options)),
        executable_(std::move(executable)),
        directory_(std::move(directory)),
        local_("http://127.0.0.1:" + std::to_string(options_.port)),
        origin_(options_.origin.empty() ? local_ : options_.origin),
        epoch_(RandomToken(16)),
        secret_(RandomToken()) {}

  bool Initialize(std::string& error) {
    if (epoch_.empty() || secret_.empty()) {
      error = "OS randomness unavailable";
      return false;
    }
    if (!options_.origin.empty()) {
      // No path, query, userinfo or forwarding-header origin inference.
      if (!origin_.starts_with("https://") || origin_.size() > 255 ||
          origin_.substr(8).find_first_of("/@?#\\ \t\r\n") !=
              std::string::npos ||
          origin_.size() == 8) {
        error = "web origin must be a bare HTTPS origin";
        return false;
      }
      std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(),
                                                              curl_url_cleanup);
      if (!url || curl_url_set(url.get(), CURLUPART_URL, origin_.c_str(), 0) !=
                      CURLUE_OK) {
        error = "web origin is not a valid HTTPS URL";
        return false;
      }
    }
    authority_ = origin_.substr(origin_.find("://") + 3);
    LoadDevices();
    push_ = std::make_unique<PushSender>(directory_, options_.push_contact);
    LoadDrafts();
    RefreshCatalogue();
    scheduled_view_ = ScheduleControl({{"action", "list"}});
    GlobalAssetUsage();
    server_.new_task_queue = [] { return new httplib::ThreadPool(12, 12, 24); };
    server_.set_read_timeout(5);
    server_.set_write_timeout(2);
    server_.set_keep_alive_max_count(50);
    server_.set_keep_alive_timeout(2);
    server_.set_payload_max_length(kUploadBytes);
    server_.set_default_headers(
        {{"X-Content-Type-Options", "nosniff"},
         {"Referrer-Policy", "no-referrer"},
         {"Cross-Origin-Resource-Policy", "same-origin"},
         {"Cache-Control", "no-store"},
         {"Content-Security-Policy",
          "default-src 'none'; script-src 'self'; style-src 'self' "
          "'unsafe-inline'; font-src 'self'; img-src 'self' blob:; connect-src "
          "'self'; manifest-src 'self'; worker-src 'self'; base-uri 'none'; "
          "form-action 'self'; frame-ancestors 'none'"}});
    server_.set_pre_routing_handler(
        [this](const Request& request, Response& response) {
          const std::string host = request.get_header_value("Host");
          bool discovery = request.path == "/api/instance";
          bool valid_host =
              host == authority_ || (discovery && host == local_.substr(7));
          std::string origin = request.get_header_value("Origin");
          bool mutation = request.method != "GET" && request.method != "HEAD";
          if (!valid_host || (!origin.empty() && origin != origin_) ||
              (mutation && !discovery && origin != origin_)) {
            Error(response, "invalid Host or Origin", 403);
            return httplib::Server::HandlerResponse::Handled;
          }
          if (request.path.starts_with("/api/") &&
              request.path != "/api/auth" && !discovery) {
            std::lock_guard lock(mutex_);
            if (DeviceId(request).empty()) {
              Error(response, "authentication required", 401);
              return httplib::Server::HandlerResponse::Handled;
            }
          }
          return httplib::Server::HandlerResponse::Unhandled;
        });
    server_.Post(
        "/api/instance", [this](const Request& request, Response& response) {
          if (!EqualSecret(request.get_header_value("X-Uagent-Instance"),
                           secret_)) {
            Error(response, "instance authentication failed", 403);
            return;
          }
          std::lock_guard lock(mutex_);
          Reply(response, {{"v", kProtocol},
                           {"epoch", epoch_},
                           {"url", origin_},
                           {"pairing_code", Pair()}});
        });
    server_.Post("/api/auth",
                 [this](const Request& request, Response& response) {
                   Authenticate(request, response);
                 });
    server_.Get("/api/sessions", [this](const Request& request,
                                        Response& response) {
      if (request.has_param("refresh")) {
        RefreshCatalogue(true);
      }
      RefreshPresence();
      std::lock_guard lock(mutex_);
      json list = json::array();
      for (const auto& [id, session] : sessions_) {
        list.push_back(Metadata(*session));
      }
      Reply(response, {{"v", kProtocol},
                       {"epoch", epoch_},
                       {"cursor", sequence_},
                       {"sessions", list},
                       {"worker_limit", kWorkerLimit},
                       {"capabilities", push_->Capabilities(DeviceId(request))},
                       {"devices", PublicDevices()},
                       {"scheduled", scheduled_view_},
                       {"device", DeviceId(request)}});
    });
    server_.Get(R"(/api/sessions/([a-f0-9]{16,64}))",
                [this](const Request& request, Response& response) {
                  Snapshot(request, response);
                });
    server_.Post("/api/command",
                 [this](const Request& request, Response& response) {
                   Command(request, response);
                 });
    server_.Get(R"(/api/receipts/([a-f0-9]{16,64}))",
                [this](const Request& request, Response& response) {
                  std::lock_guard lock(mutex_);
                  auto found = requests_.find(DeviceId(request) + ":" +
                                              request.matches[1].str());
                  Reply(response,
                        found == requests_.end()
                            ? json{{"request_id", request.matches[1].str()},
                                   {"unknown", true}}
                            : found->second.outcome);
                });
    server_.Post("/api/push/subscriptions", [this](const Request& request,
                                                   Response& response) {
      if (request.body.size() > 8192) {
        Error(response, "subscription exceeds limit", 413);
        return;
      }
      std::lock_guard lock(mutex_);
      std::string subscription_error;
      std::string device = DeviceId(request);
      if (device.empty()) {
        Error(response, "device revoked", 401);
        return;
      }
      if (!push_->Subscribe(device, json::parse(request.body, nullptr, false),
                            subscription_error)) {
        Error(response, subscription_error, 422);
        return;
      }
      Reply(response, push_->Capabilities(device));
    });
    server_.Delete("/api/push/subscriptions", [this](const Request& request,
                                                     Response& response) {
      std::lock_guard lock(mutex_);
      std::string device = DeviceId(request);
      if (device.empty()) {
        Error(response, "device revoked", 401);
        return;
      }
      if (!push_->Revoke(device)) {
        Error(response, "cannot persist notification revocation", 500);
        return;
      }
      Reply(response, push_->Capabilities(device));
    });
    server_.Get("/api/events",
                [this](const Request& request, Response& response) {
                  Events(request, response);
                });
    server_.Post(R"(/api/sessions/([a-f0-9]{16,64})/attachments)",
                 [this](const Request& request, Response& response,
                        const httplib::ContentReader& reader) {
                   Upload(request, response, reader);
                 });
    server_.Get(R"(/api/sessions/([a-f0-9]{16,64})/assets/([a-f0-9]{16,64}))",
                [this](const Request& request, Response& response) {
                  AssetRead(request, response);
                });
    server_.Get(R"(/.*)", [](const Request& request, Response& response) {
      std::string path = request.path == "/" ? "/index.html" : request.path;
      for (const Asset& asset : Assets()) {
        if (asset.path != path) {
          continue;
        }
        response.set_header("Cache-Control",
                            path.starts_with("/assets/")
                                ? "public, max-age=31536000, immutable"
                                : "no-cache");
        response.set_content(reinterpret_cast<const char*>(asset.data),
                             asset.size, std::string(asset.mime));
        return;
      }
      Error(response, "not found", 404);
    });
    if (!server_.bind_to_port("127.0.0.1", options_.port)) {
      error = "web port is unavailable; no alternate server was started";
      return false;
    }
    return true;
  }

  int Run() {
    std::string error;
    json discovery = {{"v", kProtocol},
                      {"port", options_.port},
                      {"url", origin_},
                      {"secret", secret_},
                      {"epoch", epoch_}};
    auto written = ToolWritePrivateFile(directory_ + "/discovery.json",
                                        JsonDump(discovery));
    if (!written.Ok()) {
      fprintf(stderr, "cannot publish web discovery\n");
      return 1;
    }
    Pipe stop;
    if (!stop.Open()) {
      return 1;
    }
    shutdown_fd = stop.write.Get();
    signal(SIGINT, StopSignal);
    signal(SIGTERM, StopSignal);
    signal(SIGHUP, StopSignal);
    std::string pairing = Pair();
    printf("µAgent web: %s\nPairing code (single use, 5 minutes): %s\n",
           origin_.c_str(), pairing.c_str());
    fflush(stdout);
    std::thread stopping([&] {
      pollfd ready{stop.read.Get(), POLLIN, 0};
      // The browser remains event-driven. Native ownership has no portable
      // unlock notification, so share the shutdown loop for a bounded check.
      for (;;) {
        int result = poll(&ready, 1, 1000);
        if (result > 0 || (result < 0 && errno != EINTR)) break;
        bool observed;
        {
          std::lock_guard lock(mutex_);
          observed = sse_count_ > 0;
        }
        if (observed) RefreshPresence();
      }
      stopping_.store(true);
      changed_.notify_all();
      server_.stop();
    });
    std::thread scheduled([&] {
      bool recovered = false;
      for (;;) {
        if (stopping_) break;
        if (!recovered) recovered = !RecoverScheduledRuns().contains("error");
        if (recovered) TickSchedules();
        pollfd ready{stop.read.Get(), POLLIN, 0};
        if (poll(&ready, 1, 1000) > 0) break;
      }
    });
    bool served = server_.listen_after_bind();
    stop.Wake();
    stopping.join();
    scheduled.join();
    shutdown_fd = -1;
    std::vector<std::shared_ptr<Session>> workers;
    {
      std::lock_guard lock(mutex_);
      for (auto& [id, session] : sessions_) {
        if (session->pid > 0) {
          workers.push_back(session);
        }
      }
    }
    for (const auto& worker : workers) {
      worker->Send({{"kind", "close"}, {"request_id", RandomToken(16)}});
    }
    {
      std::lock_guard lock(mutex_);
      sessions_.clear();
    }
    workers.clear();
    return served ? 0 : 1;
  }

 private:
  std::string Pair() {
    pair_ = RandomToken(12);
    pair_deadline_ = Clock::now() + std::chrono::minutes(5);
    return pair_;
  }
  json PublicDevices() const {
    json out = json::array();
    for (const Device& device : devices_) {
      out.push_back({{"id", device.id}, {"name", device.name}});
    }
    return out;
  }
  void LoadDevices() {
    std::string bytes, error;
    if (!ReadRegularFile(directory_ + "/devices.json", size_t{64} * 1024, bytes,
                         error)) {
      return;
    }
    json value = json::parse(bytes, nullptr, false);
    if (!value.is_array() || value.size() > kDeviceLimit) {
      return;
    }
    for (const json& item : value) {
      Device device{JsonValue(item, "id", ""), JsonValue(item, "token", ""),
                    JsonValue(item, "name", "Device"),
                    JsonValue(item, "expires", int64_t{0})};
      if (OpaqueId(device.id) && OpaqueId(device.token) &&
          device.expires > NowMillis()) {
        devices_.push_back(std::move(device));
      }
    }
  }
  bool SaveDevices() const {
    json out = json::array();
    for (const Device& device : devices_) {
      out.push_back({{"id", device.id},
                     {"token", device.token},
                     {"name", device.name},
                     {"expires", device.expires}});
    }
    return ToolWritePrivateFile(directory_ + "/devices.json", JsonDump(out))
        .Ok();
  }
  std::string DeviceId(const Request& request) const {
    std::string cookies = request.get_header_value("Cookie");
    // Only this one cookie; reject duplicate credentials instead of choosing.
    std::string token;
    for (const std::string& entry : SplitPathList(cookies, ';')) {
      std::string part = Trim(entry);
      if (!part.starts_with("uagent_device=")) {
        continue;
      }
      if (!token.empty()) {
        return {};
      }
      token = part.substr(14);
    }
    for (const Device& device : devices_) {
      if (device.expires > NowMillis() && EqualSecret(token, device.token)) {
        return device.id;
      }
    }
    return {};
  }
  void Authenticate(const Request& request, Response& response) {
    if (request.body.size() > 4096) {
      Error(response, "authentication request too large", 413);
      return;
    }
    json input = json::parse(request.body, nullptr, false);
    std::lock_guard lock(mutex_);
    auto now = Clock::now();
    if (now - auth_window_ > std::chrono::minutes(1)) {
      auth_window_ = now;
      auth_attempts_ = 0;
    }
    if (++auth_attempts_ > 12) {
      Error(response, "pairing rate limit; retry in a minute", 429);
      return;
    }
    if (now > pair_deadline_ ||
        !EqualSecret(JsonValue(input, "code", ""), pair_)) {
      Error(response, "invalid or expired pairing code", 403);
      return;
    }
    std::erase_if(devices_, [](const Device& device) {
      return device.expires <= NowMillis();
    });
    if (devices_.size() >= kDeviceLimit) {
      Error(response, "device limit reached; revoke a device first", 409);
      return;
    }
    Device device{RandomToken(16), RandomToken(32),
                  Utf8Prefix(JsonValue(input, "name", "Browser"), 80),
                  NowMillis() + int64_t{2592000000}};
    if (device.id.empty() || device.token.empty()) {
      Error(response, "randomness unavailable", 500);
      return;
    }
    devices_.push_back(device);
    if (!SaveDevices()) {
      devices_.pop_back();
      Error(response, "cannot persist device", 500);
      return;
    }
    pair_.clear();
    response.set_header(
        "Set-Cookie",
        "uagent_device=" + device.token +
            "; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000" +
            (origin_.starts_with("https://") ? "; Secure" : ""));
    Reply(response, {{"v", kProtocol}, {"device", device.id}});
  }
  json Metadata(const Session& session) const {
    return {
        {"id", session.id},
        {"task_id", session.task_id},
        {"run_id", session.run_id},
        {"cwd", session.cwd},
        {"title", session.title},
        {"generation", session.generation},
        {"status", session.status},
        {"presence", session.pid > 0 && !session.exited ? "web"
                     : session.terminal_active          ? "terminal"
                                                        : ""},
        {"turn_active", session.turn_active},
        {"incoming", session.incoming},
        {"activity", JsonValue(session.state, "activity", "Ready")},
        {"activities", JsonValue(session.state, "activities", json::array())},
        {"error", session.error},
        {"pending", !session.pending.is_null()},
        {"updated", session.updated}};
  }
  json LiveSnapshot(const Session& session) const {
    return {
        {"v", kProtocol},         {"epoch", epoch_},
        {"cursor", sequence_},    {"metadata", Metadata(session)},
        {"state", session.state}, {"pending", session.pending},
        {"live", session.live},   {"live_truncated", session.live_truncated}};
  }
  void LoadDrafts() {
    std::error_code ec;
    std::string folder = directory_ + "/drafts";
    if (!PrivateDirectory(folder)) {
      return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
      if (sessions_.size() >= 4096) {
        break;
      }
      if (entry.path().extension() != ".json") {
        continue;
      }
      std::string bytes, error;
      if (!ReadRegularFile(entry.path().string(), 4096, bytes, error)) {
        continue;
      }
      json draft = json::parse(bytes, nullptr, false);
      auto session = std::make_shared<Session>();
      session->id = JsonValue(draft, "id", "");
      session->path = JsonValue(draft, "path", "");
      session->cwd = JsonValue(draft, "cwd", "");
      std::filesystem::path path(session->path);
      if (!OpaqueId(session->id) || session->id != HashHex(session->path) ||
          path.parent_path() != std::filesystem::path(UagentDir(kHistoryDir)) /
                                    WorkspaceId(session->cwd) ||
          !path.filename().string().starts_with("web-") ||
          path.extension() != ".json") {
        continue;
      }
      session->draft_title = JsonValue(draft, "title", "");
      session->title = session->draft_title.empty() ? "New conversation"
                                                    : session->draft_title;
      session->status = "draft";
      session->updated = JsonValue(draft, "updated", int64_t{0});
      sessions_[session->id] = std::move(session);
    }
  }
  void RefreshCatalogue(bool force = false) {
    std::lock_guard scan(scan_mutex_);
    if (!force && Clock::now() - scanned_ < std::chrono::seconds(1)) {
      return;
    }
    scanned_ = Clock::now();
    auto list = ListSessions(SessionScope::kAll);
    std::lock_guard lock(mutex_);
    for (const SessionInfo& item : list) {
      std::string id = HashHex(item.path);
      auto [it, inserted] = sessions_.try_emplace(id, nullptr);
      if (inserted) {
        it->second = std::make_shared<Session>();
        it->second->id = id;
        it->second->path = item.path;
      }
      auto& session = *it->second;
      if (session.status == "updating" || session.status == "deleting") {
        continue;
      }
      const json before = Metadata(session);
      session.cwd = item.cwd;
      session.incoming = std::max(session.incoming, item.incoming);
      session.title = item.title;
      const auto stamp = SnapshotFile(item.path);
      session.updated =
          stamp.modified_seconds * 1000 + stamp.modified_nanoseconds / 1000000;
      if (session.status == "draft") {
        session.status = "saved";
      }
      if (session.pid <= 0) {
        session.error = item.error;
        session.terminal_active =
            FileLease::HasLiveOwner(session.path + ".lock");
      }
      if (inserted || before != Metadata(session)) {
        Publish(session.id, session.generation,
                {{"kind", "metadata"}, {"metadata", Metadata(session)}});
      }
    }
  }
  void RefreshPresence() {
    // Saved checkpoints are atomic renames. Directory stamps discover new CLI
    // sessions and checkpoints without rereading every history header each
    // tick.
    std::lock_guard monitoring(presence_mutex_);
    std::map<std::string, FileStamp> directories;
    const std::string base = UagentDir(kHistoryDir);
    directories.emplace(base, SnapshotFile(base));
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
      if (directories.size() >= 4096) break;
      if (std::filesystem::is_directory(entry.symlink_status(ec)) &&
          !entry.path().string().ends_with(".assets")) {
        directories.emplace(entry.path().string(),
                            SnapshotFile(entry.path().string()));
      }
    }
    if (directories != history_directories_) {
      history_directories_ = std::move(directories);
      RefreshCatalogue(true);
    }
    std::lock_guard lock(mutex_);
    for (const auto& [id, session] : sessions_) {
      if (session->pid > 0 || session->status == "updating" ||
          session->status == "deleting") {
        continue;
      }
      const bool active = FileLease::HasLiveOwner(session->path + ".lock");
      if (active == session->terminal_active) continue;
      session->terminal_active = active;
      Publish(id, session->generation,
              {{"kind", "metadata"}, {"metadata", Metadata(*session)}});
    }
  }
  size_t GlobalAssetUsage() {
    // Uploads serialize this cached accounting. Scan only the known two-level
    // history layout; never recurse through project paths or symlinks.
    if (Clock::now() - assets_scanned_ < std::chrono::minutes(1)) {
      return asset_bytes_;
    }
    assets_scanned_ = Clock::now();
    asset_bytes_ = 0;
    std::error_code ec;
    std::filesystem::path base = UagentDir(kHistoryDir);
    std::vector<std::filesystem::path> folders{base};
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
      if (std::filesystem::is_directory(entry.symlink_status(ec)) &&
          !entry.path().string().ends_with(".assets")) {
        folders.push_back(entry.path());
      }
      if (folders.size() > 4096) {
        return asset_bytes_ = kGlobalAssetBytes;
      }
    }
    size_t scanned = 0;
    for (const auto& directory : folders) {
      for (const auto& entry :
           std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.path().string().ends_with(".json.assets") ||
            !std::filesystem::is_directory(entry.symlink_status(ec))) {
          continue;
        }
        if (++scanned > 4096) {
          return asset_bytes_ = kGlobalAssetBytes;
        }
        AssetUsage usage = InspectAssets(entry.path().string(), true);
        if (!usage.valid || usage.bytes > kGlobalAssetBytes - asset_bytes_) {
          return asset_bytes_ = kGlobalAssetBytes;
        }
        asset_bytes_ += usage.bytes;
      }
    }
    return asset_bytes_;
  }
  void Publish(const std::string& session, const std::string& generation,
               json value) {
    value["epoch"] = epoch_;
    value["sequence"] = ++sequence_;
    value["session_id"] = session;
    value["generation"] = generation;
    value["v"] = kProtocol;
    const std::string type = JsonValue(value, "type", "");
    if (type == "turn.completed" || type == "approval.requested" ||
        type == "error" || JsonValue(value, "kind", "") == "error") {
      std::string attention = epoch_ + ":" + std::to_string(sequence_);
      value["attention_id"] = attention;
      for (const auto& device : devices_) {
        if (device.expires > NowMillis()) {
          push_->Notify(attention, session, device.id);
        }
      }
    }
    std::string frame = JsonDump(value);
    ring_bytes_ += frame.size();
    ring_.push_back({sequence_, std::move(frame)});
    while (ring_bytes_ > kQueueBytes || ring_.size() > 2048) {
      ring_bytes_ -= ring_.front().frame.size();
      ring_.pop_front();
    }
    changed_.notify_all();
  }
  void FailPending(Session& session) {
    for (const auto& [worker_id, request] : session.awaiting) {
      const auto& [key, request_id] = request;
      requests_.at(key).outcome = {{"request_id", request_id},
                                   {"accepted", false},
                                   {"error",
                                    "worker closed before acknowledgement; "
                                    "inspect history before retrying"}};
    }
    session.awaiting.clear();
    changed_.notify_all();
  }
  void Received(Session* session, json frame) {
    std::unique_lock lock(mutex_);
    auto owner = sessions_.find(session->id);
    if (owner == sessions_.end() || owner->second.get() != session ||
        JsonValue(frame, "session_id", "") != session->id ||
        JsonValue(frame, "generation", "") != session->generation) {
      return;
    }
    const std::string kind = JsonValue(frame, "kind", "");
    if (kind == "outcome") {
      auto waiting = session->awaiting.find(JsonValue(frame, "request_id", ""));
      if (waiting == session->awaiting.end()) {
        if (!session->run_id.empty() && !JsonValue(frame, "accepted", false)) {
          session->error =
              JsonValue(frame, "error", "scheduled submission failed");
          session->run_result = "failed";
        }
        return;
      }
      const auto& [key, request_id] = waiting->second;
      frame["request_id"] = request_id;
      requests_.at(key).outcome = {
          {"request_id", request_id},
          {"accepted", JsonValue(frame, "accepted", false)},
          {"error", JsonValue(frame, "error", "")},
          {"result", JsonValue(frame, "result", json::object())}};
      session->awaiting.erase(waiting);
      if (JsonValue(JsonValue(frame, "result", json::object()), "forked",
                    false)) {
        lock.unlock();
        RefreshCatalogue(true);
        lock.lock();
      }
    } else if (kind == "state") {
      session->state = JsonValue(frame, "state", json::object());
      session->pending = JsonValue(frame, "pending", json(nullptr));
      session->turn_active = JsonValue(frame, "busy", false);
      session->status = !session->pending.is_null()         ? "waiting"
                        : !session->state.contains("route") ? "starting"
                        : JsonValue(frame, "busy", false)   ? "running"
                                                            : "idle";
      if (JsonValue(frame, "checkpoint", false)) {
        if (!session->run_result.empty()) session->run_checkpoint = true;
        session->live.clear();
        session->live_bytes = 0;
        session->live_truncated = false;
        session->updated = NowMillis();
        std::string title = JsonValue(session->state, "title", "");
        if (!title.empty()) {
          session->title = title;
        }
      }
      frame["updated"] = session->updated;
    } else if (kind == "activity") {
      session->state["activity"] = JsonValue(frame, "activity", "Ready");
      session->turn_active = JsonValue(frame, "busy", false);
    } else if (kind == "gap") {
      session->live_truncated = true;
    } else if (kind == "error") {
      session->error = JsonValue(frame, "error", "worker failed");
      session->status = "failed";
    } else if (kind == "event") {
      const std::string type = JsonValue(frame, "type", "");
      if (!session->run_id.empty()) {
        if (type == "turn.completed") {
          auto outcome = JsonValue(frame["data"], "outcome", "error");
          session->run_result = outcome == "complete"      ? "completed"
                                : outcome == "interrupted" ? "interrupted"
                                                           : "failed";
          if (session->run_result == "failed") {
            session->error = "Turn ended: " + outcome;
          }
        } else if (type == "turn.stopped") {
          session->run_result = "interrupted";
        } else if (type == "error") {
          session->error =
              JsonValue(frame["data"], "error", "scheduled run failed");
        }
      }
      if (type == "http.exchange") {
        session->state["http"] = json::array({frame["data"]});
      } else if (type == "config.changed" &&
                 frame["data"].contains("permissions")) {
        session->state["permissions"] = frame["data"]["permissions"];
        session->state["yolo"] =
            frame["data"]["permissions"]["effective"] == "yolo";
      } else if (type == "activities.changed") {
        session->state["activities"] = frame["data"]["activities"];
      } else if (type == "message.changed") {
        json block = frame["data"]["block"];
        session->incoming = std::max(session->incoming,
                                     JsonValue(block, "incoming", uint64_t{0}));
        MergeDisplayBlock(session->state["view"], block);
        const std::string role = JsonValue(block, "kind", "");
        std::erase_if(session->live, [&](const json& item) {
          const std::string event_type = JsonValue(item, "type", "");
          return (role == "assistant" && event_type.starts_with("response.")) ||
                 (role == "tool_result" && event_type.starts_with("tool.") &&
                  JsonValue(JsonValue(item, "data", json::object()), "id",
                            "") == JsonValue(block, "call_id", ""));
        });
      } else {
        // Coalesce deltas within a response before bounded replay can evict its
        // beginning. The browser receives the original deltas through SSE.
        bool merged = false;
        if (type == "response.answer.delta" ||
            type == "response.reasoning.delta") {
          for (auto it = session->live.rbegin(); it != session->live.rend();
               ++it) {
            if (JsonValue(*it, "type", "") == "response.started") break;
            if (JsonValue(*it, "type", "") != type) continue;
            std::string text = JsonValue((*it)["data"], "text", "") +
                               JsonValue(frame["data"], "text", "");
            if (text.size() > size_t{64} * 1024) {
              text = Utf8Trunc(text, size_t{64} * 1024);
              (*it)["data"]["preview_truncated"] = true;
            }
            (*it)["data"]["text"] = std::move(text);
            merged = true;
            break;
          }
        }
        if (!merged) session->live.push_back(frame);
      }
      session->live_bytes = 0;
      for (const json& item : session->live) {
        session->live_bytes += JsonEstimatedBytes(item);
      }
      while (session->live_bytes > kFrameBytes || session->live.size() > 512) {
        session->live_bytes -= JsonEstimatedBytes(session->live.front());
        session->live.pop_front();
        session->live_truncated = true;
      }
    }
    Publish(session->id, session->generation, std::move(frame));
    if (kind == "gap") {
      lock.unlock();
      session->Send({{"kind", "refresh"}, {"request_id", RandomToken(16)}});
    }
  }
  bool Activate(const std::shared_ptr<Session>& session, std::string& error) {
    // Caller serializes activation with all session mutations.
    if (session->pid > 0 && !session->exited) {
      return true;
    }
    size_t workers = 0;
    for (const auto& [id, item] : sessions_) {
      if (item->pid > 0) {
        ++workers;
      }
    }
    if (workers >= kWorkerLimit) {
      error = "worker limit reached; close an idle or interrupted session";
      return false;
    }
    if (session->pid > 0) {
      error = "close the interrupted worker before resuming";
      return false;
    }
    std::error_code ec;
    auto cwd = std::filesystem::canonical(session->cwd, ec);
    if (ec || !std::filesystem::is_directory(cwd, ec)) {
      error = "recorded host directory is unavailable";
      return false;
    }
    session->cwd = cwd.string();
    if (!session->input.Open() || !session->output.Open() ||
        !session->stop.Open()) {
      error = "cannot open worker IPC";
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, session->input.read.Get(),
                                     STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, session->output.write.Get(),
                                     STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    session->generation = RandomToken(16);
    if (session->generation.empty()) {
      error = "OS randomness unavailable";
      posix_spawn_file_actions_destroy(&actions);
      return false;
    }
    std::vector<std::string> arguments{
        executable_, "--web-worker",      session->cwd,        session->path,
        session->id, session->generation, session->draft_title};
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    EnvironmentOverrides overrides{{"UAGENT_DEPTH", "0"},
                                   {"UAGENT_INTERNAL_MEMORY_SOURCE", ""}};
    for (const char* key :
         {"UAGENT_API_KEY", "UAGENT_PROVIDERS", "OPENROUTER_API_KEY"}) {
      std::string value = EnvStr(key);
      if (!value.empty()) {
        overrides.emplace_back(key, value);
      }
    }
    if (!session->launch.empty()) {
      auto model = JsonValue(session->launch, "model", "");
      if (!model.empty()) overrides.emplace_back("UAGENT_MODEL", model);
      overrides.emplace_back(
          "UAGENT_APPROVAL",
          JsonValue(session->launch, "permissions", "prompt"));
    }
    ChildEnvironment environment(overrides,
                                 ChildEnvironmentPolicy::kIndependentAgent);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    int result = posix_spawn(&session->pid, executable_.c_str(), &actions,
                             &attributes, argv.data(), environment.Data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    session->input.read.Reset();
    session->output.write.Reset();
    if (result != 0) {
      session->pid = -1;
      error = "cannot launch worker: " + std::string(strerror(result));
      return false;
    }
    session->status = "starting";
    session->error.clear();
    session->reader = std::thread([this, session = session.get()] {
      ReadFrames(session->output.read.Get(), session->stop.read.Get(),
                 kFrameBytes, [&](json frame) {
                   Received(session, std::move(frame));
                   return !stopping_;
                 });
      session->exited = true;
      std::lock_guard lock(mutex_);
      auto owner = sessions_.find(session->id);
      if (owner != sessions_.end() && owner->second.get() == session) {
        FailPending(*session);
        session->status = "interrupted";
        session->turn_active = false;
        session->state["activity"] = "Interrupted";
        session->pending = nullptr;
        Publish(session->id, session->generation, {{"kind", "closed"}});
      }
    });
    Publish(session->id, session->generation,
            {{"kind", "activated"}, {"metadata", Metadata(*session)}});
    return true;
  }

  struct RunUpdate {
    std::string id, status, error;
  };
  std::vector<RunUpdate> run_updates_;
  void RecordRun(const RunUpdate& update) {
    if (UpdateScheduledRun(update.id, update.status, update.error)
            .contains("error")) {
      run_updates_.push_back(update);
    }
  }
  void TickSchedules();
  void StartScheduledRun(const json& run);
  std::shared_ptr<Session> NewSession(const std::string& cwd,
                                      const std::string& path,
                                      const std::string& title,
                                      std::string& error);

  void Snapshot(const Request&, Response&);
  void Command(const Request&, Response&);
  void Events(const Request&, Response&);
  void Upload(const Request&, Response&, const httplib::ContentReader&);
  void AssetRead(const Request&, Response&);

  WebOptions options_;
  std::string executable_, directory_, local_, origin_, authority_, epoch_,
      secret_, pair_;
  Clock::time_point pair_deadline_{}, auth_window_{}, scanned_{};
  Clock::time_point assets_scanned_{};
  size_t asset_bytes_ = 0;
  int auth_attempts_ = 0;
  httplib::Server server_;
  std::mutex mutex_, scan_mutex_, history_mutex_, asset_mutex_, presence_mutex_;
  std::map<std::string, FileStamp> history_directories_;
  std::condition_variable changed_;
  std::map<std::string, std::shared_ptr<Session>> sessions_;
  std::vector<Device> devices_;
  std::deque<Replay> ring_;
  uint64_t sequence_ = 0;
  size_t ring_bytes_ = 0, sse_count_ = 0;
  struct Receipt {
    std::string fingerprint;
    json outcome;
    bool handling = true;
  };
  std::map<std::string, Receipt> requests_;
  std::deque<std::string> request_order_;
  std::atomic<bool> stopping_{false};
  FileStamp library_stamp_, schedule_stamp_;
  json schedule_state_ = json::object(), scheduled_view_;
  std::unique_ptr<PushSender> push_;
};

std::shared_ptr<Session> Master::NewSession(const std::string& cwd,
                                            const std::string& path,
                                            const std::string& title,
                                            std::string& error) {
  if (sessions_.size() >= 4096) {
    error = "session catalogue limit reached";
    return {};
  }
  if (!PrivateDirectory(std::filesystem::path(path).parent_path().string()) ||
      !PrivateDirectory(directory_ + "/drafts")) {
    error = "cannot create private session directory";
    return {};
  }
  auto session = std::make_shared<Session>();
  session->cwd = cwd;
  session->path = path;
  session->id = HashHex(path);
  session->title = title.empty() ? "New conversation" : title;
  session->draft_title = title;
  session->status = "draft";
  session->updated = NowMillis();
  auto written =
      ToolWritePrivateFile(directory_ + "/drafts/" + session->id + ".json",
                           JsonDump({{"cwd", cwd},
                                     {"path", path},
                                     {"id", session->id},
                                     {"title", title},
                                     {"updated", session->updated}}));
  if (!written.Ok()) {
    error = "cannot persist draft identity";
    return {};
  }
  sessions_[session->id] = session;
  return session;
}
void Master::StartScheduledRun(const json& run) {
  const auto& definition = run["definition"];
  const auto cwd = JsonValue(run, "cwd", "");
  const auto id = JsonValue(run, "id", "");
  if (JsonValue(definition, "environment", "local") == "worktree") {
    auto created =
        CaptureProcess({"git", "-C", JsonValue(definition, "cwd", ""),
                        "worktree", "add", "--detach", cwd, "HEAD"},
                       30);
    if (!created.Ok()) {
      RecordRun({id, "failed",
                 "Cannot create worktree: " +
                     Utf8Prefix(created.output + created.error, 1024)});
      return;
    }
  }
  std::string error;
  {
    std::lock_guard lock(mutex_);
    auto session = NewSession(cwd, JsonValue(run, "session_path", ""),
                              JsonValue(run, "title", "Scheduled run"), error);
    if (session) {
      session->run_id = id;
      session->task_id = JsonValue(run, "task_id", "");
      session->launch = definition;
      session->launch_prompt = JsonValue(definition, "prompt", "");
      Activate(session, error);
      Publish(session->id, session->generation,
              {{"kind", "metadata"}, {"metadata", Metadata(*session)}});
    }
  }
  if (!error.empty()) RecordRun({id, "failed", error});
}
void Master::TickSchedules() {
  auto pending_updates = std::exchange(run_updates_, {});
  for (const auto& update : pending_updates) RecordRun(update);
  const auto library = SnapshotFile(LibraryChangePath());
  if (library != library_stamp_) {
    library_stamp_ = library;
    std::lock_guard lock(mutex_);
    Publish("", "", {{"kind", "management.changed"}});
  }
  const auto stamp = SnapshotFile(SchedulePath());
  if (stamp != schedule_stamp_) {
    schedule_stamp_ = stamp;
    schedule_state_ = ReadSchedules();
    auto scheduled = ScheduleControl({{"action", "list"}});
    std::lock_guard lock(mutex_);
    scheduled_view_ = std::move(scheduled);
    Publish("", "",
            {{"kind", "scheduled.changed"}, {"scheduled", scheduled_view_}});
  }
  if (!JsonArray(schedule_state_, "runs")) return;
  size_t slots = 0;
  std::vector<std::shared_ptr<Session>> closing;
  std::vector<RunUpdate> updates;
  {
    std::lock_guard lock(mutex_);
    size_t workers = 0;
    for (auto& [session_id, session] : sessions_) {
      if (session->pid > 0 && !session->exited) ++workers;
      if (session->run_id.empty()) continue;
      auto run =
          std::find_if(schedule_state_["runs"].begin(),
                       schedule_state_["runs"].end(), [&](const json& item) {
                         return JsonValue(item, "id", "") == session->run_id;
                       });
      if (run == schedule_state_["runs"].end()) continue;
      const auto prior = JsonValue(*run, "status", "");
      if (!ScheduledRunActive(prior)) continue;
      if (prior == "stopping" && !session->stop_sent) {
        session->stop_sent = true;
        session->launch_prompt.clear();
        session->Send({{"kind", "interrupt"}, {"request_id", RandomToken(16)}});
        session->run_result = "interrupted";
      }
      if (!session->launch_prompt.empty() && session->state.contains("route") &&
          !session->turn_active && session->pending.is_null()) {
        // The durable claim already exists. A crash never resubmits this run.
        auto prompt = std::exchange(session->launch_prompt, "");
        if (!session->Send({{"kind", "submit"},
                            {"request_id", session->run_id},
                            {"client_request_id", session->run_id},
                            {"text", prompt}})) {
          session->error = "cannot submit scheduled run";
          session->run_result = "failed";
        }
        updates.push_back({session->run_id, "running", ""});
      }
      if (!session->pending.is_null()) {
        updates.push_back({session->run_id, "waiting", ""});
      } else if (prior == "waiting") {
        updates.push_back({session->run_id, "running", ""});
      }
      bool background = false;
      if (const auto* activities = JsonArray(session->state, "activities")) {
        for (const auto& activity : *activities) {
          auto state =
              JsonValue(activity, "status", JsonValue(activity, "state", ""));
          if (state == "running" || state == "starting" ||
              state == "stopping" || state == "finishing") {
            background = true;
          }
        }
      }
      if (session->exited || !session->error.empty() ||
          (!session->run_result.empty() &&
           (session->run_checkpoint ||
            (session->stop_sent && !session->turn_active)) &&
           !session->turn_active && !background)) {
        auto status = !session->error.empty()       ? "failed"
                      : session->run_result.empty() ? "interrupted"
                                                    : session->run_result;
        if (UpdateScheduledRun(session->run_id, status, session->error)
                .contains("error")) {
          continue;
        }
        auto saved = SavedSession(*session);
        closing.push_back(std::exchange(session, saved));
        Publish(saved->id, "",
                {{"kind", "deactivated"}, {"metadata", Metadata(*saved)}});
      }
    }
    slots = workers < kWorkerLimit ? kWorkerLimit - workers : 0;
  }
  for (auto& session : closing) {
    session->Send({{"kind", "close"}, {"request_id", RandomToken(16)}});
  }
  closing.clear();
  for (const auto& update : updates) RecordRun(update);
  const int64_t now = NowMillis() / 1000;
  bool due = std::any_of(
      schedule_state_["runs"].begin(), schedule_state_["runs"].end(),
      [](const json& run) { return JsonValue(run, "status", "") == "queued"; });
  if (const auto* tasks = JsonArray(schedule_state_, "tasks")) {
    for (const auto& task : *tasks) {
      due |= JsonValue(task, "enabled", false) &&
             JsonValue(task, "next", int64_t{0}) > 0 &&
             JsonValue(task, "next", int64_t{0}) <= now;
    }
  }
  if (due) {
    auto claimed = ClaimScheduledRuns(now, slots);
    if (const auto* runs = JsonArray(claimed, "runs")) {
      for (const auto& run : *runs) StartScheduledRun(run);
    }
  }
}

void Master::Snapshot(const Request& request, Response& response) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(request.matches[1].str());
    if (found == sessions_.end()) {
      Error(response, "unknown session", 404);
      return;
    }
    session = found->second;
    if (session->pid > 0 && !request.has_param("before") &&
        !request.has_param("detail") && !request.has_param("http")) {
      Reply(response, LiveSnapshot(*session));
      return;
    }
  }
  if (request.has_param("http")) {
    json exchange;
    const std::string id = request.get_param_value("http");
    {
      std::lock_guard lock(mutex_);
      exchange = FindHttpExchange(session->state, id);
    }
    if (exchange.is_null()) {
      std::lock_guard reading(history_mutex_);
      auto loaded = SessionStore::Inspect(session->path);
      if (loaded.record) {
        const json& display = loaded.record->state.display;
        if (id == "latest") {
          exchange = FindHttpExchange(
              JsonValue(JsonValue(display, "facts", json::object()),
                        "http-latest", json::object()),
              id);
        } else {
          exchange = FindHttpExchange(display, id);
        }
      }
    }
    if (exchange.is_null()) {
      Error(response, "HTTP exchange was not captured", 404);
      return;
    }
    std::string part = request.get_param_value("part");
    if (part != "request" && part != "response") {
      Error(response, "choose request or response", 400);
      return;
    }
    int64_t offset = 0;
    ParseInt64(request.get_param_value("offset").c_str(), offset);
    json body =
        ReadPrivateArtifact(JsonValue(exchange, (part + "_path").c_str(), ""),
                            static_cast<size_t>(std::max(int64_t{0}, offset)));
    body["exchange"] = exchange;
    Reply(response, body, body.contains("error") ? 404 : 200);
    return;
  }
  if (request.has_param("raw") && request.has_param("detail")) {
    std::string path;
    {
      std::lock_guard lock(mutex_);
      const json* view = JsonObject(session->state, "view");
      const json* blocks = view ? JsonArray(*view, "blocks") : nullptr;
      if (blocks) {
        for (const json& block : *blocks) {
          if (JsonValue(block, "detail_id", "") ==
              request.get_param_value("detail")) {
            path = JsonValue(block, "exchange_path", "");
          }
        }
      }
    }
    if (!path.empty()) {
      int64_t offset = 0;
      ParseInt64(request.get_param_value("offset").c_str(), offset);
      json body = ReadPrivateArtifact(
          path, static_cast<size_t>(std::max(int64_t{0}, offset)));
      Reply(response, body, body.contains("error") ? 404 : 200);
      return;
    }
  }
  // One bounded history deserialization at a time; no master/IPC lock during
  // IO.
  std::lock_guard reading(history_mutex_);
  auto loaded = SessionStore::Inspect(session->path);
  json state = json::object();
  if (loaded.record) {
    auto& record = *loaded.record;
    Conversation conversation;
    if (!conversation.Restore(std::move(record.state.messages),
                              std::move(record.state.message_kinds),
                              std::move(record.state.archive),
                              record.state.archive_dropped_segments,
                              std::move(record.state.tool_displays),
                              record.state.display)) {
      Error(response, "invalid conversation metadata", 422);
      return;
    }
    int64_t position = 0;
    if (request.has_param("detail")) {
      ParseInt64(request.get_param_value("offset").c_str(), position);
      size_t offset = static_cast<size_t>(std::max(int64_t{0}, position));
      json detail =
          request.has_param("raw")
              ? ConversationExchange(conversation,
                                     request.get_param_value("detail"), offset)
          : request.has_param("artifact")
              ? ReadPrivateArtifact(
                    JsonValue(
                        JsonValue(conversation.DisplayFacts(),
                                  request.get_param_value("detail").c_str(),
                                  json::object()),
                        "artifact", ""),
                    offset)
              : ConversationDetail(conversation,
                                   request.get_param_value("detail"), offset);
      Reply(response, detail, detail.contains("error") ? 404 : 200);
      return;
    }
    ParseInt64(request.get_param_value("before").c_str(), position);
    state = {
        {"view", ConversationView(conversation, static_cast<uint64_t>(std::max(
                                                    int64_t{0}, position)))},
        {"usage", UsageJson(record.state.usage)},
        {"context_tokens", record.state.context_tokens},
        {"model", record.metadata.model},
        {"turns", record.metadata.turns},
        {"statistics", conversation.Statistics()},
        {"http", JsonValue(JsonValue(conversation.DisplayFacts(), "http-latest",
                                     json::object()),
                           "http", json::array())}};
  } else if (PathExists(session->path)) {
    Error(response, loaded.status.message, 422);
    return;
  }
  std::lock_guard lock(mutex_);
  auto owner = sessions_.find(session->id);
  if (owner != sessions_.end()) {
    session = owner->second;
  }
  if (session->pid > 0 && !request.has_param("before")) {
    Reply(response, LiveSnapshot(*session));
    return;
  }
  Reply(response, {{"v", kProtocol},
                   {"epoch", epoch_},
                   {"cursor", sequence_},
                   {"metadata", Metadata(*session)},
                   {"state", state},
                   {"pending", session->pending},
                   {"live", json::array()}});
}

void Master::Command(const Request& request, Response& response) {
  if (request.body.size() > kFrameBytes) {
    Error(response, "command exceeds limit", 413);
    return;
  }
  json command = json::parse(request.body, nullptr, false);
  const auto category = JsonValue(command, "kind", "");
  if (request.body.size() > size_t{64} * 1024 && category != "memory" &&
      category != "skills") {
    Error(response, "command exceeds limit", 413);
    return;
  }
  std::string request_id = JsonValue(command, "request_id", "");
  if (!command.is_object() || !OpaqueId(request_id) ||
      JsonValue(command, "v", 0) != kProtocol) {
    Error(response, "invalid command or protocol; refresh the app", 409);
    return;
  }
  std::unique_lock lock(mutex_);
  std::string device = DeviceId(request);
  if (device.empty()) {
    Error(response, "device revoked", 401);
    return;
  }
  std::string key = device + ":" + request_id;
  std::string fingerprint = HashHex(request.body);
  if (auto prior = requests_.find(key); prior != requests_.end()) {
    if (prior->second.fingerprint != fingerprint) {
      Error(response, "request ID reused with different content", 409);
    } else {
      Reply(response, prior->second.outcome);
    }
    return;
  }
  // Evict only completed receipts; never forget an unacknowledged submission.
  if (request_order_.size() >= 256) {
    auto completed =
        std::find_if(request_order_.begin(), request_order_.end(),
                     [&](const std::string& id) {
                       const auto& receipt = requests_.at(id);
                       return !receipt.handling &&
                              !JsonValue(receipt.outcome, "pending", false);
                     });
    if (completed == request_order_.end()) {
      Error(response, "too many unacknowledged commands", 429);
      return;
    }
    requests_.erase(*completed);
    request_order_.erase(completed);
  }
  std::string kind = JsonValue(command, "kind", "");
  json outcome = {{"request_id", request_id}, {"accepted", true}};
  // Reserve before releasing the mutex for any pipe write or process join.
  // A concurrent retry observes this same request instead of enqueueing again.
  requests_[key] = {
      fingerprint,
      {{"request_id", request_id}, {"accepted", true}, {"pending", true}}};
  request_order_.push_back(key);
  std::string error;
  if (kind == "logout" || kind == "revoke_device") {
    std::string remove =
        kind == "logout" ? device : JsonValue(command, "device_id", "");
    auto old = devices_;
    std::erase_if(devices_,
                  [&](const Device& entry) { return entry.id == remove; });
    if (!SaveDevices()) {
      devices_ = std::move(old);
      error = "cannot persist revocation";
    } else {
      push_->Revoke(remove);
      if (remove == device) {
        response.set_header(
            "Set-Cookie",
            "uagent_device=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
      }
    }
    changed_.notify_all();
  } else if (kind == "test_notification") {
    std::string session_id = JsonValue(command, "session_id", "");
    if (!sessions_.contains(session_id)) {
      error = "select a session before testing notifications";
    } else if (!push_->Notify(epoch_ + ":" + request_id, session_id, device)) {
      error = "push is unavailable or queue is full";
    }
  } else if (kind == "memory" || kind == "skills" || kind == "schedule" ||
             kind == "models") {
    lock.unlock();
    auto result = ControlProcess(command);
    lock.lock();
    outcome["result"] = result;
    error = JsonValue(result, "error", "");
    auto action = JsonValue(command, "action", "list");
    if (error.empty() && action != "list" && action != "get" &&
        action != "preview") {
      Publish("", "", {{"kind", "management.changed"}});
    }
  } else if (kind == "create") {
    std::error_code ec;
    auto cwd = std::filesystem::canonical(JsonValue(command, "cwd", ""), ec);
    if (ec || !std::filesystem::is_directory(cwd, ec)) {
      error = "choose an accessible directory on the host";
    } else {
      auto path = UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd.string()) +
                  "/web-" + RandomToken(16) + ".json";
      auto session = NewSession(cwd.string(), path, "", error);
      if (session) outcome["session"] = Metadata(*session);
    }
  } else if (kind == "config" && JsonValue(command, "session_id", "").empty()) {
    lock.unlock();
    auto manager = ConfigManager::Capture(false, {});
    auto configured = manager.Read();
    auto result =
        ConfigurationControl(command, manager, configured.config, false);
    lock.lock();
    outcome["result"] = result;
    error = JsonValue(result, "error", "");
  } else {
    auto found = sessions_.find(JsonValue(command, "session_id", ""));
    if (found == sessions_.end()) {
      error = "unknown session";
    } else {
      auto session = found->second;
      if (session->status == "updating" || session->status == "deleting") {
        error = "conversation update in progress";
      } else if (kind == "fork" && session->pid <= 0) {
        if (JsonValue(command, "generation", "") != session->generation) {
          error = "stale session; refresh before acting";
        } else if (sessions_.size() >= 4096) {
          error = "session catalogue limit reached";
        } else {
          lock.unlock();
          json fork = SessionStore::Fork(session->path,
                                         JsonValue(command, "title", ""));
          RefreshCatalogue(true);
          lock.lock();
          outcome["result"] = fork;
          error = JsonValue(fork, "error", "");
        }
      } else if ((kind == "rename" || kind == "delete") && session->pid <= 0) {
        if (JsonValue(command, "generation", "") != session->generation) {
          error = "stale session; refresh before acting";
        } else {
          std::string title = Trim(JsonValue(command, "title", ""));
          if (kind == "rename" && !ValidSessionTitle(title)) {
            error = "title must be 1–256 bytes without control characters";
          } else {
            std::string prior_status = session->status;
            session->status = kind == "delete" ? "deleting" : "updating";
            lock.unlock();
            std::unique_lock scan(scan_mutex_);
            std::unique_lock assets(asset_mutex_);
            std::string draft_path =
                directory_ + "/drafts/" + session->id + ".json";
            SessionStoreStatus result;
            if (kind == "delete") {
              result = SessionStore::Remove(session->path, draft_path);
            } else if (PathExists(session->path)) {
              result = SessionStore::Rename(session->path, title);
            } else {
              auto written = ToolWritePrivateFile(
                  draft_path, JsonDump({{"id", session->id},
                                        {"path", session->path},
                                        {"cwd", session->cwd},
                                        {"title", title},
                                        {"updated", NowMillis()}}));
              if (!written.Ok()) error = "cannot save conversation title";
            }
            if (!result.Ok()) error = result.message;
            lock.lock();
            session->status = prior_status;
            if (error.empty() && kind == "delete") {
              sessions_.erase(session->id);
              assets_scanned_ = {};
              Publish(session->id, "", {{"kind", "deleted"}});
            } else if (error.empty()) {
              session->title = title;
              session->draft_title = title;
              session->updated = NowMillis();
              Publish(session->id, "",
                      {{"kind", "metadata"}, {"metadata", Metadata(*session)}});
            }
          }
        }
      } else if (kind == "delete") {
        error = "stop and close this conversation before deleting it";
      } else if (kind == "activate") {
        if (Activate(session, error)) {
          outcome["session"] = Metadata(*session);
        }
      } else if (JsonValue(command, "generation", "") != session->generation ||
                 session->generation.empty()) {
        error = "stale worker generation; refresh before acting";
      } else if (kind == "close") {
        auto recorded = session->run_id.empty()
                            ? json::object()
                            : UpdateScheduledRun(session->run_id, "interrupted",
                                                 "Session closed by user.");
        if (recorded.contains("error")) {
          error = JsonValue(recorded, "error", "cannot record interruption");
        } else {
          // Keep the catalog entry while releasing process ownership outside
          // the lock.
          auto saved = SavedSession(*session);
          found->second = saved;
          FailPending(*session);
          lock.unlock();
          session->Send({{"kind", "close"}, {"request_id", request_id}});
          session.reset();
          lock.lock();
          Publish(saved->id, "",
                  {{"kind", "deactivated"}, {"metadata", Metadata(*saved)}});
        }
      } else if (session->pid <= 0 || session->exited) {
        error = "session needs activation";
      } else if (kind == "submit" || kind == "steer" || kind == "interrupt" ||
                 kind == "reply" || kind == "refresh" || kind == "rename" ||
                 kind == "model" || kind == "activity" ||
                 kind == "permissions" || kind == "config" ||
                 kind == "context" || kind == "fork") {
        command["attachments"] = json::array();
        if (const json* ids = JsonArray(command, "attachment_ids");
            ids && !ids->empty()) {
          // An upload can be waiting on its HTTP body. Do not let its asset
          // lock stall Stop, decisions or event delivery for any workspace.
          lock.unlock();
          std::unique_lock assets(asset_mutex_);
          if (ids->size() > kUploadCount) {
            error = "too many attachments";
          }
          if (!PrivateDirectory(session->path + ".assets")) {
            error = "unsafe asset directory";
          }
          for (const json& id : *ids) {
            if (!error.empty()) {
              break;
            }
            if (!id.is_string() || !OpaqueId(id.get<std::string>())) {
              error = "invalid asset ID";
              break;
            }
            std::string stem =
                session->path + ".assets/" + id.get<std::string>();
            std::string metadata, read_error;
            if (!ReadRegularFile(stem + ".json", 1024, metadata, read_error)) {
              error = "attachment is unavailable";
              break;
            }
            json asset = json::parse(metadata, nullptr, false);
            std::string extension =
                JsonValue(asset, "extension",
                          ImageExtension(JsonValue(asset, "mime", "")));
            if (extension.empty() ||
                (extension != ".data" &&
                 extension != ImageExtension(JsonValue(asset, "mime", "")))) {
              error = "invalid file asset";
              break;
            }
            // Claim before publishing a prompt. A crash can retain an unused
            // image, but cannot expire one already delivered to the agent.
            asset["committed"] = true;
            if (!ToolWritePrivateFile(stem + ".json", JsonDump(asset)).Ok()) {
              error = "cannot claim attachment";
              break;
            }
            command["attachments"].push_back(
                {{"path", stem + extension},
                 {"id", id},
                 {"name", JsonValue(asset, "name", "attachment" + extension)},
                 {"mime", JsonValue(asset, "mime", "application/octet-stream")},
                 {"image", JsonValue(asset, "image", extension != ".data")}});
          }
          assets.unlock();
          lock.lock();
          if (DeviceId(request) != device ||
              (!sessions_.contains(session->id) ||
               sessions_.at(session->id) != session) ||
              session->exited) {
            error =
                "session or device changed while claiming attachments; refresh";
          }
        }
        if (session->awaiting.size() >= 32) {
          error = "too many unacknowledged worker commands";
        }
        if (error.empty() && JsonDump(command).size() > kCommandBytes) {
          error = "message and resolved attachments exceed the command limit";
        }
        if (error.empty()) {
          // IPC is bounded and per-session; never hold the shared event lock on
          // a pipe write.
          std::string worker_request = RandomToken(16);
          command["client_request_id"] = request_id;
          command["request_id"] = worker_request;
          session->awaiting[worker_request] = {key, request_id};
          lock.unlock();
          bool sent = session->Send(command);
          lock.lock();
          if (!sent) {
            session->awaiting.erase(worker_request);
            error = "worker is disconnected";
          } else {
            changed_.wait_for(lock, std::chrono::seconds(2), [&] {
              return !JsonValue(requests_.at(key).outcome, "pending", false) ||
                     session->exited;
            });
            outcome = requests_.at(key).outcome;
            if (!JsonValue(outcome, "accepted", false)) {
              error = JsonValue(outcome, "error", "command rejected");
            }
          }
        }
      } else {
        error = "unsupported command";
      }
    }
  }
  outcome["accepted"] = error.empty();
  if (!error.empty()) {
    outcome["error"] = error;
  }
  requests_.at(key).outcome = outcome;
  requests_.at(key).handling = false;
  Reply(response, outcome, error.empty() ? 200 : 409);
}

void Master::Events(const Request& request, Response& response) {
  std::unique_lock lock(mutex_);
  if (sse_count_ >= kSseLimit) {
    Error(response, "event connection limit reached", 429);
    return;
  }
  std::string device = DeviceId(request);
  std::string cursor = request.get_header_value("Last-Event-ID");
  if (cursor.empty()) {
    cursor = request.get_param_value("cursor");
  }
  size_t colon = cursor.find(':');
  int64_t parsed = 0;
  bool valid =
      colon != std::string::npos && cursor.substr(0, colon) == epoch_ &&
      ParseInt64(cursor.substr(colon + 1).c_str(), parsed) && parsed >= 0;
  uint64_t next = static_cast<uint64_t>(std::max(int64_t{0}, parsed));
  ++sse_count_;
  response.set_header("X-Accel-Buffering", "no");
  response.set_chunked_content_provider(
      "text/event-stream",
      [this, device, next, valid](size_t, httplib::DataSink& sink) mutable {
        std::unique_lock guard(mutex_);
        auto authorized = [&] {
          return std::any_of(
              devices_.begin(), devices_.end(), [&](const Device& entry) {
                return entry.id == device && entry.expires > NowMillis();
              });
        };
        if (stopping_ || !authorized()) {
          return false;
        }
        if (!valid || next > sequence_ ||
            (!ring_.empty() && next + 1 < ring_.front().sequence)) {
          guard.unlock();
          std::string reset = "event: resync\ndata: {}\n\n";
          if (!sink.write(reset.data(), reset.size())) {
            return false;
          }
          sink.done();
          return true;
        }
        changed_.wait_for(guard, std::chrono::seconds(15), [&] {
          return stopping_ || next < sequence_ || !authorized();
        });
        if (stopping_ || !authorized()) {
          return false;
        }
        if (!ring_.empty() && next + 1 < ring_.front().sequence) {
          valid = false;
          return true;
        }
        std::string batch;
        for (const Replay& event : ring_) {
          if (event.sequence <= next) {
            continue;
          }
          batch += "id: " + epoch_ + ":" + std::to_string(event.sequence) +
                   "\nevent: update\ndata: " + event.frame + "\n\n";
          next = event.sequence;
          if (batch.size() >= size_t{64} * 1024) {
            break;
          }
        }
        if (batch.empty()) {
          batch = ": heartbeat\n\n";
        }
        guard.unlock();
        return sink.is_writable() && sink.write(batch.data(), batch.size());
      },
      [this](bool) {
        std::lock_guard guard(mutex_);
        --sse_count_;
      });
}

void Master::Upload(const Request& request, Response& response,
                    const httplib::ContentReader& reader) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(request.matches[1].str());
    if (found == sessions_.end()) {
      Error(response, "unknown session", 404);
      return;
    }
    session = found->second;
  }
  // Serialize asset accounting without blocking event readers or controls.
  std::lock_guard assets(asset_mutex_);
  std::string folder = session->path + ".assets";
  if (!PrivateDirectory(folder)) {
    Error(response, "cannot create asset storage", 500);
    return;
  }
  size_t global = GlobalAssetUsage();
  AssetUsage usage = InspectAssets(folder, true);
  if (!usage.valid || usage.count >= 64 || usage.bytes >= kSessionAssetBytes ||
      global >= kGlobalAssetBytes) {
    Error(response, "attachment storage limit reached", 413);
    return;
  }
  std::string bytes;
  size_t limit = std::min({kUploadBytes, kSessionAssetBytes - usage.bytes,
                           kGlobalAssetBytes - global});
  bool read = reader([&](const char* data, size_t length) {
    if (length > limit - bytes.size()) {
      return false;
    }
    bytes.append(data, length);
    return true;
  });
  if (!read) {
    Error(response, "upload exceeds file/session limit", 413);
    return;
  }
  std::string mime = RasterMime(bytes);
  const bool image = !mime.empty();
  std::string name =
      Utf8Prefix(std::filesystem::path(request.get_param_value("name"))
                     .filename()
                     .string(),
                 240);
  if (name.empty()) {
    name = image ? "image" + ImageExtension(mime) : "attachment";
  }
  for (char& ch : name) {
    if (static_cast<unsigned char>(ch) < 32) ch = '_';
  }
  if (mime.empty()) {
    mime = AttachmentMime(name);
    if (mime.starts_with("image/")) mime = "application/octet-stream";
  }

  std::string id = RandomToken(16), stem = folder + "/" + id;
  if (id.empty() || !ToolWritePrivateFile(stem + ".data", bytes).Ok()) {
    Error(response, "cannot persist upload", 500);
    return;
  }
  asset_bytes_ += bytes.size();
  if (!ToolWritePrivateFile(stem + ".json", JsonDump({{"mime", mime},
                                                      {"name", name},
                                                      {"image", image},
                                                      {"extension", ".data"},
                                                      {"bytes", bytes.size()},
                                                      {"committed", false}}))
           .Ok()) {
    Error(response, "cannot persist upload metadata", 500);
    return;
  }
  Reply(response, {{"id", id},
                   {"name", name},
                   {"mime", mime},
                   {"image", image},
                   {"bytes", bytes.size()}});
}

void Master::AssetRead(const Request& request, Response& response) {
  std::string path;
  {
    std::lock_guard lock(mutex_);
    auto found = sessions_.find(request.matches[1].str());
    if (found == sessions_.end()) {
      Error(response, "unknown session", 404);
      return;
    }
    path = found->second->path;
  }
  std::string stem = path + ".assets/" + request.matches[2].str();
  if (!PrivateDirectory(path + ".assets")) {
    Error(response, "asset unavailable", 404);
    return;
  }
  std::string metadata, bytes, error;
  if (!ReadRegularFile(stem + ".json", 1024, metadata, error)) {
    Error(response, "asset unavailable", 404);
    return;
  }
  json asset = json::parse(metadata, nullptr, false);
  std::string mime = JsonValue(asset, "mime", ""),
              extension = JsonValue(asset, "extension", ImageExtension(mime));
  if ((extension != ".data" &&
       (extension.empty() || extension != ImageExtension(mime))) ||
      !ReadRegularFile(stem + extension, kUploadBytes, bytes, error)) {
    Error(response, "asset unavailable", 404);
    return;
  }
  bool image = JsonValue(asset, "image", !ImageExtension(mime).empty());
  if (image && RasterMime(bytes) != mime) {
    Error(response, "invalid image asset", 415);
    return;
  }
  if (!image) response.set_header("Content-Disposition", "attachment");
  response.set_content(std::move(bytes),
                       image ? mime : "application/octet-stream");
}

}  // namespace

int MasterMain(const WebOptions& options, const char* executable) {
  std::string directory = GlobalBase() + "/web";
  if (!PrivateDirectory(directory)) {
    fprintf(stderr,
            "web runtime directory must be private and owned by this user\n");
    return 1;
  }
  FileLease lease;
  std::string error;
  CurlRuntime curl;
  if (!curl.Ready()) {
    return 1;
  }
  if (!lease.Acquire(directory + "/master.lock", error, true)) {
    // The lock, not a PID/port/metadata file, is authority. A contender only
    // authenticates the recorded instance; it never binds an alternate port.
    for (int attempt = 0; attempt < 20; ++attempt) {
      std::string content, read_error;
      if (ReadRegularFile(directory + "/discovery.json", 4096, content,
                          read_error)) {
        json discovery = json::parse(content, nullptr, false);
        int port = JsonValue(discovery, "port", 0);
        std::string secret = JsonValue(discovery, "secret", "");
        if (JsonValue(discovery, "v", 0) != kProtocol) {
          fprintf(stderr,
                  "existing master protocol differs; stop it explicitly before "
                  "upgrading\n");
          return 2;
        }
        if (port >= 1024 && port <= 65535 && OpaqueId(secret)) {
          httplib::Client client("127.0.0.1", port);
          client.set_connection_timeout(1);
          client.set_read_timeout(1);
          auto response =
              client.Post("/api/instance", {{"X-Uagent-Instance", secret}}, "",
                          "application/json");
          if (response && response->status == 200) {
            json identity = json::parse(response->body, nullptr, false);
            if (JsonValue(identity, "epoch", "") !=
                    JsonValue(discovery, "epoch", "") ||
                JsonValue(identity, "v", 0) != kProtocol) {
              break;
            }
            if (options.port != port ||
                (!options.origin.empty() &&
                 options.origin != JsonValue(identity, "url", ""))) {
              fprintf(stderr,
                      "existing master settings take precedence; using its "
                      "published origin\n");
            }
            printf(
                "µAgent web: %s\nPairing code (single use, 5 minutes): "
                "%s\nSuggested host directory: %s\n",
                JsonValue(identity, "url", "").c_str(),
                JsonValue(identity, "pairing_code", "").c_str(),
                CanonicalCwd().c_str());
            return 0;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fprintf(stderr,
            "master lock is held but authenticated discovery is unavailable; "
            "retry after startup or inspect the owning terminal\n");
    return 2;
  }
  SetExecutablePath(executable);
  Master master(options, ExecutablePath(), directory);
  if (!master.Initialize(error)) {
    fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  return master.Run();
}
}  // namespace uagent::web
