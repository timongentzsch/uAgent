// Copyright 2026 Timon Gentzsch

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "include/core/limits.h"
#include "include/web/terminal.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace {
// How long the output relay waits before looking again; it is woken sooner
// by output, an end or its socket closing.
constexpr auto kRelayWait = std::chrono::seconds(30);
}  // namespace

json Master::TerminalList() const {
  if (!terminals_) return nullptr;
  json list = json::array();
  for (const TerminalInfo& terminal : terminals_->List()) {
    list.push_back({{"id", terminal.id},
                    {"cwd", terminal.cwd},
                    {"folder", terminal.folder},
                    {"title", terminal.title},
                    {"started", terminal.started}});
  }
  return list;
}

// One socket for each shell a paired device may open.
void Master::RegisterTerminalRoutes() {
  server_.WebSocket(
      R"(/api/terminals/([a-f0-9]{16,64}))",
      [this](const Request& request, httplib::ws::WebSocket& socket) {
        TerminalSocket(request, socket);
      });
}

// Binary frames carry the shell's output out and the device's input in; a
// text frame is a control message, a resize in or an end out. A newer
// socket for the same shell replaces this one.
void Master::TerminalSocket(const Request& request,
                            httplib::ws::WebSocket& socket) {
  std::string device;
  {
    std::lock_guard lock(mutex_);
    const std::string* expected_origin = ExpectedOrigin(request);
    if (expected_origin &&
        request.get_header_value("Origin") == *expected_origin) {
      device = DeviceId(request);
    }
  }
  if (device.empty() || !terminals_) {
    socket.close(httplib::ws::CloseStatus::PolicyViolation);
    return;
  }
  // Each socket holds a worker for as long as it is open; the pool has
  // kWebTerminalSockets of them, apart from those for requests.
  if (terminal_sockets_.fetch_add(1) >= kWebTerminalSockets) {
    --terminal_sockets_;
    socket.close(httplib::ws::CloseStatus::PolicyViolation);
    return;
  }
  const std::unique_ptr<TerminalViewer> viewer =
      terminals_->Attach(request.matches[1].str());
  if (!viewer) {
    socket.send(JsonDump({{"type", "exit"}, {"code", -1}}));
    socket.close();
    --terminal_sockets_;
    return;
  }
  std::atomic<bool> stopped{false};
  std::thread relay([&] {
    while (!stopped) {
      const TerminalOutput out = viewer->Read(kRelayWait);
      if (!out.bytes.empty() &&
          !socket.send(out.bytes.data(), out.bytes.size())) {
        break;
      }
      if (out.ended) {
        socket.send(JsonDump({{"type", "exit"}, {"code", out.code}}));
        break;
      }
      if (out.replaced) {
        socket.send(JsonDump({{"type", "replaced"}}));
        break;
      }
    }
    stopped = true;
    socket.close();
  });
  std::string data;
  while (!stopped) {
    const auto result = socket.read(data);
    if (result == httplib::ws::ReadResult::Fail ||
        data.size() > kWebTerminalMessageBytes) {
      break;
    }
    if (result == httplib::ws::ReadResult::Binary) {
      viewer->Write(data);
      continue;
    }
    const json message = json::parse(data, nullptr, false);
    const int64_t rows = JsonValue(message, "rows", int64_t{0});
    const int64_t cols = JsonValue(message, "cols", int64_t{0});
    if (JsonValue(message, "type", "") != "resize" || rows < 1 || cols < 1 ||
        rows > kPtyMaxDimension || cols > kPtyMaxDimension) {
      break;
    }
    viewer->Resize(static_cast<uint16_t>(rows), static_cast<uint16_t>(cols));
  }
  stopped = true;
  viewer->Wake();
  socket.close();
  relay.join();
  --terminal_sockets_;
}

// Opens a shell in a directory (the home folder without one), of a folder or
// the page's own, renames one or closes one.
void Master::TerminalCommand(CommandCall& call) {
  if (!terminals_) {
    call.error = "the terminal is turned off (web.terminal)";
    return;
  }
  const std::string action = JsonValue(call.command, "action", "");
  json result;
  if (action == "open") {
    const std::string id =
        terminals_->Open(JsonValue(call.command, "cwd", ""),
                         JsonValue(call.command, "folder", ""), call.error);
    if (id.empty()) return;
    result["id"] = id;
  } else if (action == "close") {
    if (!terminals_->Close(JsonValue(call.command, "id", ""))) {
      call.error = "no such terminal";
      return;
    }
  } else if (action == "rename") {
    call.error = terminals_->Rename(JsonValue(call.command, "id", ""),
                                    JsonValue(call.command, "title", ""));
    if (!call.error.empty()) return;
  } else {
    call.error = "unsupported terminal action";
    return;
  }
  result["terminals"] = TerminalList();
  call.outcome["result"] = std::move(result);
}

}  // namespace uagent::web::master
