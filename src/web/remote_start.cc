// Copyright 2026 Timon Gentzsch
#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT
#include <httplib.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "include/core/build.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/private_store.h"
#include "include/core/signals.h"
#include "include/web/protocol.h"

namespace uagent::web {
namespace {
json Discover(bool stop = false) {
  const json saved = ReadJsonFile(GlobalBase() + "/web/discovery.json", KiB(4));
  const std::string secret = JsonValue(saved, "secret", "");
  const std::string socket = JsonValue(saved, "socket", "");
  const int port = JsonValue(saved, "port", 0);
  if (!OpaqueId(secret) || (socket.empty() && (port < 1024 || port > 65535)))
    return nullptr;
  const std::string origin = socket.empty()
                                 ? "http://127.0.0.1:" + std::to_string(port)
                                 : "http://localhost";
  httplib::Client client(origin);
  client.set_connection_timeout(1);
  client.set_read_timeout(1);
  if (!socket.empty()) {
    client.set_address_family(AF_UNIX);
    client.set_hostname_addr_map({{"localhost", socket}});
  }
  if (stop) {
    auto status = client.Post("/api/instance", {{"X-Uagent-Instance", secret}},
                              "", "application/json");
    if (!status || status->status != 200)
      return {{"error", "Cannot inspect the existing host safely."}};
    const json capabilities = json::parse(status->body, nullptr, false);
    if (!JsonValue(capabilities, "idle_update", false))
      return {{"error",
               "The existing host predates safe automatic updates. Update and "
               "restart it explicitly."}};
  }
  auto result = client.Post("/api/instance", {{"X-Uagent-Instance", secret}},
                            stop ? "{\"action\":\"stop-if-idle\"}" : "",
                            "application/json");
  if (!result) return nullptr;
  if (stop) return json::parse(result->body, nullptr, false);
  if (result->status != 200) return nullptr;
  json identity = json::parse(result->body, nullptr, false);
  if (JsonValue(identity, "epoch", "") != JsonValue(saved, "epoch", ""))
    return nullptr;
  if (JsonValue(identity, "build", "") != BuildIdentity() ||
      JsonValue(identity, "v", 0) != kProtocol)
    return {{"error",
             "The existing web host has a different build. Leave active work "
             "running; update and restart it explicitly before connecting."}};
  identity["origin"] = origin;
  identity["socket"] = socket;
  // Only stdout of the authenticated SSH command receives this one-use code.
  return identity;
}
}  // namespace
int RemoteStart(bool stop) {
  if (!stop && PathExists(GlobalBase() + "/remote/install.lock")) {
    printf(
        "{\"error\":\"A remote update is in progress; reconnect shortly.\"}\n");
    return 2;
  }
  json result = Discover(stop);
  if (stop) {
    if (!result.is_object()) result = {{"stopping", true}};
    if (!result.contains("error")) {
      FileLease lease;
      std::string error;
      bool released = false;
      for (int attempt = 0; attempt < 100 && !released; ++attempt) {
        released =
            lease.Acquire(GlobalBase() + "/web/" + kMasterLockFile, error);
        if (!released)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!released)
        result = {{"error",
                   "The existing host did not release ownership; its binary "
                   "was left unchanged."}};
    }
    printf("%s\n", JsonDump(result).c_str());
    return result.contains("error") ? 2 : 0;
  }
  if (result.is_null()) {
    const std::string directory = GlobalBase() + "/web";
    if (!EnsurePrivateDirectory(directory)) return 1;
    FileLease lease;
    std::string error;
    if (lease.Acquire(directory + "/" + kMasterLockFile, error, true)) {
      lease.Reset();
      pid_t pid = -1;
      SpawnSpec spec;
      spec.argv = {ExecutablePath(), "--remote-host"};
      spec.group = SpawnSpec::Group::kSession;
      const std::string logfile = directory + "/remote-start.log";
      Fd log(open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW,
                  0600));
      if (!log) return 1;
      spec.fds = {{0}, {1, log.Get()}, {2, log.Get()}};
      if (SpawnProcess(spec, pid) != 0) return 1;
    }
    for (int attempt = 0; attempt < 50 && result.is_null(); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      result = Discover();
    }
  }
  if (result.is_null())
    result = {{"error",
               "The remote web host did not become ready. Inspect "
               "~/.uagent/web/remote-start.log and the master lock."}};
  printf("%s\n", JsonDump(result).c_str());
  return result.contains("error") ? 2 : 0;
}
}  // namespace uagent::web
