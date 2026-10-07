// Copyright 2026 Timon Gentzsch

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#include "include/agent/session_store.h"
#include "include/app/chat.h"
#include "include/app/coordinator.h"
#include "include/app/launch.h"
#include "include/app/session.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/transport/session.h"

namespace uagent::session {
// One request to a folder's coordinator from the command line: its answer,
// with what its threads reported, on standard output.
int CoordinatorPromptMain(const Options& options) {
  const std::string cwd = CanonicalCwd();
  const std::string path = CoordinatorPath(cwd);
  std::string error;
  auto connection = Open(ExecutablePath(), cwd, path, "", options, error);
  if (!connection.socket) {
    fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  auto send = [&](json command) {
    StampFrame(command, HashHex(path), connection.generation);
    command["request_id"] = command.value("request_id", RandomToken(16));
    return WriteFrame(connection.socket.Get(), command);
  };
  const std::string request = RandomToken(16);
  bool submitted = false, rejected = false, completed = false, busy = true;
  bool accepted = false, paused = false, done = false;
  std::string probe;  // the request that asks the coordinator how it stands
  json stop = json::object();
  // The coordinator's session runs on; this request is its growth.
  Usage before, after;
  const auto receive = [&](const json& frame) {
    const std::string kind = JsonValue(frame, "kind", "");
    if (kind == "outcome" && JsonValue(frame, "request_id", "") == request) {
      accepted = JsonValue(frame, "accepted", false);
      if (accepted) return true;
      error = JsonValue(frame, "error", "coordinator refused");
      rejected = true;
      return false;
    }
    // Its state came just before, and nothing is owed: it stands as it said.
    if (kind == "outcome" && !probe.empty() &&
        JsonValue(frame, "request_id", "") == probe) {
      probe.clear();
      done = !busy;
      return !done;
    }
    if (kind != "state") return true;
    if (!submitted && !JsonValue(frame, "busy", true)) {
      before =
          UsageFromJson(JsonValue(frame["state"], "usage", json::object()));
      submitted = send({{"kind", "submit"},
                        {"request_id", request},
                        {"text", options.prompt}});
      return submitted;
    }
    // Nobody is here to approve: a question is declined.
    if (const json* pending = JsonObject(frame, "pending")) {
      fprintf(stderr, "· declined: %s\n",
              TerminalSafe(JsonValue(*pending, "prompt", "approval")).c_str());
      send({{"kind", "reply"},
            {"interaction_id", JsonValue(*pending, "id", "")},
            {"text", ""}});
    }
    // A report it has taken waits as guidance until its turn starts.
    busy = JsonValue(frame, "busy", false) || JsonValue(frame, "guidance", 0);
    paused = frame["state"].contains("paused");
    // A request that met a turn a thread's report had just started was
    // taken into that turn, which ends under its own name: idle after the
    // request was accepted is its end too.
    if (!JsonValue(frame, "checkpoint", false) ||
        (!completed &&
         JsonValue(frame, "completed_request_id", "") != request &&
         (!accepted || busy))) {
      return true;
    }
    // A queued thread event may already have started the next turn, which
    // clears the stop record: the request's own is kept.
    if (!completed) stop = JsonValue(frame["state"], "stop", json::object());
    after = UsageFromJson(JsonValue(frame["state"], "usage", json::object()));
    completed = true;
    return true;
  };
  // The work it delegated is part of the answer. Each step is in place
  // before the one before it ends: a thread mails its report before it shows
  // idle, the coordinator queues a report before acknowledging it, and the
  // queue starts its turn. So when no thread owes anything, the coordinator
  // is asked, and its word that it is idle is the end. A thread that waits
  // for a person, or whose runtime is gone, is not waited for, nor are
  // reports the spend limit holds. One buffer for the whole wait: a frame
  // may arrive in pieces on either side of a tick.
  constexpr int kTickMs = 250;
  FrameBuffer frames;
  char buffer[4096];
  for (bool open = true; open && !rejected && !done;) {
    pollfd wait{connection.socket.Get(), POLLIN, 0};
    const int ready = poll(&wait, 1, kTickMs);
    if (ready < 0 && errno != EINTR) break;
    if (ready > 0) {
      const ssize_t count = read(wait.fd, buffer, sizeof buffer);
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      open = count > 0 &&
             frames.Feed(std::string_view(buffer, static_cast<size_t>(count)),
                         receive);
      continue;
    }
    if (!submitted || !completed || busy || !probe.empty() ||
        (!paused && ThreadsOwe(cwd))) {
      continue;
    }
    probe = RandomToken(16);
    open = send({{"kind", "refresh"}, {"request_id", probe}});
  }
  // A runtime that closed with work still owed has not answered.
  if (rejected || !done) {
    fprintf(stderr, "%s\n",
            error.empty() ? "coordinator runtime closed" : error.c_str());
    return 1;
  }
  // Everything the coordinator and its chat's members said since the
  // request, in full: a thread's report reopens its turn or starts another,
  // and each says part of it.
  std::string answer;
  SessionLoadResult saved = SessionStore::Inspect(path);
  Conversation conversation;
  if (saved.record &&
      std::move(saved.record->state).RestoreConversation(conversation)) {
    const json& messages = conversation.Messages();
    size_t asked = messages.size();
    for (size_t index = messages.size(); index > 0 && asked == messages.size();
         --index) {
      const json& message = messages[index - 1];
      if (JsonValue(message, "role", "") == "user" &&
          Trim(JsonValue(message, "content", "")) == Trim(options.prompt)) {
        asked = index - 1;
      }
    }
    for (size_t index = asked + 1; index < messages.size(); ++index) {
      const std::string content = JsonValue(messages[index], "content", "");
      // What it passed on or waited over is not part of what was said.
      const std::string text =
          JsonValue(messages[index], "role", "") != "assistant"
              ? ChatPost(content)
          : SilentAnswer(Trim(content)) ? std::string()
                                        : content;
      if (!text.empty()) answer += (answer.empty() ? "" : "\n\n") + text;
    }
    // The request itself was compacted away: what was said last.
    if (asked == messages.size()) answer = conversation.LastAssistantText();
  }
  if (options.json) {
    printf("%s\n",
           JsonDump({{"answer", answer},
                     {"session_id", HashHex(path)},
                     {"usage", UsageJson(UsageDifference(after, before))},
                     {"stop", stop}})
               .c_str());
  } else {
    printf("%s\n", answer.c_str());
  }
  const std::string reason = JsonValue(stop, "reason", "completed");
  if (reason == "completed") return 0;
  // Why there is no answer, where a person running this would look for it.
  if (!options.json) {
    const std::string detail = JsonValue(stop, "detail", "");
    fprintf(
        stderr, "%s\n",
        TerminalSafe(detail.empty() ? "the turn stopped: " + reason : detail)
            .c_str());
  }
  return 1;
}
}  // namespace uagent::session
