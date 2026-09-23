// Copyright 2026 Timon Gentzsch

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT

#include <arpa/inet.h>
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
#include "include/app/self_description.h"
#include "include/app/session_host.h"
#include "include/browser/browser.h"
#include "include/core/capture.h"
#include "include/core/child_env.h"
#include "include/core/effective_config.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/time.h"
#include "include/tools/files.h"
#include "include/web/assets.h"
#include "include/web/browser_viewer.h"
#include "include/web/protocol.h"
#include "include/web/push.h"

namespace uagent::web {
namespace {
using Clock = std::chrono::steady_clock;
using Request = httplib::Request;
using Response = httplib::Response;
// Browser-service policies live together here. They are operational limits,
// not inferred model or billing data.
constexpr size_t kOriginChars = 255;
constexpr size_t kSubscriptionRequestBytes = KiB(8);
constexpr size_t kAuthenticationRequestBytes = KiB(4);
constexpr size_t kDeviceStoreBytes = KiB(64);
constexpr size_t kRegularCommandBytes = KiB(64);
constexpr size_t kAssetMetadataBytes = KiB(1);
constexpr size_t kDiscoveryBytes = KiB(4);
constexpr size_t kDeviceNameChars = 80;
constexpr int kWorkerThreads = 12;
constexpr int kWorkerQueue = 24;
constexpr int kReadTimeoutSeconds = 5;
constexpr int kWriteTimeoutSeconds = 2;
constexpr int kKeepAliveRequests = 50;
constexpr int kKeepAliveTimeoutSeconds = 2;
constexpr int kAuthenticationAttempts = 12;
constexpr int kStartupAttempts = 20;
constexpr auto kPairingLifetime = std::chrono::minutes(5);
constexpr auto kAuthenticationWindow = std::chrono::minutes(1);
constexpr auto kDeviceLifetime = std::chrono::hours(24 * 30);
constexpr auto kReplayWait = std::chrono::seconds(15);
constexpr auto kStartupRetry = std::chrono::milliseconds(100);
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

struct Device {
  std::string id, token, name;
  int64_t expires = 0;
};
class Master {
 public:
  Master(WebOptions options, std::string executable, std::string directory)
      : options_(std::move(options)),
        executable_(std::move(executable)),
        directory_(std::move(directory)),
        local_("http://127.0.0.1:" + std::to_string(options_.port)),
        local_authority_(local_.substr(7)),
        origin_(options_.origin.empty() ? local_ : options_.origin),
        epoch_(RandomToken(16)),
        secret_(RandomToken()),
        host_(epoch_, kQueueBytes, executable_, directory_) {}

