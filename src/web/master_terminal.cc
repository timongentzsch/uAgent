// Copyright 2026 Timon Gentzsch

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "include/app/terminal.h"
#include "include/core/limits.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace {
// How long the output relay waits before looking again; it is woken sooner
// by output, an end or its socket closing.
constexpr auto kRelayWait = std::chrono::seconds(30);
}  // namespace

json Master::TerminalList() const {
  if (!terminals_) return nullptr;
  json list;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    list = remote_terminals_;
  }
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

void Master::TerminalSocket(const Request& request,
                            httplib::ws::WebSocket& socket) {
  const std::string device = WebSocketDevice(request);
  if (device.empty() || !terminals_) {
    socket.close(httplib::ws::CloseStatus::PolicyViolation);
    return;
  }
  // Each socket holds a worker for as long as it is open; the pool has
  // kWebTerminalSockets of them, apart from those for requests.
  if (terminal_sockets_.fetch_add(1) >= kWebTerminalSockets) {
    --terminal_sockets_;
    ForgetRemoteTerminal(request.matches[1].str(), true);
    socket.close(httplib::ws::CloseStatus::PolicyViolation);
    return;
  }
  if (RemoteTerminalSocket(request.matches[1].str(), socket)) {
    --terminal_sockets_;
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
  RelayTerminal(
      socket, [&] { return std::optional(viewer->Read(kRelayWait)); },
      [&](const std::string& input) {
        viewer->Write(input);
        return true;
      },
      [&](uint16_t rows, uint16_t cols) {
        viewer->Resize(rows, cols);
        return true;
      },
      [&] { viewer->Wake(); });
  --terminal_sockets_;
}

// Binary frames carry the shell's output out and the device's input in; a
// text frame is a control message, a resize in or an end out. A newer
// socket for the same shell replaces this one. `read` has nothing when its
// shell is out of reach, `wake` ends a `read` that waits.
TerminalOutput RelayTerminal(
    httplib::ws::WebSocket& socket,
    const std::function<std::optional<TerminalOutput>()>& read,
    const std::function<bool(const std::string&)>& write,
    const std::function<bool(uint16_t, uint16_t)>& resize,
    const std::function<void()>& wake) {
  std::atomic<bool> stopped{false};
  TerminalOutput last;
  std::thread relay([&] {
    while (!stopped) {
      const std::optional<TerminalOutput> out = read();
      if (!out || (!out->bytes.empty() &&
                   !socket.send(out->bytes.data(), out->bytes.size()))) {
        break;
      }
      if (out->ended || out->replaced) {
        last = *out;
        socket.send(JsonDump(out->ended
                                 ? json{{"type", "exit"}, {"code", out->code}}
                                 : json{{"type", "replaced"}}));
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
      if (!write(data)) break;
      continue;
    }
    const json message = json::parse(data, nullptr, false);
    const int64_t rows = JsonValue(message, "rows", int64_t{0});
    const int64_t cols = JsonValue(message, "cols", int64_t{0});
    if (JsonValue(message, "type", "") != "resize" || rows < 1 || cols < 1 ||
        rows > kPtyMaxDimension || cols > kPtyMaxDimension ||
        !resize(static_cast<uint16_t>(rows), static_cast<uint16_t>(cols))) {
      break;
    }
  }
  stopped = true;
  wake();
  socket.close();
  relay.join();
  return last;
}

// Opens a shell in a directory (the home folder without one), of a folder or
// the page's own, renames one or closes one.
void Master::TerminalCommand(CommandCall& call) {
  if (!terminals_) {
    call.error = "the terminal is turned off (web.terminal)";
    return;
  }
  if (RemoteTerminalCommand(call)) return;
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
