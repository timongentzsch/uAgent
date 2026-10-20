// Copyright 2026 Timon Gentzsch

#include <arpa/inet.h>
#include <curl/curl.h>
#include <signal.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/bootstrap.h"
#include "include/app/self_description.h"
#include "include/app/session_host.h"
#include "include/browser/browser.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/private_store.h"
#include "include/core/settings_store.h"
#include "include/core/signals.h"
#include "include/core/thread_annotations.h"
#include "include/core/verbosity.h"
#include "include/tools/files.h"
#include "include/web/assets.h"
#include "include/web/browser_viewer.h"
#include "include/web/protocol.h"
#include "include/web/push.h"
#include "src/web/master_internal.h"

namespace uagent::web {
namespace {
// Browser-service policies live together here. They are operational limits,
// not inferred model or billing data.
constexpr size_t kOriginChars = 255;
constexpr size_t kSubscriptionRequestBytes = KiB(8);
constexpr size_t kDiscoveryBytes = KiB(4);
// Four workers wait while idle. Busy, the pool grows to one per event stream
// and viewer, which each hold theirs as long as they stay open, plus twelve
// for requests, so those connections cannot starve the API.
constexpr size_t kIdleWorkerThreads = 4;
constexpr size_t kRequestWorkerThreads = 12;
constexpr size_t kWorkerQueue = 24;
constexpr int kReadTimeoutSeconds = 5;
constexpr int kWriteTimeoutSeconds = 2;
constexpr int kKeepAliveRequests = 50;
constexpr int kKeepAliveTimeoutSeconds = 2;
constexpr int kStartupAttempts = 20;
constexpr auto kStartupRetry = std::chrono::milliseconds(100);
constexpr auto kRestartReply = std::chrono::milliseconds(300);
void StopSignal(int) { WakeDescriptor(master::shutdown_fd); }

// The code, and a link that pairs a browser with it on opening: the fragment
// never reaches a server, and the page drops it once it has paired.
std::string PairingText(const std::string& origin, const std::string& code) {
  return "uagent web: " + origin +
         "\nPairing code (single use, 5 minutes): " + code +
         "\nOr open: " + origin + "/#pair=" + code + "\n";
}

// Whether the client takes gzip: an explicit gzip entry decides, else `*`;
// a q of 0 refuses. Codings match case-insensitively and whole.
bool AcceptsGzip(const std::string& header) {
  auto trim = [](std::string& text) {
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
  };
  int gzip = -1, any = -1;
  size_t at = 0;
  while (at <= header.size()) {
    size_t end = std::min(header.find(',', at), header.size());
    std::string entry = header.substr(at, end - at);
    at = end + 1;
    size_t semi = entry.find(';');
    std::string coding = entry.substr(0, semi);
    std::string params =
        semi == std::string::npos ? "" : entry.substr(semi + 1);
    trim(coding);
    trim(params);
    std::transform(coding.begin(), coding.end(), coding.begin(), ::tolower);
    int accepted = 1;
    if (params.size() > 2 && (params[0] == 'q' || params[0] == 'Q') &&
        params[1] == '=') {
      accepted = strtod(params.c_str() + 2, nullptr) > 0 ? 1 : 0;
    }
    if (coding == "gzip" || coding == "x-gzip") {
      gzip = accepted;
    } else if (coding == "*") {
      any = accepted;
    }
  }
  return (gzip >= 0 ? gzip : any) == 1;
}

// The asset's plain bytes, exactly plain_size of them; false on any fault.
bool Gunzip(const Asset& asset, std::string& plain) {
  if (asset.size > UINT_MAX || asset.plain_size > UINT_MAX) {
    return false;
  }
  plain.assign(asset.plain_size, '\0');
  z_stream stream{};
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return false;
  }
  stream.next_in = const_cast<Bytef*>(asset.data);
  stream.avail_in = static_cast<uInt>(asset.size);
  stream.next_out = reinterpret_cast<Bytef*>(plain.data());
  stream.avail_out = static_cast<uInt>(plain.size());
  int status = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  return status == Z_STREAM_END && stream.total_out == asset.plain_size;
}
}  // namespace

namespace master {

// Read by the signal handler and by the delayed restart thread, written by
// Run(): a lock-free atomic is safe in both, where sig_atomic_t is not
// across threads.
std::atomic<int> shutdown_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free);

