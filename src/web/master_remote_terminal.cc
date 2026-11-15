// Copyright 2026 Timon Gentzsch
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
// A shell of no folder is the page's own: it ends with its viewer.
bool Panel(const json& record) {
  return JsonValue(record, "folder", "").empty();
}
auto FindTerminal(json& records, const std::string& id) {
  return std::ranges::find_if(
      records, [&](const json& row) { return JsonValue(row, "id", "") == id; });
}
// Folder shells are saved, for a restarted host to attach again.
bool SaveTerminals(const json& records, std::string& error) {
  json saved = json::array();
  for (const json& row : records) {
    if (!Panel(row)) saved.push_back(row);
  }
  return WritePrivateFile(RecordsPath(), JsonDump(saved), error);
}
// The record's execution context, its own; null while its host is
// disconnected.
std::unique_ptr<execution::Client> TerminalClient(const RemoteHosts& hosts,
                                                  const json& record) {
  const json target =
      hosts.Target(JsonValue(record, "host", ""), JsonValue(record, "cwd", ""),
                   JsonValue(record, "id", ""));
  return target.is_null() ? nullptr
                          : std::make_unique<execution::Client>(
                                execution::TargetFromJson(target));
}
void EndShell(execution::Client& remote, const json& record) {
  remote.Call({{"op", "terminal"},
               {"action", "close"},
               {"terminal", JsonValue(record, "terminal", "")}});
  remote.Release();
}
}  // namespace
void Master::ForgetRemoteTerminal(const std::string& id, bool unviewed) {
  json record;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    const auto found = FindTerminal(remote_terminals_, id);
    if (found == remote_terminals_.end() ||
        (unviewed && (!Panel(*found) || JsonValue(*found, "viewed", false)))) {
      return;
    }
    record = *found;
    remote_terminals_.erase(found);
    std::string ignored;
    SaveTerminals(remote_terminals_, ignored);
  }
  if (const auto remote = TerminalClient(*remotes_, record)) {
    EndShell(*remote, record);
  }
  host_.Publish("", "",
                {{"kind", "terminals.changed"}, {"terminals", TerminalList()}});
}
void Master::LoadRemoteTerminals() {
  const json saved = ReadJsonFile(RecordsPath(), KiB(64));
  std::lock_guard lock(remote_terminals_mutex_);
  if (!saved.is_array() || saved.size() > kWebTerminals) return;
  for (const json& row : saved) {
    const std::string cwd = JsonValue(row, "cwd", "");
    if (!session::OpaqueId(JsonValue(row, "id", "")) ||
        !session::OpaqueId(JsonValue(row, "terminal", "")) ||
        !session::OpaqueId(JsonValue(row, "host", "")) || cwd.empty() ||
        cwd.front() != '/' || Panel(row)) {
      continue;
    }
    remote_terminals_.push_back(row);
  }
}
bool Master::RemoteTerminalCommand(CommandCall& call) {
  const std::string action = JsonValue(call.command, "action", "");
  const std::string id = JsonValue(call.command, "id", "");
  json record;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    const auto found = FindTerminal(remote_terminals_, id);
    if (found != remote_terminals_.end()) record = *found;
  }
  const std::string host =
      JsonValue(record.is_null() ? call.command : record, "host", "");
  if (host.empty() || (action != "open" && record.is_null()) ||
      (action != "open" && action != "close" && action != "rename")) {
    return false;
  }
  if (action == "close") {
    ForgetRemoteTerminal(id);
    call.outcome["result"] = {{"id", id}, {"terminals", TerminalList()}};
    return true;
  }
  const std::string folder = JsonValue(call.command, "folder", "");
  if (action == "open") {
    std::vector<std::string> expired;
    {
      std::lock_guard lock(remote_terminals_mutex_);
      for (const json& row : remote_terminals_) {
        if (Panel(row) && !JsonValue(row, "viewed", false) &&
            NowMillis() - JsonValue(row, "started", int64_t{0}) >=
                kTerminalFirstViewerWait.count() * 1000) {
          expired.push_back(JsonValue(row, "id", ""));
        }
      }
    }
    for (const auto& stale : expired) ForgetRemoteTerminal(stale, true);
    {
      std::lock_guard lock(remote_terminals_mutex_);
      if (remote_terminals_.size() >= kWebTerminals) {
        call.error = "Terminal limit reached.";
        return true;
      }
    }
    record = {{"id", RandomToken(16)},
              {"host", host},
              {"cwd", JsonValue(call.command, "cwd", "")},
              {"folder", folder},
              {"title", "Terminal"},
              {"started", NowMillis()}};
  }
  const auto remote = TerminalClient(*remotes_, record);
  if (!remote) {
    call.error = kHostDisconnected;
    return true;
  }
  const json result =
      remote->Call({{"op", "terminal"},
                    {"action", action},
                    {"folder", folder},
                    {"terminal", JsonValue(record, "terminal", "")},
                    {"title", JsonValue(call.command, "title", "")}});
  call.error = JsonValue(result, "error", "");
  if (!call.error.empty()) return true;
  if (action == "open") {
    record["terminal"] = JsonValue(result, "terminal", "");
    record["cwd"] = JsonValue(remote->Call({{"op", "ping"}}), "cwd",
                              JsonValue(record, "cwd", ""));
  }
  {
    std::lock_guard lock(remote_terminals_mutex_);
    json next = remote_terminals_;
    if (action == "rename") {
      const auto found = FindTerminal(next, id);
      if (found != next.end()) {
        (*found)["title"] = JsonValue(call.command, "title", "");
      }
    } else if (next.size() >= kWebTerminals) {
      call.error = "Terminal limit reached.";
    } else {
      // A folder's shells are numbered as its local ones are: the lowest
      // "Terminal N" none of them has.
      for (size_t number = 1; !folder.empty(); ++number) {
        const std::string title = "Terminal " + std::to_string(number);
        record["title"] = title;
        if (std::ranges::none_of(next, [&](const json& row) {
              return JsonValue(row, "host", "") == host &&
                     JsonValue(row, "folder", "") == folder &&
                     JsonValue(row, "title", "") == title;
            })) {
          break;
        }
      }
      next.push_back(record);
    }
    if (call.error.empty() && SaveTerminals(next, call.error)) {
      remote_terminals_ = std::move(next);
    }
  }
  if (!call.error.empty()) {
    if (action == "open") EndShell(*remote, record);
    return true;
  }
  const json terminals = TerminalList();
  call.outcome["result"] = {{"id", record["id"]}, {"terminals", terminals}};
  host_.Publish("", "",
                {{"kind", "terminals.changed"}, {"terminals", terminals}});
  return true;
}
bool Master::RemoteTerminalSocket(const std::string& id,
                                  httplib::ws::WebSocket& socket) {
  json record;
  {
    std::lock_guard lock(remote_terminals_mutex_);
    const auto found = FindTerminal(remote_terminals_, id);
    if (found == remote_terminals_.end()) return false;
    (*found)["viewed"] = true;
    record = *found;
  }
  const auto remote = TerminalClient(*remotes_, record);
  if (!remote) {
    if (Panel(record)) ForgetRemoteTerminal(id);
    socket.close();
    return true;
  }
  const std::string viewer = RandomToken(16);
  auto command = [&](const std::string& action, json fields = json::object()) {
    fields["op"] = "terminal";
    fields["action"] = action;
    fields["viewer"] = viewer;
    fields["terminal"] = JsonValue(record, "terminal", "");
    return remote->Call(std::move(fields));
  };
  const std::string attach_error = JsonValue(command("attach"), "error", "");
  if (!attach_error.empty()) {
    // A transport failure must not discard a surviving folder shell's identity.
    if (Panel(record) || attach_error == "Remote terminal was interrupted.") {
      ForgetRemoteTerminal(id);
    }
    socket.send(JsonDump({{"type", "exit"}, {"code", -1}}));
    socket.close();
    return true;
  }
  const TerminalOutput last = RelayTerminal(
      socket,
      [&]() -> std::optional<TerminalOutput> {
        // The host's shutdown ends the relay: the pool waits for its socket.
        if (stopping_) return std::nullopt;
        const json result = command("read");
        TerminalOutput out{.ended = JsonValue(result, "ended", false),
                           .replaced = JsonValue(result, "replaced", false),
                           .code = JsonValue(result, "code", -1)};
        if (result.contains("error") ||
            !HexDecode(JsonValue(result, "data", ""), out.bytes, MiB(1))) {
          return std::nullopt;
        }
        return out;
      },
      [&](const std::string& input) {
        return !command("write", {{"data", HexEncode(input)}})
                    .contains("error");
      },
      [&](uint16_t rows, uint16_t cols) {
        return !command("resize", {{"rows", rows}, {"cols", cols}})
                    .contains("error");
      },
      [] {});
  command("detach");
  if (!last.replaced && (last.ended || Panel(record))) {
    ForgetRemoteTerminal(id);
    // A concurrent close command may already have removed the record; detach
    // can still have reattached its forward while the socket was winding down.
    remote->Release();
  }
  return true;
}
}  // namespace uagent::web::master
