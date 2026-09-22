// Copyright 2026 Timon Gentzsch

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_DEFAULT_USER_AGENT
#include "include/web/browser_viewer.h"

#include <httplib.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include "include/browser/browser.h"
#include "include/core/fd.h"
#include "include/web/rfb_filter.h"

namespace uagent::web {
namespace {
Fd ConnectRfb() {
  std::string path = browser::RfbPath();
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return {};
  Fd fd(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!fd) return {};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (connect(fd.Get(), reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) != 0) {
    return {};
  }
  return fd;
}
bool StillViews(const std::string& device, const std::string& role,
                uint64_t generation) {
  json status = browser::Request(
      {{"op", "viewer"}, {"device", device}, {"role", role}}, 1000);
  return status.value("ok", false) &&
         JsonValue(status, "generation", uint64_t{0}) == generation;
}
}  // namespace

uint64_t RelayBrowserViewer(httplib::ws::WebSocket& socket,
                            const std::string& device,
                            const std::string& role) {
  json status =
      browser::Request({{"op", "viewer"}, {"device", device}, {"role", role}});
  if (!status.value("ok", false)) {
    socket.close(httplib::ws::CloseStatus::PolicyViolation);
    return 0;
  }
  const uint64_t generation = JsonValue(status, "generation", uint64_t{0});
  Fd rfb = ConnectRfb();
  if (!rfb) {
    socket.close(httplib::ws::CloseStatus::GoingAway);
    return 0;
  }
  std::atomic<bool> stopped{false};
  std::thread output([&] {
    char buffer[65536];
    auto next_check = std::chrono::steady_clock::now();
    while (!stopped) {
      pollfd ready{rfb.Get(), POLLIN, 0};
      int result = poll(&ready, 1, 500);
      if (std::chrono::steady_clock::now() >= next_check) {
        if (!StillViews(device, role, generation)) break;
        next_check =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      }
      if (result < 0) break;
      if (result == 0) continue;
      ssize_t n = read(rfb.Get(), buffer, sizeof(buffer));
      if (n <= 0 || !socket.send(buffer, static_cast<size_t>(n))) break;
    }
    stopped = true;
    socket.close(httplib::ws::CloseStatus::GoingAway);
  });
  std::string data;
  RfbViewOnlyFilter view_only;
  while (!stopped) {
    auto result = socket.read(data);
    if (result != httplib::ws::ReadResult::Binary || data.size() > 262144 ||
        !StillViews(device, role, generation)) {
      break;
    }
    std::string filtered;
    if (role == "observe") {
      if (!view_only.Push(data, filtered)) break;
      data = std::move(filtered);
    }
    size_t sent = 0;
    while (sent < data.size()) {
      pollfd ready{rfb.Get(), POLLOUT, 0};
      if (poll(&ready, 1, 2000) <= 0) break;
      ssize_t n = write(rfb.Get(), data.data() + sent, data.size() - sent);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    if (sent != data.size()) break;
  }
  stopped = true;
  shutdown(rfb.Get(), SHUT_RDWR);
  socket.close();
  output.join();
  return generation;
}
}  // namespace uagent::web
