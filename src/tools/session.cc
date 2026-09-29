// Copyright 2026 Timon Gentzsch

#include "include/tools/session.h"

#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/file_services.h"
#include "include/agent/session_store.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/transport/session.h"

namespace uagent {
namespace {

constexpr int kSessionMailMaxHops = 8;
}  // namespace


ToolResult MessageSession(const std::string& id, const std::string& text,
                          const std::string& from, int hops) {
  const std::string me = OwnSessionId();
  if (me.empty()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "no saved session file yet; say something first "
                       "so the session persists, then message");
  }
  if (id.empty() || SafeFileComponent(id) != id) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "unknown session " + id);
  }
  if (!SharesLink(me, id)) {
    return ToolFailure(
        ToolErrorCode::kPermissionDenied,
        "session " + id + " is not linked; join its link first (/link)");
  }
  if (text.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "message requires text");
  }
  if (hops >= kSessionMailMaxHops) {
    return ToolSuccess("dropped message for session " + id + " [loop-clamped]");
  }
  ToolResult saved = WriteSessionMail(id, text, from.empty() ? me : from, hops);
  return saved.Ok() ? ToolSuccess("queued message for session " + id) : saved;
}

Tool SessionTool() {
  json parameters = json{
      {"type", "object"},
      {"properties",
       {{"operation",
         {{"type", "string"},
          {"enum", json::array({"list", "message"})},
          {"description", "list linked/linkable sessions or message one"}}},
        {"session_id",
         {{"type", json::array({"string", "array"})},
          {"items", {{"type", "string"}}},
          {"description",
           "peer session id for message; accepts an array for fan-out"}}},
        {"broadcast",
         {{"type", "boolean"},
          {"description", "message only: fan out to every linked session"}}},
        {"prompt",
         {{"type", "string"},
          {"description", "message text for operation=message"}}},
        {"hops",
         {{"type", "integer"},
          {"minimum", 0},
          {"maximum", 8},
          {"description",
           "message only: peer-forward count for loop clamping"}}}}},
      {"required", json::array({"prompt"})}};
  Tool tool = MakeTool(
      "session",
      "Message another live uagent session linked with this one (yolo "
      "sessions auto-link per workspace; otherwise /link TOKEN). list shows "
      "linked, then linkable sessions; message queues text the peer reads "
      "at its next step; broadcast reaches every linked session. Unlinked "
      "sessions are refused.",
      parameters, [](const json& arguments, const ToolContext&) {
        (void)EnsureSessionAutoLink();
        std::string operation = JsonValue(arguments, "operation", "list");
        if (operation == "list") {
          std::vector<json> peers = SessionSummaries();
          return ToolSuccess(peers.empty() ? "no linked sessions"
                                           : JsonDump(peers, 2));
        }
        if (operation != "message") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "unknown operation " + operation);
        }
        std::string prompt = JsonValue(arguments, "prompt", "");
        std::vector<std::string> targets;
        const json* ids = JsonArray(arguments, "session_id");
        if (ids != nullptr) {
          for (const json& entry : *ids) {
            if (entry.is_string()) targets.push_back(entry.get<std::string>());
          }
        } else {
          std::string single = JsonValue(arguments, "session_id", "");
          if (!single.empty()) targets.push_back(single);
        }
        if (targets.empty() && JsonValue(arguments, "broadcast", false)) {
          for (const json& row : SessionSummaries()) {
            if (JsonValue(row, "linked", false)) {
              targets.push_back(JsonValue(row, "id", ""));
            }
          }
        }
        if (targets.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "message requires session_id or broadcast");
        }
        const std::string me = OwnSessionId();
        const int hops =
            static_cast<int>(JsonValue(arguments, "hops", int64_t{0}));
        std::string combined;
        for (const std::string& target : targets) {
          ToolResult one = MessageSession(target, prompt, me, hops);
          if (!combined.empty()) combined += "\n";
          combined +=
              one.Ok() ? one.output : ("error " + target + ": " + one.output);
          if (!one.Ok()) return ToolFailure(one.error, combined);
        }
        return ToolSuccess(combined);
      });
  tool.available_in_lean = true;
  tool.intent = "delegate";
  tool.header = [](const json& arguments) {
    return JsonValue(arguments, "operation", "") == "list"
               ? json{{"verb", {"Listing", "Listed"}}, {"target", "sessions"}}
               : json{{"verb", {"Messaging", "Messaged"}}};
  };
  return tool;
}

std::string SessionText(const json& result) {
  const json* rows = JsonArray(result, "sessions");
  if (rows == nullptr) return JsonDump(result, 2) + "\n";
  std::string text =
      "sessions (" + FmtCount(static_cast<int64_t>(rows->size())) + ")\n";
  for (const json& row : *rows) {
    const std::string id = JsonValue(row, "id", "");
    std::string title = JsonValue(row, "title", "");
    if (title.empty()) title = id;
    text += (JsonValue(row, "linked", false) ? "" : "[unlinked] ") + title;
    if (title != id) text += " (" + id + ")";
    text += "\n";
  }
  return text;
}

json SessionSlashPeers() {
  (void)EnsureSessionAutoLink();
  return {{"sessions", SessionSummaries()}};
}

json SessionSlashTell(const std::string& argument) {
  (void)EnsureSessionAutoLink();
  std::string args = Trim(argument);
  size_t space = args.find_first_of(" \t");
  if (space == std::string::npos) {
    return {{"error", "usage: /tell ID TEXT"}};
  }
  std::string text = Trim(args.substr(space));
  if (text.empty()) return {{"error", "usage: /tell ID TEXT"}};
  ToolResult sent =
      MessageSession(args.substr(0, space), text, OwnSessionId(), 0);
  return sent.Ok() ? json{{"output", sent.output}}
                   : json{{"error", sent.output}};
}

json SessionSlashLink(const std::string& argument) {
  std::string token = Trim(argument);
  if (token.empty()) {
    std::string created;
    ToolResult made = CreateSessionLink(created);
    if (!made.Ok()) return {{"error", made.output}};
    return {{"output", "link token: " + created +
                           "\nhand it to another session as /link " + created}};
  }
  ToolResult joined = JoinSessionLink(token);
  if (!joined.Ok()) return {{"error", joined.output}};
  return {{"output", "joined link " + token}};
}

}  // namespace uagent
