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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "include/browser/browser.h"
#include "include/core/fd.h"
#include "include/core/platform.h"
#include "include/web/rfb_filter.h"

namespace uagent::web {
namespace {
// How often the relay re-reads the lease; input follows control within this.
constexpr int kLeaseCheckMs = 250;

Fd ConnectRfb() { return ConnectUnix(browser::RfbPath()); }

// The display this device views and whether it drives it, or nullopt once
// the browser stops.
struct Lease {
  uint64_t display = 0;
  uint64_t generation = 0;
  bool controller = false;
};
std::optional<Lease> ReadLease(const std::string& device) {
  json status = browser::Request({{"op", "viewer"}, {"device", device}}, 1000);
  if (!status.value("ok", false)) return std::nullopt;
  return Lease{JsonValue(status, "display", uint64_t{0}),
               JsonValue(status, "generation", uint64_t{0}),
               status.value("controller", false)};
}

}  // namespace

ViewerSession RelayBrowserViewer(httplib::ws::WebSocket& socket,
                                 const std::string& device) {
  const std::optional<Lease> initial = ReadLease(device);
  if (!initial) {
    socket.close(httplib::ws::CloseStatus::PolicyViolation);
    return {};
  }
  const uint64_t display = initial->display;
  Fd rfb = ConnectRfb();
  if (!rfb) {
    socket.close(httplib::ws::CloseStatus::GoingAway);
    return {};
  }
  std::atomic<bool> stopped{false};
  // Input to Chrome has one filter and one writer: this device's messages
  // while it drives, and the release that lifts whatever it still holds the
  // moment control moves away, even if it sends nothing more.
  std::mutex input;
  RfbInputFilter filter;
  bool driving = initial->controller;
  uint64_t generation = initial->generation;
  const auto release = [&] {
    if (driving) WriteAllWithin(rfb.Get(), filter.Release(), 2000);
    driving = false;
  };
  std::thread output([&] {
    char buffer[65536];
    auto next_check = std::chrono::steady_clock::now();
    while (!stopped) {
      pollfd ready{rfb.Get(), POLLIN, 0};
      int result = poll(&ready, 1, kLeaseCheckMs);
      if (std::chrono::steady_clock::now() >= next_check) {
        // A new display (Chrome restarted) needs a fresh RFB session; a
        // control change only gates input on this one.
        auto lease = ReadLease(device);
        if (!lease || lease->display != display) break;
        {
          std::lock_guard lock(input);
          generation = lease->generation;
          if (!lease->controller) release();
          driving = lease->controller;
        }
        next_check = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(kLeaseCheckMs);
      }
      if (result < 0) break;
      if (result == 0) continue;
      ssize_t n = read(rfb.Get(), buffer, sizeof(buffer));
      if (n <= 0 || !socket.send(buffer, static_cast<size_t>(n))) break;
    }
    stopped = true;
    socket.close(httplib::ws::CloseStatus::GoingAway);
  });
  std::string data, forwarded;
  while (!stopped) {
    auto result = socket.read(data);
    if (result != httplib::ws::ReadResult::Binary || data.size() > 262144) {
      break;
    }
    std::lock_guard lock(input);
    if (!filter.Push(data, forwarded, driving) ||
        !WriteAllWithin(rfb.Get(), forwarded, 2000)) {
      break;
    }
  }
  stopped = true;
  {
    std::lock_guard lock(input);
    release();
  }
  shutdown(rfb.Get(), SHUT_RDWR);
  socket.close();
  output.join();
  std::lock_guard lock(input);
  return {display, generation};
}
}  // namespace uagent::web