void Reply(Response& response, const json& value, int status) {
  response.status = status;
  response.set_content(JsonDump(value), "application/json");
}
void Error(Response& response, const std::string& error, int status) {
  Reply(response, {{"error", error}}, status);
}

Master::Master(WebOptions options, std::string executable,
               std::string directory)
    : options_(std::move(options)),
      executable_(std::move(executable)),
      directory_(std::move(directory)),
      local_("http://127.0.0.1:" + std::to_string(options_.port)),
      local_authority_(local_.substr(7)),
      origin_(options_.origin.empty() ? local_ : options_.origin),
      epoch_(RandomToken(16)),
      secret_(RandomToken()),
      host_(epoch_, kQueueBytes, executable_, directory_) {}

bool Master::Initialize(std::string& error) {
  if (epoch_.empty() || secret_.empty() || !host_wake_.Open()) {
    error = "OS randomness unavailable";
    return false;
  }
  if (!options_.origin.empty()) {
    // No path, query, userinfo or forwarding-header origin inference.
    bool http = origin_.starts_with("http://");
    size_t prefix = http ? 7 : 8;
    in_addr address{};
    std::string host = http ? origin_.substr(7, origin_.find(':', 7) - 7) : "";
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
  // Integration tests read deliveries from a file instead of a push
  // service. Internal: a deployment never sets it.
  PushTransport capture;
  if (std::string path = EnvStr("UAGENT_INTERNAL_PUSH_CAPTURE");
      !path.empty()) {
    capture = [path](const std::string& endpoint, const std::string& vapid,
                     const std::string& body) -> int64_t {
      std::string ignored;
      AppendPrivateLine(path,
                        JsonDump({{"endpoint", endpoint},
                                  {"vapid", vapid.starts_with("vapid t=")},
                                  {"bytes", body.size()}}),
                        ignored);
      return 201;
    };
  }
  push_ =
      std::make_unique<PushSender>(directory_, options_.push_contact, capture);
  host_.LoadDrafts();
  host_.RefreshCatalogue();
  host_.RefreshInvalidations({});
  server_.new_task_queue = [] {
    return new httplib::ThreadPool(
        kIdleWorkerThreads,
        kWebSseConnections + kViewerInstances + kRequestWorkerThreads,
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
        "'self'; manifest-src 'self'; worker-src 'self'; frame-src 'self'; "
        "base-uri 'none'; form-action 'self'; frame-ancestors 'none'"}});
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
  RegisterApiRoutes();
  if (!browser::DataDirectory().empty()) RegisterBrowserRoutes();
  if (!RegisterAssetRoutes(error)) return false;
  if (!server_.bind_to_port(options_.bind, options_.port)) {
    error = "web port is unavailable; no alternate server was started";
    return false;
  }
  return true;
}