  bool Initialize(std::string& error) {
    if (epoch_.empty() || secret_.empty() || !host_wake_.Open()) {
      error = "OS randomness unavailable";
      return false;
    }
    if (!options_.origin.empty()) {
      // No path, query, userinfo or forwarding-header origin inference.
      bool http = origin_.starts_with("http://");
      size_t prefix = http ? 7 : 8;
      in_addr address{};
      std::string host =
          http ? origin_.substr(7, origin_.find(':', 7) - 7) : "";
      bool tailnet = http && inet_pton(AF_INET, host.c_str(), &address) == 1 &&
                     (ntohl(address.s_addr) & 0xffc00000U) == 0x64400000U;
      if ((!origin_.starts_with("https://") && !tailnet) ||
          origin_.size() <= prefix || origin_.size() > kOriginChars ||
          origin_.substr(prefix).find_first_of("/@?#\\ \t\r\n") !=
              std::string::npos) {
        error =
            "web origin must be bare HTTPS, or HTTP on a 100.64.0.0/10 "
            "tailnet address";
        return false;
      }
      std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(),
                                                              curl_url_cleanup);
      if (!url || curl_url_set(url.get(), CURLUPART_URL, origin_.c_str(), 0) !=
                      CURLUE_OK) {
        error = "web origin is not a valid URL";
        return false;
      }
    }
    authority_ = origin_.substr(origin_.find("://") + 3);
    if (!browser::StartService(executable_, browser_process_, error)) {
      return false;
    }
    LoadDevices();
    push_ = std::make_unique<PushSender>(directory_, options_.push_contact);
    host_.LoadDrafts();
    host_.RefreshCatalogue();
    host_.RefreshInvalidations({});
    server_.new_task_queue = [] {
      return new httplib::ThreadPool(kWorkerThreads, kWorkerThreads,
                                     kWorkerQueue);
    };
    server_.set_read_timeout(kReadTimeoutSeconds);
    server_.set_write_timeout(kWriteTimeoutSeconds);
    server_.set_keep_alive_max_count(kKeepAliveRequests);
    server_.set_keep_alive_timeout(kKeepAliveTimeoutSeconds);
    server_.set_payload_max_length(kUploadBytes);
    server_.set_default_headers(
        {{"X-Content-Type-Options", "nosniff"},
         {"Referrer-Policy", "no-referrer"},
         {"Cross-Origin-Resource-Policy", "same-origin"},
         {"Cache-Control", "no-store"},
         {"Content-Security-Policy",
          "default-src 'none'; script-src 'self'; style-src 'self' "
          "'unsafe-inline'; font-src 'self'; img-src 'self' blob: data:; "
          "connect-src "
          "'self'; manifest-src 'self'; worker-src 'self'; base-uri 'none'; "
          "form-action 'self'; frame-ancestors 'none'"}});
    server_.set_pre_routing_handler([this](const Request& request,
                                           Response& response) {
      const std::string* expected_origin = ExpectedOrigin(request);
      const std::string origin = request.get_header_value("Origin");
      const bool instance = request.path == "/api/instance";
      const bool mutation = request.method != "GET" && request.method != "HEAD";
      if (!expected_origin || (!origin.empty() && origin != *expected_origin) ||
          (mutation && !instance && origin != *expected_origin)) {
        Error(response, "invalid Host or Origin", 403);
        return httplib::Server::HandlerResponse::Handled;
      }
      if (request.path.starts_with("/api/") && request.path != "/api/auth" &&
          !instance) {
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
        host_.RefreshCatalogue(true);
      }
      host_.RefreshPresence();
      std::lock_guard lock(mutex_);
      json catalogue = host_.Catalogue();
      Reply(response, {{"v", kProtocol},
                       {"epoch", epoch_},
                       {"cursor", catalogue["cursor"]},
                       {"sessions", catalogue["sessions"]},
                       {"commands", CommandSchemaJson()},
                       {"capabilities", push_->Capabilities(DeviceId(request))},
                       {"devices", PublicDevices()},
                       {"scheduled", catalogue["scheduled"]},
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
    if (!browser::DataDirectory().empty()) {
      server_.Get("/api/browser/status",
                  [this](const Request& request, Response& response) {
                    json status = browser::Request({{"op", "status"}});
                    std::lock_guard lock(mutex_);
                    status["controller"] =
                        JsonValue(status, "viewer", "") == DeviceId(request);
                    status["leased"] = !JsonValue(status, "viewer", "").empty();
                    status.erase("viewer");
                    Reply(response, status);
                  });
      server_.WebSocket(
          "/api/browser/viewer",
          [this](const Request& request, httplib::ws::WebSocket& socket) {
            std::string device;
            const std::string role = request.has_param("role")
                                         ? request.get_param_value("role")
                                         : "control";
            {
              std::lock_guard lock(mutex_);
              const std::string* expected_origin = ExpectedOrigin(request);
              if (expected_origin &&
                  request.get_header_value("Origin") == *expected_origin) {
                device = DeviceId(request);
              }
            }
            if (device.empty() || (role != "control" && role != "observe") ||
                viewer_active_.exchange(true)) {
              socket.close(httplib::ws::CloseStatus::PolicyViolation);
              return;
            }
            uint64_t generation = RelayBrowserViewer(socket, device, role);
            if (generation && role == "control") {
              browser::Request({{"op", "viewer_disconnected"},
                                {"device", device},
                                {"generation", generation}});
            }
            viewer_active_ = false;
          });
    }
    server_.Get(R"(/api/receipts/([a-f0-9]{16,64}))",
                [this](const Request& request, Response& response) {
                  std::lock_guard lock(mutex_);
                  auto found = requests_.find(DeviceId(request) + ":" +
                                              request.matches[1].str());
                  if (found != requests_.end() &&
                      !found->second.worker_request.empty() &&
                      JsonValue(found->second.outcome, "pending", false)) {
                    found->second.outcome = host_.CommandOutcome(
                        found->second.worker_request, request.matches[1].str());
                  }
                  Reply(response,
                        found == requests_.end()
                            ? json{{"request_id", request.matches[1].str()},
                                   {"unknown", true}}
                            : found->second.outcome);
                });
    server_.Post("/api/push/subscriptions", [this](const Request& request,
                                                   Response& response) {
      if (request.body.size() > kSubscriptionRequestBytes) {
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
        response.set_header(
            "Cache-Control",
            path.starts_with("/assets/")
                ? "public, max-age=" + std::to_string(kSecondsPerYear) +
                      ", immutable"
                : "no-cache");
        response.set_content(reinterpret_cast<const char*>(asset.data),
                             asset.size, std::string(asset.mime));
        return;
      }
      Error(response, "not found", 404);
    });
    if (!server_.bind_to_port(options_.bind, options_.port)) {
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
    printf("uagent web: %s\nPairing code (single use, 5 minutes): %s\n",
           origin_.c_str(), pairing.c_str());
    fflush(stdout);
    std::thread stopping([&] {
      for (;;) {
        std::vector<std::string> paths = host_.PresencePaths();
        auto result = WaitForAnyFileChange(
            paths, Clock::now() + std::chrono::hours(24), stop.read.Get());
        if (result == FileWaitResult::kInterrupted) break;
        bool observed;
        {
          std::lock_guard lock(mutex_);
          observed = sse_count_ > 0;
        }
        if (observed) {
          host_.RefreshPresence();
        }
      }
      stopping_.store(true);
      host_.Stop();
      host_wake_.Wake();
      server_.stop();
    });
    std::thread scheduled([&] {
      bool recovered = false;
      for (;;) {
        if (stopping_) break;
        auto wait = host_.RunSchedules(recovered);
        if (wait.wake) host_wake_.Wake();
        WaitForAnyFileChange(wait.paths, wait.deadline, host_wake_.read.Get());
        host_wake_.Drain();
      }
    });
    std::thread notifications([&] {
      while (!stopping_) {
        auto notices = host_.WaitForNotices();
        for (const auto& notice : notices) {
          if (!notice.attention_id.empty()) {
            std::lock_guard lock(mutex_);
            for (const auto& device : devices_) {
              if (device.expires > NowMillis()) {
                push_->Notify(notice.attention_id, notice.session, device.id);
              }
            }
          }
          if (notice.wake) host_wake_.Wake();
        }
      }
    });
    bool served = server_.listen_after_bind();
    stop.Wake();
    stopping.join();
    scheduled.join();
    notifications.join();
    shutdown_fd = -1;
    // Browser service shutdown detaches; sessions belong to their runtimes.
    host_.Shutdown();
    browser_process_.owner.Reset();
    return served ? 0 : 1;
  }

 private:
  const std::string* ExpectedOrigin(const Request& request) const {
    // Keep loopback available for a local browser or an SSH-forwarded PWA while
    // preserving exact Host and Origin checks for the configured remote URL.
    const std::string host = request.get_header_value("Host");
    if (host == authority_) {
      return &origin_;
    }
    if (host == local_authority_) {
      return &local_;
    }
    return nullptr;
  }
  std::string Pair() {
    pair_ = RandomToken(12);
    pair_deadline_ = Clock::now() + kPairingLifetime;
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
    if (!ReadRegularFile(directory_ + "/devices.json", kDeviceStoreBytes, bytes,
                         error)) {
      return;
    }
    json value = json::parse(bytes, nullptr, false);
    if (!value.is_array() || value.size() > kWebDeviceLimit) {
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
    if (request.body.size() > kAuthenticationRequestBytes) {
      Error(response, "authentication request too large", 413);
      return;
    }
    json input = json::parse(request.body, nullptr, false);
    std::lock_guard lock(mutex_);
    auto now = Clock::now();
    if (now - auth_window_ > kAuthenticationWindow) {
      auth_window_ = now;
      auth_attempts_ = 0;
    }
    if (++auth_attempts_ > kAuthenticationAttempts) {
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
    if (devices_.size() >= kWebDeviceLimit) {
      Error(response, "device limit reached; revoke a device first", 409);
      return;
    }
    Device device{
        RandomToken(16), RandomToken(32),
        Utf8Prefix(JsonValue(input, "name", "Browser"), kDeviceNameChars),
        NowMillis() + std::chrono::duration_cast<std::chrono::milliseconds>(
                          kDeviceLifetime)
                          .count()};
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
    const std::string* expected_origin = ExpectedOrigin(request);
    response.set_header(
        "Set-Cookie",
        "uagent_device=" + device.token +
            "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                               kDeviceLifetime)
                               .count()) +
            (expected_origin && expected_origin->starts_with("https://")
                 ? "; Secure"
                 : ""));
    Reply(response, {{"v", kProtocol}, {"device", device.id}});
  }
  void Publish(const std::string& session, const std::string& generation,
               json value) {
    host_.Publish(session, generation, std::move(value));
  }
  void Snapshot(const Request&, Response&);
  void Command(const Request&, Response&);
  void Events(const Request&, Response&);
  void Upload(const Request&, Response&, const httplib::ContentReader&);
  void AssetRead(const Request&, Response&);

  WebOptions options_;
  std::string executable_, directory_, local_, local_authority_, origin_,
      authority_, epoch_, secret_, pair_;
  session::SessionHost host_;
  Clock::time_point pair_deadline_{}, auth_window_{}, scanned_{};
  int auth_attempts_ = 0;
  httplib::Server server_;
  std::mutex mutex_, scan_mutex_;
  std::map<std::string, FileStamp> history_directories_;
  Pipe host_wake_;
  std::vector<Device> devices_;
  size_t sse_count_ = 0;
  struct Receipt {
    std::string fingerprint;
    json outcome;
    bool handling = true;
    std::string worker_request;
  };
  std::map<std::string, Receipt> requests_;
  std::deque<std::string> request_order_;
  std::atomic<bool> stopping_{false};
  std::unique_ptr<PushSender> push_;
  browser::ServiceProcess browser_process_;
  std::atomic<bool> viewer_active_{false};
};

void Master::Snapshot(const Request& request, Response& response) {
  session::SnapshotQuery query;
  query.has_before = request.has_param("before");
  query.has_detail = request.has_param("detail");
  query.raw = request.has_param("raw");
  query.artifact = request.has_param("artifact");
  query.has_http = request.has_param("http");
  query.before = request.get_param_value("before");
  query.detail = request.get_param_value("detail");
  query.http = request.get_param_value("http");
  query.part = request.get_param_value("part");
  query.offset = request.get_param_value("offset");
  auto result = host_.Snapshot(request.matches[1].str(), query);
  Reply(response, result.value, result.status);
}

void Master::Command(const Request& request, Response& response) {
  if (request.body.size() > kFrameBytes) {
    Error(response, "command exceeds limit", 413);
    return;
  }
  json command = json::parse(request.body, nullptr, false);
  const auto category = JsonValue(command, "kind", "");
  if (request.body.size() > kRegularCommandBytes && category != "memory" &&
      category != "skills" && category != "prompt") {
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
      if (!prior->second.worker_request.empty() &&
          JsonValue(prior->second.outcome, "pending", false)) {
        prior->second.outcome =
            host_.CommandOutcome(prior->second.worker_request, request_id);
      }
      Reply(response, prior->second.outcome);
    }
    return;
  }
  // Evict only completed receipts; never forget an unacknowledged submission.
  if (request_order_.size() >= kWebRequestReceipts) {
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
  // Recall targets a queued steer by its own id, never the envelope id:
  // request ids are single-use receipts and reuse is rejected above.
  std::string target = JsonValue(command, "target_id", "");
  if (kind == "recall" && !OpaqueId(target)) {
    Error(response, "recall needs a queued guidance id", 409);
    return;
  }
  json outcome = {{"request_id", request_id}, {"accepted", true}};
  // Reserve before releasing the mutex for any pipe write or process join.
  // A concurrent retry observes this same request instead of enqueueing again.
  Receipt receipt;
  receipt.fingerprint = fingerprint;
  receipt.outcome = {
      {"request_id", request_id}, {"accepted", true}, {"pending", true}};
  requests_[key] = std::move(receipt);
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
  } else if (kind == "test_notification") {
    std::string session_id = JsonValue(command, "session_id", "");
    if (!host_.Contains(session_id)) {
      error = "select a session before testing notifications";
    } else if (!push_->Notify(epoch_ + ":" + request_id, session_id, device)) {
      error = "push is unavailable or queue is full";
    }
  } else if (kind == "browser" && !browser::DataDirectory().empty()) {
    const std::string action = JsonValue(command, "action", "");
    const std::string interaction = JsonValue(command, "interaction_id", "");
    json browser_command = {
        {"op", action == "done" ? "prepare_done" : action},
        {"device", device},
        {"interaction_id", interaction},
        {"name", JsonValue(command, "name", "")},
        {"profile_id", JsonValue(command, "profile_id", "")}};
    if (action != "takeover" && action != "done" && action != "stop" &&
        action != "create_profile" && action != "select_profile") {
      error = "unsupported browser control";
    } else {
      lock.unlock();
      json result = browser::Request(browser_command, 30000);
      std::string control_error = JsonValue(result, "error", "");
      if (control_error.empty() && action == "done") {
        const std::string session_id = JsonValue(result, "session_id", "");
        if (!interaction.empty() && session_id.empty()) {
          control_error = "browser conversation is missing";
        } else if (!interaction.empty()) {
          json reply = {{"kind", "reply"},
                        {"v", kProtocol},
                        {"session_id", session_id},
                        {"generation", JsonValue(command, "generation", "")},
                        {"interaction_id", interaction},
                        {"text", "done"},
                        {"request_id", request_id}};
          auto executed = host_.ExecuteCommand(reply, device, request_id);
          control_error = executed.error;
          if (control_error.empty()) {
            if (executed.wake) host_wake_.Wake();
          }
        }
        if (control_error.empty()) {
          result = browser::Request({{"op", "done"},
                                     {"device", device},
                                     {"interaction_id", interaction}});
          control_error = JsonValue(result, "error", "");
          if (control_error == "browser service timed out or disconnected") {
            json current = browser::Request({{"op", "status"}}, 1000);
            if (current.value("ok", false) &&
                JsonValue(current, "mode", "") != "human" &&
                JsonValue(current, "interaction_id", "").empty() &&
                JsonValue(current, "session_id", "") == session_id) {
              result = std::move(current);
              control_error.clear();
            }
          }
        }
      }
      lock.lock();
      if (DeviceId(request) != device) control_error = "device revoked";
      error = std::move(control_error);
      outcome["result"] = result;
    }
  } else if (kind == "memory" || kind == "skills" || kind == "schedule" ||
             kind == "models" || kind == "permission_rules" ||
             kind == "tool_categories" ||
             (kind == "prompt" &&
              JsonValue(command, "session_id", "").empty())) {
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
    lock.unlock();
    auto executed =
        host_.ExecuteCommand(std::move(command), device, request_id);
    lock.lock();
    if (DeviceId(request) != device) {
      executed.error = "device changed while handling command; refresh";
    }
    outcome = std::move(executed.outcome);
    error = std::move(executed.error);
    if (!executed.worker_request.empty()) {
      requests_.at(key).worker_request = std::move(executed.worker_request);
    }
    if (executed.wake) host_wake_.Wake();
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
  if (sse_count_ >= kWebSseConnections) {
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
  const uint64_t replay_watermark = host_.Cursor();
  ++sse_count_;
  response.set_header("X-Accel-Buffering", "no");
  response.set_chunked_content_provider(
      "text/event-stream",
      [this, device, next, valid, replay_watermark, ready_sent = false](
          size_t, httplib::DataSink& sink) mutable {
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
        auto replay = host_.ReadReplay(next, valid, replay_watermark);
        if (replay.reset) {
          guard.unlock();
          std::string reset = "event: resync\ndata: {}\n\n";
          if (!sink.write(reset.data(), reset.size())) {
            return false;
          }
          sink.done();
          return true;
        }
        if (ready_sent) {
          guard.unlock();
          host_.WaitForReplay(next, kReplayWait);
          guard.lock();
          if (stopping_ || !authorized()) return false;
        }
        replay = host_.ReadReplay(
            next, valid, ready_sent ? host_.Cursor() : replay_watermark);
        if (replay.reset) {
          valid = false;
          return true;
        }
        std::string batch;
        for (const session::HostReplay& event : replay.events) {
          batch += "id: " + epoch_ + ":" + std::to_string(event.sequence) +
                   "\nevent: update\ndata: " + event.frame + "\n\n";
          next = event.sequence;
          if (batch.size() >= kWebEventBatchBytes) {
            break;
          }
        }
        if (!ready_sent && next >= replay_watermark) {
          batch += "event: ready\ndata: " +
                   JsonDump({{"epoch", epoch_}, {"cursor", replay_watermark}}) +
                   "\n\n";
          ready_sent = true;
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
  std::string bytes;
  bool read = reader([&](const char* data, size_t length) {
    if (length > kUploadBytes - bytes.size()) return false;
    bytes.append(data, length);
    return true;
  });
  if (!read) {
    Error(response, "upload exceeds file/session limit", 413);
    return;
  }
  auto stored = host_.StoreAsset(request.matches[1].str(), bytes,
                                 request.get_param_value("name"));
  if (!stored.error.empty()) {
    Error(response, stored.error, stored.status);
    return;
  }
  Reply(response, stored.value);
}

void Master::AssetRead(const Request& request, Response& response) {
  std::string path;
  {
    std::lock_guard lock(mutex_);
    path = host_.AssetPath(request.matches[1].str());
    if (path.empty()) {
      Error(response, "unknown session", 404);
      return;
    }
  }
  std::string stem = path + ".assets/" + request.matches[2].str();
  if (!EnsurePrivateDirectory(path + ".assets")) {
    Error(response, "asset unavailable", 404);
    return;
  }
  std::string metadata, bytes, error;
  if (!ReadRegularFile(stem + ".json", kAssetMetadataBytes, metadata, error)) {
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
  if (image && RasterMime(bytes) != mime && SvgMime(bytes) != mime) {
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
  if (!EnsurePrivateDirectory(directory)) {
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
    for (int attempt = 0; attempt < kStartupAttempts; ++attempt) {
      std::string content, read_error;
      if (ReadRegularFile(directory + "/discovery.json", kDiscoveryBytes,
                          content, read_error)) {
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
                "uagent web: %s\nPairing code (single use, 5 minutes): "
                "%s\nSuggested host directory: %s\n",
                JsonValue(identity, "url", "").c_str(),
                JsonValue(identity, "pairing_code", "").c_str(),
                CanonicalCwd().c_str());
            return 0;
          }
        }
      }
      std::this_thread::sleep_for(kStartupRetry);
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
