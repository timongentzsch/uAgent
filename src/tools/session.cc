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
#include "include/core/mailbox.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/transport/session.h"

namespace uagent {

ToolResult MessageSession(const std::string& id, const std::string& text,
                          int hops) {
  const std::string me = OwnSessionId();
  if (me.empty()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "no saved session file yet; say something first "
                       "so the session persists, then message");
  }
  if (id.empty() || SafeFileComponent(id) != id) {
    return ToolFailure(ToolErrorCode::kNotFound, "unknown session " + id);
  }
  if (!SharesLink(me, id)) {
    return ToolFailure(ToolErrorCode::kPermissionDenied,
                       "session " + id + " is not linked with this one");
  }
  if (text.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "message requires text");
  }
  const std::string own = OwnSessionFile();
  const std::string title = JsonValue(SessionHeader(own), "title", "");
  Mail mail;
  mail.from = MailboxIdFor(own);
  mail.sender_path = own;
  mail.to = MailboxIdFor(LinkedSessionPath(id));
  mail.type = kMailNote;
  mail.hops = hops;
  mail.body = {{"text", "[session " +
                            (title.empty() || title == me
                                 ? me
                                 : OneLine(title) + " (" + me + ")") +
                            "]\n" + text}};
  const std::string error = SendMail(std::move(mail));
  return error.empty() ? ToolSuccess("sent to session " + id)
                       : ToolFailure(ToolErrorCode::kUnavailable, error);
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
      "sessions auto-link per workspace). list shows "
      "linked, then linkable sessions; message reaches the peer at its next "
      "step, or starts its turn when it is idle; broadcast reaches every "
      "linked session. Unlinked "
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
        const int hops =
            static_cast<int>(JsonValue(arguments, "hops", int64_t{0}));
        std::string combined;
        for (const std::string& target : targets) {
          ToolResult one = MessageSession(target, prompt, hops);
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

}  // namespace uagent
