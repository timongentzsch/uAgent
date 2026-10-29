// Copyright 2026 Timon Gentzsch

#include "include/tools/session.h"

#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/file_services.h"
#include "include/agent/session_links.h"
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
                           " is not linked with this one; the session tool "
                           "lists the sessions that are");
  }
  const std::string error = SendSessionMail({.from_path = OwnSessionFile(),
                                             .to_path = peer,
                                             .text = text,
                                             .hops = hops});
  if (!error.empty()) return ToolFailure(ToolErrorCode::kUnavailable, error);
  return ToolSuccess("sent to session " + id);
}

Tool SessionTool() {
  Tool tool = MakeTool(
      "session",
      "List the uagent sessions linked with this one (its parent, children "
      "and siblings), then the folder's others; session_message reaches the "
      "linked ones.",
      {{"type", "object"}, {"properties", json::object()}},
      [](const json&, const ToolContext&) {
        std::vector<json> peers = SessionSummaries();
        return ToolSuccess(peers.empty() ? "no linked sessions"
                                         : JsonDump(peers, 2));
      });
  tool.available_in_lean = true;
  tool.category = "system";
  tool.intent = "delegate";
  tool.headline = [](const json&) {
    return Headline{"Listing", "Listed", "sessions"};
  };
  return tool;
}

Tool SessionMessageTool(
    const std::function<void(const std::string& path)>& start) {
  json parameters =
      json{{"type", "object"},
           {"properties",
            {{"session_id",
              {{"type", "string"}, {"description", "peer session id"}}},
             {"broadcast",
              {{"type", "boolean"},
               {"description", "fan out to every linked session"}}},
             {"prompt", {{"type", "string"}, {"minLength", 1}}}}},
           {"required", json::array({"prompt"})}};
  Tool tool = MakeTool(
      "session_message",
      "Message a session linked with this one, by session_id, or every "
      "linked one with broadcast: it reads the prompt at its next step, or "
      "starts a turn on it when idle. Unlinked sessions are refused.",
      parameters, [start](const json& arguments, const ToolContext& context) {
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
                             "name a session_id, or broadcast");
        }
        const std::string prompt = JsonValue(arguments, "prompt", "");
        std::string combined;
        for (const std::string& target : targets) {
          ToolResult one = MessageSession(target, prompt, context.hops);
          if (!combined.empty()) combined += "\n";
          combined +=
              one.Ok() ? one.output : ("error " + target + ": " + one.output);
          if (!one.Ok()) return ToolFailure(one.error, combined);
          // A panel or thread found stopped is started; any other session
          // reads its mail when it is next opened.
          if (const std::string path = LinkedSessionPath(target);
              start &&
              !JsonValue(SessionHeader(path), kSessionHeaderKind, "").empty()) {
            start(path);
          }
        }
        return ToolSuccess(combined);
      });
  tool.available_in_lean = true;
  tool.category = "system";
  tool.intent = "delegate";
  tool.headline = Verbs("Messaging", "Messaged");
  return tool;
}

}  // namespace uagent
