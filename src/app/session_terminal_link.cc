// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#include "include/agent/session_view.h"
#include "include/core/events.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/verbosity.h"
#include "include/ui/interactive.h"
#include "include/ui/presentation.h"
#include "src/app/session_terminal_internal.h"

namespace uagent::session {
namespace {
const json kNone = json::object();
}  // namespace

void Terminal::Reply(const std::string& id, json text, const json& extra) {
  json command = {
      {"kind", "reply"}, {"interaction_id", id}, {"text", std::move(text)}};
  command.update(extra);
  Send(std::move(command));
}

void Terminal::Send(json command) {
  std::lock_guard lock(send_mutex_);
  if (JsonValue(command, "kind", "") == "reply") interaction_ = false;
  StampFrame(command, HashHex(path_), connection_.generation);
  command["request_id"] = RandomToken(16);
  command["client_request_id"] = command["request_id"];
  {
    std::lock_guard state_lock(mutex_);
    own_requests_.insert(command["request_id"]);
    if (JsonValue(command, "kind", "") == "submit" ||
        JsonValue(command, "kind", "") == "fork" ||
        JsonValue(command, "kind", "") == "side" ||
        JsonValue(command, "kind", "") == "share") {
      if (raw_ && JsonValue(command, "kind", "") == "submit" &&
          !JsonValue(command, "text", "").starts_with('/')) {
        echoed_.insert(command["request_id"]);
      }
      waiting_ = command["request_id"];
      input_blocked_ = true;
    }
  }
  if (!WriteFrame(connection_.socket.Get(), command)) {
    failed_ = true;
    stop_.Wake();
  }
}

void Terminal::Receive(const json& frame) {
  auto kind = JsonValue(frame, "kind", "");
  if (kind == "state") {
    ReceiveState(frame);
  } else if (kind == "activity") {
    presenter_.Consume(AppEvent{0, "", "activity.status", frame, false});
    std::lock_guard lock(mutex_);
    state_["activity"] = JsonValue(frame, "activity", "Ready");
    wake_.Wake();
  } else if (kind == "event") {
    if (!ReceiveEvent(frame)) return;
  } else if (kind == "outcome") {
    if (!ReceiveOutcome(frame)) return;
  } else if (kind == "error") {
    failed_ = true;
    WriteTerminalRecord(TerminalSafe(JsonValue(frame, "error", "")) + "\n");
  } else if (kind == "gap") {
    Send({{"kind", "refresh"}});
  }
  fflush(stdout);
  wake_.Wake();
}

void Terminal::ReceiveState(const json& frame) {
  auto sent = frame.find("state");
  const json& state = sent != frame.end() ? *sent : kNone;
  {
    std::lock_guard lock(mutex_);
    for (const char* field :
         {"activity", "route", "usage", "turn_active", "turn_started_ms",
          "context_tokens", "context_window", "activities", "permissions"}) {
      if (state.contains(field)) state_[field] = state[field];
    }
    pending_ = JsonValue(frame, "pending", json(nullptr));
    running_ = JsonValue(frame, "busy", false);
    interaction_ = !pending_.is_null();
    if (JsonValue(frame, "checkpoint", false) &&
        (waiting_.empty() ||
         JsonValue(frame, "completed_request_id", "") == waiting_)) {
      waiting_.clear();
      input_blocked_ = JsonValue(frame, "command_busy", false);
    }
  }
  if (!history_) {
    if (const json* notices = JsonArray(state, "notices")) {
      for (const auto& notice : *notices) {
        presenter_.Consume(AppEvent{0, "", "notice", notice, false});
      }
    }
    const json* view = JsonObject(state, "view");
    if (const json* blocks = view ? JsonArray(*view, "blocks") : nullptr) {
      for (const auto& block : *blocks) {
        shown_.insert(JsonValue(block, "id", ""));
        presenter_.Block(block);
      }
      history_ = true;
      // Why the screen changed, where the eye is after a redraw.
      if (std::exchange(redrawn_, false)) {
        WriteTerminalRecord(
            Note(Tone::kNeutral, "verbosity " + std::string(Detail().level)));
      }
    }
  }
  // A panel holding thread events says why, once per change.
  if (std::string paused = JsonValue(state, "paused", ""); paused != paused_) {
    paused_ = std::move(paused);
    if (!paused_.empty()) {
      presenter_.Consume(
          NoticeEvent(PresentationStatus::kWarned, paused_, false));
    }
  }
  wake_.Wake();
}

bool Terminal::ReceiveEvent(const json& frame) {
  std::string type = JsonValue(frame, "type", "");
  auto sent = frame.find("data");
  const json& data = sent != frame.end() ? *sent : kNone;
  if (type == AppType(EventId::kSessionEnded)) {
    ended_ = true;
  }
  if (type == AppType(EventId::kNotice) && !history_) {
    return false;
  }
  if (type == AppType(EventId::kUsageUpdated)) {
    std::lock_guard lock(mutex_);
    ApplySessionEvent(state_, type, data);
    wake_.Wake();
  } else if (type == AppType(EventId::kMessageChanged)) {
    const json* found = JsonObject(data, "block");
    const json& block = found ? *found : kNone;
    const auto block_kind = JsonValue(block, "kind", "");
    bool echoed = false;
    {
      std::lock_guard lock(mutex_);
      echoed = echoed_.erase(JsonValue(block, "request_id", "")) > 0;
    }
    if (shown_.insert(JsonValue(block, "id", "")).second && !echoed &&
        block_kind == "user") {
      presenter_.Block(block);
    }
  } else if (type == AppType(EventId::kCommandCompleted) &&
             data.contains("output")) {
    std::string output = JsonValue(data, "output", "");
    if (output.empty() && data.contains("result") && !data["result"].empty()) {
      output = JsonDump(data["result"], 2);
    }
    // Command replies are this program's own rows, joins and all.
    if (!output.empty()) {
      WriteTerminalRecord(AsciiGlyphs(TerminalSafe(output)) + "\n");
    }
  } else {
    // The level changed elsewhere (another terminal, the web, the file).
    if (const json* changed = JsonArray(data, "changed");
        type == AppType(EventId::kConfigChanged) && !pinned_ && changed &&
        std::ranges::find(*changed, json(kVerbositySetting)) !=
            changed->end()) {
      if (const DetailPolicy* detail = &ConfiguredDetail({});
          detail != detail_) {
        Restyle(*detail);
      }
    }
    presenter_.Consume(
        AppEvent{0, JsonValue(frame, "time", ""), type, data, false});
  }
  return true;
}

bool Terminal::ReceiveOutcome(const json& frame) {
  {
    std::lock_guard lock(mutex_);
    if (JsonValue(frame, "pending", false) ||
        !own_requests_.erase(JsonValue(frame, "request_id", ""))) {
      return false;
    }
  }
  auto error = JsonValue(frame, "error", "");
  if (!error.empty()) {
    input_blocked_ = false;
    WriteTerminalRecord(TerminalSafe(error) + "\n");
  }
  const json result = JsonValue(frame, "result", json::object());
  if (result.contains("answer")) {
    WriteTerminalRecord(
        StyledBlock(AsciiGlyphs("side · not in history"), DIM()) +
        TerminalSafe(JsonValue(result, "answer", "")) + "\n");
  } else if (!result.empty()) {
    WriteTerminalRecord(TerminalSafe(JsonDump(result, 2)) + "\n");
  }
  // A panel rewinds in place: reopening its path shows the
  // shortened conversation, with the message back in the composer.
  if (JsonValue(result, "forked", false) ||
      JsonValue(result, "rewound", false)) {
    std::lock_guard lock(mutex_);
    next_ = JsonValue(result, "path", "");
    side_question_ = JsonValue(result, "side_question", "");
    if (rewinding_) carry_ = JsonValue(result, "prompt", "");
    navigate_ = true;
    wake_.Wake();
  }
  rewinding_ = false;
  return true;
}
}  // namespace uagent::session
