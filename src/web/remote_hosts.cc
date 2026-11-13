// Copyright 2026 Timon Gentzsch
#include "include/web/remote_hosts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/build.h"
#include "include/core/capture.h"
#include "include/core/child_env.h"
#include "include/core/fs.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"
#include "include/web/protocol.h"

namespace uagent::web {
namespace {
using Clock = std::chrono::steady_clock;
struct Host {
  std::string id, address, control, log, client, executable, auth;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::jthread worker;
  std::string phase = "disconnected", error;
  bool requested = false, disconnect = false, attempting = false;
  int retry = 0;
  pid_t pid = -1;
  bool endpoint = false;
  // The main's executable is being copied there: a first connection, or the
  // first one after the main was updated.
  std::atomic<bool> installing = false;
  std::optional<double> round_trip_ms;
};
bool Address(const std::string& address) {
  return !address.empty() && address.size() <= 255 && address[0] != '-' &&
         std::all_of(address.begin(), address.end(), [](unsigned char ch) {
           return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' ||
                  ch == '@' || ch == ':' || ch == '[' || ch == ']';
         });
}
std::vector<std::string> Ssh(const Host& host) {
  return SshCommand(host.control, host.address);
}
CapturedProcess Run(const Host& host, const std::string& command,
                    const std::string& input = "") {
  auto argv = Ssh(host);
  argv.push_back(command);
  ChildEnvironment environment({}, ChildEnvironmentPolicy::kUserShell);
  return CaptureProcess(argv, 30, input, environment.Data());
}
std::optional<double> RoundTrip(const Host& host) {
  auto argv = Ssh(host);
  // An existing mux connection or failure: a measurement never reconnects.
  argv.insert(argv.begin() + 1, {"-o", "ProxyCommand=false"});
  argv.emplace_back("true");
  ChildEnvironment environment({}, ChildEnvironmentPolicy::kUserShell);
  const auto started = Clock::now();
  const auto probe = CaptureProcess(argv, 3, "", environment.Data());
  if (!probe.Ok()) return std::nullopt;
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}
void Close(Host& host) {
  if (host.pid > 0) StopProcess(host.pid, true, std::chrono::milliseconds(500));
  host.pid = -1;
  unlink(host.control.c_str());
}
bool Install(const Host& host, const std::string& bundle, std::string& error) {
  auto probe = Run(host, "printf 'uagent-platform='; uname -s; uname -m");
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
  const auto manifest = CheckRemoteBundle(bundle, error);
  if (!manifest) return false;
  const std::string archive = bundle + "/" + target + ".tar.gz";
  const std::string hash =
      JsonValue((*manifest)["targets"][target], "sha256", "");
  std::string bytes, read_error;
  if (!ReadRegularFile(archive, MiB(128), bytes, read_error)) {
    error = "Cannot read remote payload: " + read_error;
    return false;
  }
  // Immutable build directories let different mains coexist without stopping
  // another user's execution process or replacing its resources.
  const std::string build(BuildIdentity());
  const std::string command =
      "set -eu; umask 077; mkdir -p \"$HOME/.uagent/remote/builds\"; "
      "cd \"$HOME/.uagent/remote\"; "
      "stage=$(mktemp -d \"$PWD/stage.XXXXXXXX\"); trap 'rm -rf \"$stage\"' "
      "EXIT; "
      "cat > \"$stage/payload.tar.gz\"; cd \"$stage\"; " +
      std::string(mac ? "actual=$(shasum -a 256 payload.tar.gz)"
                      : "actual=$(sha256sum payload.tar.gz)") +
      "; test \"${actual%% *}\" = " + ShellQuote(hash) +
      "; tar -xzf payload.tar.gz; test \"$(./uagent --build-id)\" = " +
      ShellQuote(build) +
      "; rm payload.tar.gz; cd ..; "
      "if mkdir install.lock 2>/dev/null; then "
      "trap 'rm -rf \"$stage\"; rmdir install.lock' EXIT; "
      "if test ! -d builds/" +
      build + "; then mv \"$stage\" builds/" + build +
      "; fi; "
      "else for n in 1 2 3 4 5; do test ! -d install.lock && break; sleep 1; "
      "done; "
      "test -x builds/" +
      build + "/uagent; fi";
  if (!Run(host, command, bytes).Ok()) {
    error =
        "Remote payload installation failed. Check the platform minimum and "
        "~/.uagent/remote/install.lock.";
    return false;
  }
  return true;
}
bool Connect(Host& host, const std::string& bundle, std::string& error,
             bool& retryable, const std::stop_token& stop) {
  retryable = false;
  Close(host);
  SpawnSpec spec;
  spec.argv = SshCommand(host.control, host.address, true);
  ChildEnvironment environment({}, ChildEnvironmentPolicy::kUserShell);
  spec.environment = environment.Data();
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
  for (int attempt = 0; attempt < 300 && !stop.stop_requested(); ++attempt) {
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
  const std::string build(BuildIdentity());
  // Administrators can share one read-only installation across OS accounts.
  const std::string shared =
      "/usr/local/lib/uagent/remote/" + build + "/uagent";
  auto present = Run(host, "test -x " + ShellQuote(shared) + " && " +
                               ShellQuote(shared) + " --build-id");
  if (present.Ok() && Trim(present.output) == build) {
    host.executable = ShellQuote(shared);
  } else {
    host.executable = "\"$HOME/.uagent/remote/builds/" + build + "/uagent\"";
    present = Run(host, "test -x " + host.executable + " && " +
                            host.executable + " --build-id");
    if (!present.Ok() || Trim(present.output) != build) {
      host.installing = true;
      const bool installed = Install(host, bundle, error);
      host.installing = false;
      if (!installed) return false;
    }
  }
  return true;
}

void Work(Host& host, const std::string& bundle, std::stop_token stop) {
  std::unique_lock lock(host.mutex);
  auto due = Clock::time_point::max();
  auto sample_due = Clock::time_point::min();
  while (!stop.stop_requested()) {
    if (host.disconnect) {
      host.disconnect = false;
      // Invalidate in-flight streams before closing their SSH transport.
      // Their failures must not turn an explicit disconnect into a retry.
      host.endpoint = false;
      host.phase = "disconnected";
      host.round_trip_ms.reset();
      host.error.clear();
      due = Clock::time_point::max();
      lock.unlock();
      Close(host);
      lock.lock();
    }
    if (host.endpoint) {
      int status = 0;
      if (WaitPid(host.pid, &status, WNOHANG) == host.pid) {
        host.pid = -1;
        host.endpoint = false;
        host.retry = 0;
        host.phase = "reconnecting";
        due = Clock::now() + std::chrono::seconds(1);
      }
    }
    if (host.requested || Clock::now() >= due) {
      host.requested = false;
      host.phase = host.retry ? "reconnecting" : "connecting";
      host.round_trip_ms.reset();
      host.attempting = true;
      lock.unlock();
      std::string error;
      bool retryable = false;
      const bool connected = Connect(host, bundle, error, retryable, stop);
      if (!connected) Close(host);
      lock.lock();
      host.attempting = false;
      if (connected) {
        host.endpoint = true;
        host.phase = "connected";
        sample_due = Clock::time_point::min();
        host.error.clear();
        host.retry = 0;
        due = Clock::time_point::max();
      } else {
        host.error = std::move(error);
        host.endpoint = false;
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
    if (host.endpoint && !host.disconnect && Clock::now() >= sample_due) {
      lock.unlock();
      const auto measured = RoundTrip(host);
      lock.lock();
      if (host.endpoint && !host.disconnect) host.round_trip_ms = measured;
      sample_due = Clock::now() + std::chrono::seconds(30);
    }
    host.wake.wait_for(lock, std::chrono::seconds(1), [&] {
      return stop.stop_requested() || host.requested || host.disconnect;
    });
  }
  lock.unlock();
  Close(host);
}
}  // namespace
std::optional<json> CheckRemoteBundle(const std::string& directory,
                                      std::string& error) {
  json manifest = ReadJsonFile(directory + "/manifest.json", MiB(1));
  if (JsonValue(manifest, "build", "") != BuildIdentity() ||
      !manifest.contains("targets") || !manifest["targets"].is_object() ||
      manifest["targets"].size() != 4) {
    error =
        "A complete four-target remote bundle for this exact build is "
        "required.";
    return std::nullopt;
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
      return std::nullopt;
    }
#ifdef __APPLE__
    const auto checksum =
        CaptureProcess({"/usr/bin/shasum", "-a", "256", path});
#else
    const auto checksum = CaptureProcess({"sha256sum", path});
#endif
    if (!checksum.Ok() || !checksum.output.starts_with(expected + " ")) {
      error = "Remote payload integrity differs: " + std::string(target);
      return std::nullopt;
    }
  }
  return manifest;
}

struct RemoteHosts::Impl {
  std::string bundle;
  mutable std::mutex mutex;
  std::map<std::string, std::shared_ptr<Host>> hosts;
  explicit Impl(std::string path) : bundle(std::move(path)) {}
  std::shared_ptr<Host> Add(const std::string& id, const std::string& address) {
    auto host = std::make_shared<Host>();
    const std::string private_path =
        GlobalBase() + "/web/client-" + id + ".json";
    const json private_identity = ReadJsonFile(private_path, KiB(4));
    host->client = JsonValue(private_identity, "client", "");
    host->auth = JsonValue(private_identity, "auth", "");
    if (!OpaqueId(host->client) || !OpaqueId(host->auth)) {
      host->client = RandomToken(16);
      host->auth = RandomToken(32);
      std::string error;
      if (!WritePrivateFile(
              private_path,
              JsonDump({{"client", host->client}, {"auth", host->auth}}),
              error)) {
        return {};
      }
    }
    host->id = id;
    host->address = address;
    const std::string directory = RuntimeDir() + "/remote";
    if (!EnsurePrivateDirectory(directory)) return {};
    const std::string base = directory + "/" + id.substr(0, 12);
    host->control = base + ".ctl";
    host->log = GlobalBase() + "/web/ssh-" + id + ".log";
    hosts[id] = host;
    host->worker = std::jthread([host, this](std::stop_token stop) {
      Work(*host, bundle, std::move(stop));
    });
    return host;
  }
};
RemoteHosts::RemoteHosts(const std::string& executable,
                         const std::string& bundle)
    : impl_(std::make_unique<Impl>(
          bundle.empty()
              ? (std::filesystem::path(executable).parent_path().parent_path() /
                 "share/uagent/remote" / std::string(BuildIdentity()))
                    .string()
              : bundle)) {
  const json saved = ReadJsonFile(GlobalBase() + "/web/hosts.json", KiB(64));
  if (saved.is_array()) {
    for (const json& row : saved) {
      const std::string id = JsonValue(row, "id", ""),
                        address = JsonValue(row, "address", "");
      if (OpaqueId(id) && Address(address) && impl_->hosts.size() < 16) {
        impl_->Add(id, address);
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
                    {"address", host->address},
                    {"phase", host->phase},
                    {"error", host->error},
                    {"installing", host->installing.load()},
                    // A connected host runs the main's own build: Connect
                    // accepts no other.
                    {"version", host->endpoint ? kVersion : ""},
                    {"round_trip_ms", host->endpoint && host->round_trip_ms
                                          ? json(*host->round_trip_ms)
                                          : json(nullptr)}});
  }
  return rows;
}
json RemoteHosts::Ping(const std::string& id) const {
  std::shared_ptr<Host> host;
  {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->hosts.find(id);
    if (found == impl_->hosts.end()) return {{"error", "Unknown host."}};
    host = found->second;
  }
  {
    std::lock_guard lock(host->mutex);
    if (!host->endpoint || host->disconnect) {
      return {{"error", "Host is disconnected."}};
    }
  }
  const auto measured = RoundTrip(*host);
  if (!measured) return {{"error", "Round-trip measurement unavailable."}};
  {
    std::lock_guard lock(host->mutex);
    if (host->endpoint && !host->disconnect) host->round_trip_ms = measured;
  }
  return {{"round_trip_ms", *measured}};
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
    host = impl_->Add(RandomToken(16), address);
    if (!host) {
      error = "Cannot create the private SSH runtime directory.";
      return nullptr;
    }
    json saved = json::array();
    for (const auto& [key, item] : impl_->hosts) {
      saved.push_back({{"id", key}, {"address", item->address}});
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
json RemoteHosts::Target(const std::string& id, const std::string& cwd,
                         const std::string& runtime) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->hosts.find(id);
  if (found == impl_->hosts.end()) return json();
  std::lock_guard state(found->second->mutex);
  const Host& host = *found->second;
  if (!host.endpoint || host.disconnect) return json();
  return {{"address", host.address},
          {"control", host.control},
          {"executable", host.executable},
          {"client", host.client},
          {"auth", host.auth},
          {"runtime", runtime},
          {"cwd", cwd}};
}
}  // namespace uagent::web