int Master::Run() {
  std::string error;
  json discovery = {{"v", kProtocol},
                    {"port", options_.port},
                    {"url", origin_},
                    {"secret", secret_},
                    {"epoch", epoch_}};
  auto written =
      ToolWritePrivateFile(directory_ + "/discovery.json", JsonDump(discovery));
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
  std::string pairing;
  {
    std::lock_guard lock(mutex_);
    pairing = Pair();
  }
  printf("%s", PairingText(origin_, pairing).c_str());
  fflush(stdout);
  std::thread stopping([&] {
    for (;;) {
      std::vector<std::string> paths = host_.PresencePaths();
      // Whoever saves a setting (here, a terminal) reaches every browser.
      paths.push_back(SettingsPath());
      // Wakes for a catalogue scan the throttle deferred.
      auto deadline = Clock::now() + std::chrono::hours(kIdleSleepHours);
      if (auto rescan = host_.RescanDue()) {
        deadline = std::min(deadline, *rescan);
      }
      auto result = WaitForAnyFileChange(paths, deadline, stop.read.Get());
      if (result == FileWaitResult::kInterrupted) {
        // A restart was requested over HTTP: its reply is still leaving.
        if (reexec_) std::this_thread::sleep_for(kRestartReply);
        break;
      }
      PublishSettings();
      // Sessions a panel creates or deletes reach every client.
      host_.RefreshCatalogue();
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
      WaitForAnyFileChange(wait.paths, wait.deadline, host_wake_.read.Get(),
                           wait.observed);
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
            if (device.Active()) {
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

// The API: pairing, conversations, commands, events and their files.
void Master::RegisterApiRoutes() {
  server_.Post("/api/instance", [this](const Request& request,
                                       Response& response) {
    if (!EqualSecret(request.get_header_value("X-Uagent-Instance"), secret_)) {
      Error(response, "instance authentication failed", 403);
      return;
    }
    std::lock_guard lock(mutex_);
    Reply(response, {{"v", kProtocol},
                     {"epoch", epoch_},
                     {"url", origin_},
                     {"pairing_code", Pair()}});
  });
  server_.Post("/api/auth", [this](const Request& request, Response& response) {
    Authenticate(request, response);
  });
  server_.Get(
      "/api/sessions", [this](const Request& request, Response& response) {
        if (request.has_param("refresh")) {
          host_.RefreshCatalogue(true);
        }
        host_.RefreshPresence();
        json catalogue = host_.Catalogue();
        std::string device;
        json devices;
        {
          std::lock_guard lock(mutex_);
          device = DeviceId(request);
          devices = PublicDevices();
        }
        const json body = {{"v", kProtocol},
                           {"epoch", epoch_},
                           {"cursor", std::move(catalogue["cursor"])},
                           {"sessions", std::move(catalogue["sessions"])},
                           {"spaces", std::move(catalogue["spaces"])},
                           {"commands", CommandSchemaJson()},
                           {"verbosity",
                            [] {
                              json table = DetailPoliciesJson();
                              table["level"] = ConfiguredVerbosity();
                              return table;
                            }()},
                           {"capabilities", push_->Capabilities(device)},
                           {"devices", std::move(devices)},
                           {"scheduled", std::move(catalogue["scheduled"])},
                           {"device", device}};
        Reply(response, body);
      });
  server_.Get(R"(/api/sessions/([a-f0-9]{16,64}))",
              [this](const Request& request, Response& response) {
                Snapshot(request, response);
              });
  server_.Post("/api/command",
               [this](const Request& request, Response& response) {
                 Command(request, response);
               });
  server_.Get(
      R"(/api/receipts/([a-f0-9]{16,64}))",
      [this](const Request& request, Response& response) {
        std::lock_guard lock(mutex_);
        auto found =
            requests_.find(DeviceId(request) + ":" + request.matches[1].str());
        if (found != requests_.end()) {
          RefreshReceipt(found->second, request.matches[1].str());
        }
        Reply(response, found == requests_.end()
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
  server_.Get(R"(/api/sessions/([a-f0-9]{16,64})/files)",
              [this](const Request& request, Response& response) {
                Files(request, response);
              });
}

// The shared browser's status and its live viewer.
void Master::RegisterBrowserRoutes() {
  server_.Get("/api/browser/status", [this](const Request& request,
                                            Response& response) {
    json status = browser::Request(
        {{"op", "status"},
         {"instance_id", request.has_param("instance")
                             ? request.get_param_value("instance")
                             : "0"}});
    std::lock_guard lock(mutex_);
    status["controller"] = JsonValue(status, "viewer", "") == DeviceId(request);
    status["leased"] = !JsonValue(status, "viewer", "").empty();
    status.erase("viewer");
    Reply(response, status);
  });
  server_.WebSocket(
      "/api/browser/viewer",
      [this](const Request& request, httplib::ws::WebSocket& socket) {
        std::string device;
        {
          std::lock_guard lock(mutex_);
          const std::string* expected_origin = ExpectedOrigin(request);
          if (expected_origin &&
              request.get_header_value("Origin") == *expected_origin) {
            device = DeviceId(request);
          }
        }
        const std::string instance = request.has_param("instance")
                                         ? request.get_param_value("instance")
                                         : "0";
        if (instance.size() != 1 || instance[0] < '0' || instance[0] > '4') {
          socket.close(httplib::ws::CloseStatus::PolicyViolation);
          return;
        }
        auto& active = viewer_active_[static_cast<size_t>(instance[0] - '0')];
        if (device.empty() || active.exchange(true)) {
          socket.close(httplib::ws::CloseStatus::PolicyViolation);
          return;
        }
        if (auto viewed = RelayBrowserViewer(socket, device, instance);
            viewed.display) {
          browser::Request({{"op", "viewer_disconnected"},
                            {"instance_id", instance},
                            {"device", device},
                            {"display", viewed.display},
                            {"generation", viewed.generation}});
        }
        active = false;
      });
}

// The app itself: the embedded bundle, or a test's build of it.
bool Master::RegisterAssetRoutes(std::string& error) {
  // Browser tests serve the bundle they just built, so a web edit needs no
  // rebuild of this binary: that folder alone answers, never a mix of it
  // and the embedded bundle. Never set outside a test run.
  const char* dist = getenv("UAGENT_INTERNAL_WEB_DIST");
  if (dist) {
    server_.set_file_extension_and_mimetype_mapping(
        "webmanifest", "application/manifest+json");
    if (!server_.set_mount_point("/", dist)) {
      error = std::string("cannot serve the web bundle in ") + dist;
      return false;
    }
  }
  server_.Get(R"(/.*)", [dist](const Request& request, Response& response) {
    std::string path = request.path == "/" ? "/index.html" : request.path;
    for (const Asset& asset : Assets()) {
      if (dist) break;
      if (asset.path != path) {
        continue;
      }
      response.set_header(
          "Cache-Control",
          path.starts_with("/assets/")
              ? "public, max-age=" + std::to_string(kSecondsPerYear) +
                    ", immutable"
              : "no-cache");
      if (!asset.gzip) {
        response.set_content(reinterpret_cast<const char*>(asset.data),
                             asset.size, std::string(asset.mime));
        return;
      }
      response.set_header("Vary", "Accept-Encoding");
      // A range selects bytes of the plain asset: slices of a gzip stream
      // are not one, so a ranged request is always answered plain.
      if (!request.has_header("Range") &&
          AcceptsGzip(request.get_header_value("Accept-Encoding"))) {
        response.set_header("Content-Encoding", "gzip");
        response.set_content(reinterpret_cast<const char*>(asset.data),
                             asset.size, std::string(asset.mime));
        return;
      }
      std::string plain;
      if (!Gunzip(asset, plain)) {
        fprintf(stderr, "web asset %s does not inflate\n", path.c_str());
        Error(response, "asset unavailable", 500);
        return;
      }
      response.set_content(std::move(plain), std::string(asset.mime));
      return;
    }
    Error(response, "not found", 404);
  });
  return true;
}

const std::string* Master::ExpectedOrigin(const Request& request) const {
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
// The configured level, by a name the policy table knows.
std::string Master::ConfiguredVerbosity() {
  const auto values = ConfigManager::Capture({}, "").Read().values;
  const auto found = values.find(std::string(kVerbositySetting));
  return std::string(
      DetailFor(found == values.end() ? "" : found->second).level);
}
// Tells every browser that what is saved is no longer what they read, and
// the level with it: display state they all share.
void Master::PublishSettings() {
  const FileStamp saved = SnapshotFile(SettingsPath());
  {
    std::lock_guard lock(settings_mutex_);
    if (saved == settings_) return;
    settings_ = saved;
  }
  host_.Publish(
      "", "", {{"kind", "settings.changed"}, {"level", ConfiguredVerbosity()}});
}

}  // namespace master

int MasterMain(const WebOptions& options, char** argv) {
  if (!options.browser_data.empty()) {
    browser::SetDataDirectory(options.browser_data);
  }
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
  if (!lease.Acquire(directory + "/" + kMasterLockFile, error, true)) {
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
            printf("%sSuggested host directory: %s\n",
                   PairingText(JsonValue(identity, "url", ""),
                               JsonValue(identity, "pairing_code", ""))
                       .c_str(),
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
  SetExecutablePath(argv[0]);
  int code = 1;
  bool reexec = false;
  {
    // The whole master, browser service included, is gone before a re-exec,
    // so the new one never races the old for the lock or the sockets.
    master::Master master(options, ExecutablePath(), directory);
    if (!master.Initialize(error)) {
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    code = master.Run();
    reexec = master.Reexec();
  }
  if (!reexec) return code;
  lease.Reset();
  execv(ExecutablePath().c_str(), argv);
  fprintf(stderr, "cannot restart the web host: %s\n", strerror(errno));
  return 1;
}
}  // namespace uagent::web
