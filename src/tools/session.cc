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
  const std::string peer = LinkedSessionPath(id);
  if (peer.empty()) {
    return ToolFailure(ToolErrorCode::kPermissionDenied,
                       "session " + id +
                           " is not linked with this one; operation=list "
                           "shows the sessions that are");
  }
  if (text.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "message requires prompt");
  }
  const std::string own = OwnSessionFile();
  const std::string title = JsonValue(SessionHeader(own), "title", "");
  Mail mail;
  mail.from = MailboxIdFor(own);
  mail.sender_path = own;
  mail.to = MailboxIdFor(peer);
  mail.type = kMailNote;
  mail.hops = hops;
  // Named as lists and boards name it, so the reply can address it.
  const std::string from = HashHex(own);
  mail.body = {
      {"text", "[session " +
                   (title.empty() ? from : OneLine(title) + " (" + from + ")") +
                   "]\n" + text}};
  const std::string error = SendMail(std::move(mail));
  return error.empty() ? ToolSuccess("sent to session " + id)
                       : ToolFailure(ToolErrorCode::kUnavailable, error);
}

Tool SessionTool(const std::function<void(const std::string& path)>& start) {
  json parameters = json{
      {"type", "object"},
      {"properties",
       {{"operation",
         {{"type", "string"},
          {"enum", json::array({"list", "message"})},
          {"description", "list linked/linkable sessions or message one"}}},
        {"session_id",
         {{"type", "string"}, {"description", "peer session id for message"}}},
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
           "message only: peer-forward count for loop clamping"}}}}}};
  Tool tool = MakeTool(
      "session",
      "Message another uagent session linked with this one (a "
      "coordinator's threads and yolo sessions link per folder). list shows "
      "linked, then linkable sessions; message reaches the peer at its next "
      "step, or starts its turn when it is idle; broadcast reaches every "
      "linked session. Unlinked "
      "sessions are refused.",
      parameters, [start](const json& arguments, const ToolContext&) {
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
        std::string single = JsonValue(arguments, "session_id", "");
        if (!single.empty()) targets.push_back(single);
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
          if (const std::string path = LinkedSessionPath(target);
              start && SocietySession(path)) {
            start(path);
          }
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
