// Copyright 2026 Timon Gentzsch
#include "include/app/execution.h"
#include "include/core/fs.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/transport/session.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace {
std::string RecordsPath() {
  return GlobalBase() + "/web/remote-terminals.json";
}
}  // namespace
void Master::LoadRemoteTerminals() {
  const json saved = ReadJsonFile(RecordsPath(), KiB(64));
  std::lock_guard lock(remote_terminals_mutex_);
  if (!saved.is_array() || saved.size() > kWebTerminals) return;
  for (const json& row : saved) {
    const std::string cwd = JsonValue(row, "cwd", "");
    if (!session::OpaqueId(JsonValue(row, "id", "")) ||
        !session::OpaqueId(JsonValue(row, "runtime", "")) ||
        !session::OpaqueId(JsonValue(row, "terminal", "")) ||
        !session::OpaqueId(JsonValue(row, "host", "")) || cwd.empty() ||
        cwd.front() != '/')
      continue;
    remote_terminals_.push_back(row);
  }
}
bool Master::RemoteTerminalCommand(CommandCall& call) {
  const std::string action = JsonValue(call.command, "action", "");
  const std::string id = JsonValue(call.command, "id", "");
  json record;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    for (const json& item : remote_terminals_)
      if (JsonValue(item, "id", "") == id) record = item;
  }
  const std::string host = record.is_null()
                               ? JsonValue(call.command, "host", "")
                               : JsonValue(record, "host", "");
  if (host.empty()) return false;
  if (action == "open") {
    {
      std::lock_guard lock(remote_terminals_mutex_);
      if (remote_terminals_.size() >= kWebTerminals) {
        call.error = "Terminal limit reached.";
        return true;
      }
    }
    const std::string runtime = RandomToken(16);
    record = {{"id", runtime},
              {"runtime", runtime},
              {"host", host},
              {"cwd", JsonValue(call.command, "cwd", "")},
              {"folder", JsonValue(call.command, "folder", "")},
              {"title", "Terminal"},
              {"started", NowMillis()}};
  } else if (record.is_null()) {
    call.error = "No such remote terminal.";
    return true;
  }
  const json target = remotes_->Target(host, JsonValue(record, "cwd", ""),
                                       JsonValue(record, "runtime", ""));
  if (target.is_null()) {
    call.error = "Host is disconnected. Choose Connect or Reconnect.";
    return true;
  }
  execution::Client remote(execution::TargetFromJson(target));
  json request = {{"op", "terminal"},
                  {"action", action},
                  {"terminal", JsonValue(record, "terminal", "")},
                  {"title", JsonValue(call.command, "title", "")}};
  const json result = remote.Call(request);
  call.error = JsonValue(result, "error", "");
  if (action == "close" && (call.error == "Remote terminal was interrupted." ||
                            call.error == "Terminal has ended."))
    call.error.clear();
  if (!call.error.empty()) return true;
  if (action == "open") {
    record["terminal"] = JsonValue(result, "terminal", "");
    const json identity = remote.Call({{"op", "ping"}});
    record["cwd"] = JsonValue(identity, "cwd", JsonValue(record, "cwd", ""));
  }
  bool abandon = false;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    if (action == "open") {
      if (remote_terminals_.size() >= kWebTerminals) {
        call.error = "Terminal limit reached.";
        abandon = true;
      } else
        remote_terminals_.push_back(record);
    } else {
      for (auto it = remote_terminals_.begin(); it != remote_terminals_.end();
           ++it) {
        if (JsonValue(*it, "id", "") != id) continue;
        if (action == "close")
          remote_terminals_.erase(it);
        else if (action == "rename")
          (*it)["title"] = JsonValue(call.command, "title", "");
        break;
      }
    }
    if (!abandon &&
        !WritePrivateFile(RecordsPath(), JsonDump(remote_terminals_),
                          call.error) &&
        action == "open") {
      for (auto it = remote_terminals_.begin(); it != remote_terminals_.end();
           ++it)
        if (JsonValue(*it, "id", "") == JsonValue(record, "id", "")) {
          remote_terminals_.erase(it);
          break;
        }
      abandon = true;
    }
  }
  if (abandon)
    remote.Call({{"op", "terminal"},
                 {"action", "close"},
                 {"terminal", JsonValue(record, "terminal", "")}});
  if (call.error.empty()) {
    const json terminals = TerminalList();
    call.outcome["result"] = {{"id", record["id"]}, {"terminals", terminals}};
    host_.Publish("", "",
                  {{"kind", "terminals.changed"}, {"terminals", terminals}});
  }
  return true;
}
bool Master::RemoteTerminalSocket(const std::string& id,
                                  httplib::ws::WebSocket& socket) {
  json record;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    for (const json& item : remote_terminals_)
      if (JsonValue(item, "id", "") == id) record = item;
  }
  if (record.is_null()) return false;
  const json target = remotes_->Target(JsonValue(record, "host", ""),
                                       JsonValue(record, "cwd", ""),
                                       JsonValue(record, "runtime", ""));
  if (target.is_null()) {
    socket.close();
    return true;
  }
  execution::Client remote(execution::TargetFromJson(target));
  const std::string viewer = RandomToken(16);
  auto command = [&](const std::string& action, json fields = json::object()) {
    fields["op"] = "terminal";
    fields["action"] = action;
    fields["viewer"] = viewer;
    fields["terminal"] = JsonValue(record, "terminal", "");
    return remote.Call(std::move(fields));
  };
  if (command("attach").contains("error")) {
    socket.send(JsonDump({{"type", "exit"}, {"code", -1}}));
    socket.close();
    return true;
  }
  std::atomic<bool> stopped{false};
  std::thread relay([&] {
    while (!stopped) {
      const json result = command("read");
      if (result.contains("error")) break;
      std::string bytes;
      if (!HexDecode(JsonValue(result, "data", ""), bytes, MiB(1))) break;
      if (!bytes.empty() && !socket.send(bytes.data(), bytes.size())) break;
      if (JsonValue(result, "ended", false)) {
        socket.send(JsonDump(
            {{"type", "exit"}, {"code", JsonValue(result, "code", -1)}}));
        break;
      }
      if (JsonValue(result, "replaced", false)) {
        socket.send(JsonDump({{"type", "replaced"}}));
        break;
      }
    }
    stopped = true;
    socket.close();
  });
  std::string data;
  while (!stopped) {
    const auto read = socket.read(data);
    if (read == httplib::ws::ReadResult::Fail ||
        data.size() > kWebTerminalMessageBytes)
      break;
    if (read == httplib::ws::ReadResult::Binary) {
      if (command("write", {{"data", HexEncode(data)}}).contains("error"))
        break;
    } else {
      const json value = json::parse(data, nullptr, false);
      if (JsonValue(value, "type", "") != "resize" ||
          command("resize", value).contains("error"))
        break;
    }
  }
  stopped = true;
  socket.close();
  relay.join();
  command("detach");
  return true;
}
}  // namespace uagent::web::master
