// Copyright 2026 Timon Gentzsch
#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT
#include "include/web/remote_hosts.h"

#include <httplib.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/build.h"
#include "include/core/capture.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"
#include "include/web/protocol.h"

namespace uagent::web {
namespace {
using Clock = std::chrono::steady_clock;
struct Host {
  std::string id, address, name, control, socket, log, cookie;
  uint64_t generation = 0;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::jthread worker;
  std::string phase = "disconnected", error;
  bool requested = false, disconnect = false, attempting = false,
       failed = false;
  int retry = 0;
  pid_t pid = -1;
  std::optional<RemoteEndpoint> endpoint;
};
bool Address(const std::string& address) {
  return !address.empty() && address.size() <= 255 && address[0] != '-' &&
         std::all_of(address.begin(), address.end(), [](unsigned char ch) {
           return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' ||
                  ch == '@' || ch == ':' || ch == '[' || ch == ']';
         });
}
std::vector<std::string> Ssh(const Host& host) {
  return {"ssh",
          "-S",
          host.control,
          "-o",
          "BatchMode=yes",
          "-o",
          "StrictHostKeyChecking=yes",
          "-o",
          "ConnectTimeout=10",
          "-o",
          "ForwardAgent=no",
          "-o",
          "ControlMaster=no",
          host.address};
}
CapturedProcess Run(const Host& host, const std::string& command,
                    const std::string& input = "") {
  auto argv = Ssh(host);
  argv.push_back(command);
  return CaptureProcess(argv, 30, input);
}
void Close(Host& host) {
  if (host.pid > 0) StopProcess(host.pid, true, std::chrono::milliseconds(500));
  host.pid = -1;
  unlink(host.socket.c_str());
  unlink(host.control.c_str());
}
std::string Bundle(const std::string& executable) {
  std::string supplied = EnvStr("UAGENT_REMOTE_BUNDLE");
  if (!supplied.empty()) return supplied;
  return (std::filesystem::path(executable).parent_path().parent_path() /
          "share/uagent/remote" / std::string(BuildIdentity()))
      .string();
}
bool Install(const Host& host, const std::string& executable,
             std::string& error) {
  auto probe = Run(host,
                   "printf 'uagent-platform='; uname -s; uname -m; if test "
                   "\"$(uname -s)\" = Darwin; then sw_vers -productVersion; "
                   "else getconf GNU_LIBC_VERSION; fi");
  if (!probe.Ok()) {
    error = "Cannot inspect the remote platform.";
    return false;
  }
  const auto marker = probe.output.find("uagent-platform=");
  const std::string platform =
      marker == std::string::npos ? "" : probe.output.substr(marker + 16);
  const bool mac = platform.starts_with("Darwin\n");
  const bool linux = platform.starts_with("Linux\n");
  const bool arm = platform.find("\narm64") != std::string::npos ||
                   platform.find("\naarch64") != std::string::npos;
  const bool intel = platform.find("\nx86_64") != std::string::npos;
  if ((!mac && !linux) || (!arm && !intel)) {
    error = "Remote hosts require Linux or macOS on ARM64 or x86_64.";
    return false;
  }
  // Refuse unsupported loaders before transferring or launching a payload.
  const std::string version_command =
      mac ? "test $(sw_vers -productVersion | cut -d. -f1) -ge 15"
          : "v=$(getconf GNU_LIBC_VERSION); v=${v#glibc }; major=${v%%.*}; "
            "minor=${v#*.}; minor=${minor%%.*}; test \"$major\" -gt 2 || { "
            "test \"$major\" -eq 2 && test \"$minor\" -ge 38; }";
  if (!Run(host, version_command).Ok()) {
    error = "Remote hosts require macOS 15+ or Linux with glibc 2.38+.";
    return false;
  }
  const std::string target =
      std::string(mac ? "macos-" : "linux-") + (arm ? "arm64" : "x86_64");
  const std::string bundle = Bundle(executable);
  const json manifest = ReadJsonFile(bundle + "/manifest.json", MiB(1));
  if (JsonValue(manifest, "build", "") != BuildIdentity() ||
      !manifest.contains("targets") || !manifest["targets"].contains(target) ||
      manifest["targets"].size() != 4) {
    error =
        "This installation has no complete matching remote bundle. Install the "
        "four-target bundle for this build.";
    return false;
  }
  if (!CheckRemoteBundle(bundle, error)) return false;
  const std::string archive = bundle + "/" + target + ".tar.gz";
  CapturedProcess checksum;
#ifdef __APPLE__
  checksum = CaptureProcess({"/usr/bin/shasum", "-a", "256", archive});
#else
  checksum = CaptureProcess({"sha256sum", archive});
#endif
  const std::string hash = JsonValue(manifest["targets"][target], "sha256", "");
  if (!checksum.Ok() || hash.size() != 64 ||
      !checksum.output.starts_with(hash + " ")) {
    error = "Remote bundle integrity check failed.";
    return false;
  }
  std::string bytes, read_error;
  if (!ReadRegularFile(archive, MiB(128), bytes, read_error)) {
    error = "Cannot read remote payload: " + read_error;
    return false;
  }
  // A private staging directory and a remote lock serialize first install.
  // The staged new executable asks the old host's authenticated admission
  // gate to stop only when idle. Old hosts without that capability refuse.
  const std::string build(BuildIdentity());
  const std::string command =
      "set -eu; umask 077; mkdir -p \"$HOME/.uagent/remote\"; "
      "cd \"$HOME/.uagent/remote\"; mkdir install.lock || exit 73; "
      "stage=$(mktemp -d \"$PWD/stage.XXXXXXXX\"); trap 'rm -rf \"$stage\"; "
      "rmdir install.lock' EXIT; "
      "cat > \"$stage/payload.tar.gz\"; cd \"$stage\"; " +
      std::string(mac ? "actual=$(shasum -a 256 payload.tar.gz)"
                      : "actual=$(sha256sum payload.tar.gz)") +
      "; test \"${actual%% *}\" = " + ShellQuote(hash) +
      "; tar -xzf payload.tar.gz; test \"$(./uagent --build-id)\" = " +
      ShellQuote(build) +
      "; if test -x ../uagent && test \"$(../uagent --build-id)\" != " +
      ShellQuote(build) +
      "; then ./uagent --remote-stop-if-idle; fi; cd ..; "
      "mkdir -p builds; rm \"$stage/payload.tar.gz\"; "
      "if test ! -d builds/" +
      build + "; then mv \"$stage\" builds/" + build +
      "; fi; "
      "ln -s builds/" +
      build + "/uagent uagent.new; mv -f uagent.new uagent";
  const auto installed = Run(host, command, bytes);
  if (!installed.Ok()) {
    const json detail = json::parse(installed.output, nullptr, false);
    error = JsonValue(detail, "error",
                      "Remote payload installation failed. Check the platform "
                      "minimum and ~/.uagent/remote/install.lock.");
    return false;
  }
  return true;
}
bool Connect(Host& host, const std::string& executable,
             RemoteEndpoint& endpoint, std::string& error, bool& retryable) {
  retryable = false;
  Close(host);
  SpawnSpec spec;
  spec.argv = {"ssh",
               "-M",
               "-N",
               "-S",
               host.control,
               "-o",
               "ControlPersist=no",
               "-o",
               "BatchMode=yes",
               "-o",
               "StrictHostKeyChecking=yes",
               "-o",
               "ConnectTimeout=10",
               "-o",
               "ServerAliveInterval=10",
               "-o",
               "ServerAliveCountMax=3",
               "-o",
               "ForwardAgent=no",
               host.address};
  spec.group = SpawnSpec::Group::kOwn;
  Fd log(
      open(host.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600));
  if (!log) {
    error = "Cannot open the private SSH log.";
    return false;
  }
  spec.fds = {{0}, {1}, {2, log.Get()}};
  if (SpawnProcess(spec, host.pid) != 0) {
    error = "OpenSSH is not available on the serving host.";
    return false;
  }
  bool ready = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    int status = 0;
    if (WaitPid(host.pid, &status, WNOHANG) == host.pid) {
      host.pid = -1;
      break;
    }
    if (PathExists(host.control)) {
      ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!ready) {
    const std::string detail = ReadFile(host.log, KiB(64)).value_or("");
    if (detail.find("Host key verification failed") != std::string::npos ||
        detail.find("REMOTE HOST IDENTIFICATION HAS CHANGED") !=
            std::string::npos) {
      error =
          "SSH host identity needs attention. On the serving host, run "
          "ssh " +
          host.address + " and verify its fingerprint.";
    } else if (detail.find("Permission denied") != std::string::npos) {
      error =
          "SSH key access failed. On the serving host, unlock an "
          "encrypted key with ssh-add, then verify ssh " +
          host.address + "; use ssh-copy-id " + host.address +
          " if a key has not been installed.";
    } else if (detail.find("Could not resolve hostname") != std::string::npos) {
      error =
          "SSH destination was not found. Check the address or alias "
          "in the serving host's ~/.ssh/config.";
    } else {
      retryable = true;
      error = "SSH network connection failed. Check that " + host.address +
              " is reachable from the serving host.";
    }
    error += " Details: " + host.log;
    return false;
  }
  auto present = Run(host,
                     "test -x \"$HOME/.uagent/remote/uagent\" && "
                     "\"$HOME/.uagent/remote/uagent\" --build-id");
  if ((!present.Ok() || Trim(present.output) != BuildIdentity()) &&
      !Install(host, executable, error)) {
    return false;
  }
  auto startup = Run(host, "\"$HOME/.uagent/remote/uagent\" --remote-start");
  const json identity = json::parse(startup.output, nullptr, false);
  if (!startup.Ok() || JsonValue(identity, "build", "") != BuildIdentity()) {
    error = JsonValue(identity, "error",
                      "Remote build differs or startup failed. Update the "
                      "remote host explicitly; active work was left running.");
    return false;
  }
  endpoint.origin = JsonValue(identity, "origin", "");
  const std::string remote_socket = JsonValue(identity, "socket", "");
  const std::string code = JsonValue(identity, "pairing_code", "");
  if ((!endpoint.origin.starts_with("http://127.0.0.1:") &&
       endpoint.origin != "http://localhost") ||
      (!remote_socket.empty() &&
       (remote_socket[0] != '/' ||
        remote_socket.find_first_of(":\n\r") != std::string::npos))) {
    error = "Remote discovery returned an invalid endpoint.";
    return false;
  }
  auto forwarding = Ssh(host);
  forwarding.insert(
      forwarding.begin() + 1,
      {"-O", "forward", "-L",
       host.socket + ":" +
           (remote_socket.empty() ? "127.0.0.1:" + endpoint.origin.substr(17)
                                  : remote_socket)});
  auto forward = CaptureProcess(forwarding, 10);
  if (!forward.Ok()) {
    retryable = true;
    error = "SSH could not forward the private remote API.";
    return false;
  }
  chmod(host.socket.c_str(), 0600);
  httplib::Client client(endpoint.origin);
  client.set_address_family(AF_UNIX);
  client.set_hostname_addr_map({{client.host(), host.socket}});
  client.set_connection_timeout(5);
  client.set_read_timeout(5);
  if (!host.cookie.empty()) {
    auto authorized = client.Get(
        "/api/sessions",
        httplib::Headers{{"Cookie", host.cookie}, {"Origin", endpoint.origin}});
    if (authorized && authorized->status == 200) {
      endpoint.cookie = host.cookie;
      endpoint.socket = host.socket;
      return true;
    }
  }
  auto paired = client.Post(
      "/api/auth", {{"Origin", endpoint.origin}},
      JsonDump({{"code", code}, {"name", "Remote workspace gateway"}}),
      "application/json");
  if (!paired || paired->status != 200) {
    retryable = !paired;
    error = "Remote API authentication failed.";
    return false;
  }
  const std::string cookie = paired->get_header_value("Set-Cookie");
  endpoint.cookie = cookie.substr(0, cookie.find(';'));
  endpoint.socket = host.socket;
  host.cookie = endpoint.cookie;
  std::string save_error;
  if (!WritePrivateFile(GlobalBase() + "/web/remote-" + host.id + ".json",
                        JsonDump({{"cookie", host.cookie}}), save_error)) {
    error = "Cannot save the private remote credential: " + save_error;
    return false;
  }
  if (endpoint.cookie.empty()) {
    error = "Remote API did not issue a device credential.";
    return false;
  }
  return true;
}
void Work(Host& host, const std::string& executable, std::stop_token stop) {
  std::unique_lock lock(host.mutex);
  auto due = Clock::time_point::max();
  while (!stop.stop_requested()) {
    if (host.disconnect) {
      host.disconnect = false;
      host.failed = false;
      lock.unlock();
      Close(host);
      lock.lock();
      host.endpoint.reset();
      host.phase = "disconnected";
      host.error.clear();
      due = Clock::time_point::max();
    }
    if (host.failed && !host.disconnect) {
      host.failed = false;
      lock.unlock();
      Close(host);
      lock.lock();
      host.endpoint.reset();
      host.retry = 0;
      host.phase = "reconnecting";
      due = Clock::now() + std::chrono::seconds(1);
    }
    if (host.endpoint) {
      int status = 0;
      if (WaitPid(host.pid, &status, WNOHANG) == host.pid) {
        host.pid = -1;
        host.endpoint.reset();
        host.retry = 0;
        host.phase = "reconnecting";
        due = Clock::now() + std::chrono::seconds(1);
      }
    }
    if (host.requested || Clock::now() >= due) {
      host.requested = false;
      host.phase = host.retry ? "reconnecting" : "connecting";
      host.attempting = true;
      ++host.generation;
      lock.unlock();
      RemoteEndpoint endpoint;
      std::string error;
      bool retryable = false;
      const bool connected =
          Connect(host, executable, endpoint, error, retryable);
      if (!connected) Close(host);
      lock.lock();
      host.attempting = false;
      if (connected) {
        host.endpoint = std::move(endpoint);
        host.phase = "connected";
        host.error.clear();
        host.retry = 0;
        due = Clock::time_point::max();
      } else {
        host.error = std::move(error);
        host.endpoint.reset();
        constexpr int kDelays[] = {1, 2, 4, 8, 15};
        if (retryable && host.retry < 5) {
          host.phase = "reconnecting";
          // A little per-host jitter avoids retry bursts after a network drop.
          due = Clock::now() +
                std::chrono::milliseconds(kDelays[host.retry++] * 1000 +
                                          static_cast<int>(host.id[0]) * 3);
        } else {
          host.phase = "disconnected";
          due = Clock::time_point::max();
        }
      }
    }
    host.wake.wait_for(lock, std::chrono::seconds(1), [&] {
      return stop.stop_requested() || host.requested || host.disconnect ||
             host.failed;
    });
  }
  lock.unlock();
  Close(host);
}
}  // namespace
bool CheckRemoteBundle(const std::string& directory, std::string& error) {
  const json manifest = ReadJsonFile(directory + "/manifest.json", MiB(1));
  if (JsonValue(manifest, "build", "") != BuildIdentity() ||
      !manifest.contains("targets") || !manifest["targets"].is_object() ||
      manifest["targets"].size() != 4) {
    error =
        "A complete four-target remote bundle for this exact build is "
        "required.";
    return false;
  }
  for (const char* target :
       {"linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64"}) {
    const std::string path = directory + "/" + target + ".tar.gz";
    const json record = manifest["targets"].value(target, json::object());
    const std::string expected = JsonValue(record, "sha256", "");
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size != JsonValue(record, "bytes", uintmax_t{0}) ||
        expected.size() != 64 ||
        !std::all_of(expected.begin(), expected.end(),
                     [](unsigned char ch) { return std::isxdigit(ch); })) {
      error = "Remote payload is missing or malformed: " + std::string(target);
      return false;
    }
#ifdef __APPLE__
    const auto checksum =
        CaptureProcess({"/usr/bin/shasum", "-a", "256", path});
#else
    const auto checksum = CaptureProcess({"sha256sum", path});
#endif
    if (!checksum.Ok() || !checksum.output.starts_with(expected + " ")) {
      error = "Remote payload integrity differs: " + std::string(target);
      return false;
    }
  }
  return true;
}

struct RemoteHosts::Impl {
  std::string executable;
  mutable std::mutex mutex;
  std::map<std::string, std::shared_ptr<Host>> hosts;
  explicit Impl(std::string path) : executable(std::move(path)) {}
  std::shared_ptr<Host> Add(const std::string& id, const std::string& address,
                            const std::string& name) {
    auto host = std::make_shared<Host>();
    host->cookie = JsonValue(
        ReadJsonFile(GlobalBase() + "/web/remote-" + id + ".json", KiB(4)),
        "cookie", "");
    host->id = id;
    host->address = address;
    host->name = name;
    const std::string directory = RuntimeDir() + "/remote";
    if (!EnsurePrivateDirectory(directory)) return {};
    const std::string base = directory + "/" + id.substr(0, 12);
    host->control = base + ".ctl";
    host->socket = base + ".sock";
    host->log = GlobalBase() + "/web/ssh-" + id + ".log";
    hosts[id] = host;
    host->worker = std::jthread([host, this](std::stop_token stop) {
      Work(*host, executable, std::move(stop));
    });
    return host;
  }
};
RemoteHosts::RemoteHosts(std::string executable)
    : impl_(std::make_unique<Impl>(std::move(executable))) {
  const json saved = ReadJsonFile(GlobalBase() + "/web/hosts.json", KiB(64));
  if (saved.is_array()) {
    for (const json& row : saved) {
      const std::string id = JsonValue(row, "id", ""),
                        address = JsonValue(row, "address", "");
      if (OpaqueId(id) && Address(address) && impl_->hosts.size() < 16) {
        impl_->Add(id, address, JsonValue(row, "name", address));
      }
    }
  }
}
RemoteHosts::~RemoteHosts() {
  for (const auto& [id, host] : impl_->hosts) {
    host->worker.request_stop();
    host->wake.notify_all();
  }
  for (const auto& [id, host] : impl_->hosts) host->worker.join();
}
json RemoteHosts::List() const {
  std::lock_guard lock(impl_->mutex);
  json rows = json::array();
  for (const auto& [id, host] : impl_->hosts) {
    std::lock_guard state(host->mutex);
    rows.push_back({{"id", id},
                    {"name", host->name},
                    {"address", host->address},
                    {"phase", host->phase},
                    {"error", host->error}});
  }
  return rows;
}
json RemoteHosts::Change(const json& action, std::string& error) {
  std::lock_guard lock(impl_->mutex);
  const std::string kind = JsonValue(action, "action", ""),
                    id = JsonValue(action, "id", "");
  std::shared_ptr<Host> host;
  if (kind == "add") {
    const std::string address = JsonValue(action, "address", "");
    if (!Address(address) || impl_->hosts.size() >= 16) {
      error = "Enter an SSH alias or user@machine (maximum 16 hosts).";
      return nullptr;
    }
    for (const auto& [key, item] : impl_->hosts) {
      if (item->address == address) {
        error = "That host is already saved.";
        return nullptr;
      }
    }
    host = impl_->Add(RandomToken(16), address,
                      JsonValue(action, "name", address).substr(0, 80));
    if (!host) {
      error = "Cannot create the private SSH runtime directory.";
      return nullptr;
    }
    json saved = json::array();
    for (const auto& [key, item] : impl_->hosts) {
      saved.push_back(
          {{"id", key}, {"name", item->name}, {"address", item->address}});
    }
    if (!WritePrivateFile(GlobalBase() + "/web/hosts.json", JsonDump(saved),
                          error)) {
      host->worker.request_stop();
      host->wake.notify_all();
      host->worker.join();
      impl_->hosts.erase(host->id);
      return nullptr;
    }
  } else {
    const auto found = impl_->hosts.find(id);
    if (found == impl_->hosts.end()) {
      error = "Unknown host.";
      return nullptr;
    }
    host = found->second;
  }
  std::lock_guard state(host->mutex);
  if (kind == "disconnect") {
    host->disconnect = true;
    host->requested = false;
  } else if (kind == "connect" || kind == "reconnect" || kind == "add") {
    if (kind == "reconnect" && host->endpoint) host->failed = true;
    if (!host->endpoint) {
      host->retry = 0;
      if (!host->attempting) host->requested = true;
    }
  } else {
    error = "Unknown host action.";
    return nullptr;
  }
  host->wake.notify_all();
  return {{"id", host->id}};
}
std::optional<RemoteEndpoint> RemoteHosts::Endpoint(
    const std::string& id) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->hosts.find(id);
  if (found == impl_->hosts.end()) return {};
  std::lock_guard state(found->second->mutex);
  auto endpoint = found->second->endpoint;
  if (endpoint) {
    std::weak_ptr<Host> owner = found->second;
    const uint64_t generation = found->second->generation;
    endpoint->unavailable = [owner, generation] {
      if (auto host = owner.lock()) {
        std::lock_guard endpoint_lock(host->mutex);
        if (host->endpoint && host->generation == generation &&
            !host->disconnect) {
          host->failed = true;
          host->wake.notify_all();
        }
      }
    };
  }
  return endpoint;
}
}  // namespace uagent::web
