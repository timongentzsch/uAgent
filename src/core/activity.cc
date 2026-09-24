// Copyright 2026 Timon Gentzsch

#include "include/core/activity.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "include/core/events.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {

std::string ActivityLabel(std::string_view text) {
  std::string safe = TerminalSafe(Utf8Prefix(
      std::string(text.substr(0, kActivityLineBytes)), kActivityLabelBytes));
  std::string plain;
  plain.reserve(safe.size());
  bool separator = false;
  for (size_t index = 0; index < safe.size(); ++index) {
    unsigned char c = static_cast<unsigned char>(safe[index]);
    bool left_word =
        index > 0 && isalnum(static_cast<unsigned char>(safe[index - 1]));
    bool right_word = index + 1 < safe.size() &&
                      isalnum(static_cast<unsigned char>(safe[index + 1]));
    bool decoration = c == '`' ||
                      ((c == '*' || c == '_') && !(left_word && right_word)) ||
                      (c == '#' && !left_word);
    bool whitespace = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (decoration || whitespace) {
      separator = !plain.empty();
      continue;
    }
    if (separator && plain.back() != ' ') plain.push_back(' ');
    separator = false;
    plain.push_back(static_cast<char>(c));
  }
  plain = Trim(plain);
  while (plain.starts_with("- ") || plain.starts_with("> ")) {
    plain = Trim(plain.substr(2));
  }
  return Utf8Prefix(std::move(plain), kActivityLabelBytes);
}

void ActivityProjection::Reasoning(const json& data) {
  const std::string part = JsonValue(data, "part", "");
  if (part != part_ || JsonValue(data, "reasoning_reset", false)) {
    part_ = part;
    line_.clear();
    long_line_ = false;
  }
  if (JsonValue(data, "reasoning_reset", false)) excerpt_.clear();
  auto finish = [&] {
    if (!long_line_) {
      std::string label = ActivityLabel(line_);
      if (!label.empty()) {
        excerpt_ = std::move(label);
        reasoning_source_ = JsonValue(data, "reasoning_kind", "reasoning");
      }
    }
    line_.clear();
    long_line_ = false;
  };
  const char* text_field =
      data.contains("reasoning_text") ? "reasoning_text" : "text";
  for (char ch : JsonValue(data, text_field, "")) {
    if (ch == '\n' || ch == '\r') {
      finish();
    } else if (line_.size() < kActivityLineBytes) {
      line_ += ch;
    } else {
      long_line_ = true;
    }
  }
  if (JsonValue(data, "complete", false)) finish();
}

json ActivityProjection::Project() const {
  std::string phase = phase_, label, source = "lifecycle", occurrence;
  std::string response = response_;
  size_t running = 0;
  for (const auto& [id, call] : calls_) {
    if (call.running) ++running;
  }
  if (!decisions_.empty()) {
    phase = "decision";
    label = "Awaiting input or permission";
  } else if (retry_.is_object()) {
    phase = "retrying";
    label = "Retrying model · attempt " +
            std::to_string(JsonValue(retry_, "attempt", int64_t{0})) + "/" +
            std::to_string(JsonValue(retry_, "max_attempts", int64_t{0}));
  } else if (!calls_.empty()) {
    auto found =
        std::find_if(calls_.begin(), calls_.end(),
                     [](const auto& call) { return call.second.running; });
    if (found == calls_.end()) found = calls_.begin();
    const Call& call = found->second;
    phase = call.running ? "tool" : "preparing";
    label = (call.running ? "Running · " : "Preparing · ") + call.label;
    if (running > 1) label += " · +" + std::to_string(running - 1) + " tools";
    source = call.source;
    occurrence = found->first;
    response = call.response;
  } else if (!searches_.empty()) {
    phase = "searching";
    label = "Searching the web";
  } else if (phase == "thinking") {
    label = "Thinking";
    if (!excerpt_.empty()) {
      label += " · " + excerpt_;
      source = reasoning_source_;
    }
  } else {
    label = phase == "waiting"      ? "Waiting for model"
            : phase == "preparing"  ? "Preparing tool call"
            : phase == "responding" ? "Responding"
            : phase == "finishing"  ? "Finishing"
            : phase == "idle"       ? "Ready"
                                    : "Working";
  }
  json detail = {{"source", source},
                 {"label", label},
                 {"turn", turn_},
                 {"response_id", response},
                 {"occurrence_id", occurrence},
                 {"active_tools", running}};
  if (retry_.is_object()) detail["retry"] = retry_;
  return {{"phase", phase},
          {"activity", label},
          {"activity_detail", std::move(detail)}};
}

bool ActivityProjection::Consume(EventId id, const json& data) {
  // Auxiliary model calls (permission judge, titles, memory) have no turn
  // identity. They must not replace the conversation's response or caption.
  if (id == EventId::kResponseStarted && !data.contains("turn")) return false;
  if (id == EventId::kResponseRetry && !data.contains("turn")) return false;
  if (id == EventId::kTurnStarted) {
    active_ = true;
    turn_ = JsonValue(data, "turn", int64_t{0});
    response_.clear();
    calls_.clear();
    searches_.clear();
    decisions_.clear();
    retry_ = nullptr;
    phase_ = "working";
  } else {
    if (!active_ ||
        (data.contains("turn") && JsonValue(data, "turn", turn_) != turn_))
      return false;
    const std::string response = JsonValue(data, "response_id", "");
    if (id != EventId::kResponseStarted && !response.empty() &&
        response != response_)
      return false;
    switch (id) {
      case EventId::kResponseStarted:
        response_ = response;
        phase_ = "waiting";
        part_.clear();
        line_.clear();
        excerpt_.clear();
        long_line_ = false;
        retry_ = nullptr;
        searches_.clear();
        break;
      case EventId::kReasoningDelta: {
        const std::string previous = excerpt_;
        const std::string source = reasoning_source_;
        Reasoning(data);
        if (phase_ == "thinking" && previous == excerpt_ &&
            source == reasoning_source_)
          return false;
        phase_ = "thinking";
        break;
      }
      case EventId::kAnswerDelta:
        if (phase_ == "responding") return false;
        phase_ = "responding";
        break;
      case EventId::kToolArguments:
        phase_ = "preparing";
        break;
      case EventId::kToolCall:
      case EventId::kToolStarted: {
        const json activity = JsonValue(data, "activity", json::object());
        const std::string key = JsonValue(data, "occurrence_id", "");
        if (key.empty()) return false;
        Call& call = calls_[key];
        call.label = ActivityLabel(JsonValue(activity, "status_label",
                                             JsonValue(data, "name", "tool")));
        call.source = JsonValue(activity, "label_source", "tool");
        call.response = response;
        call.running = call.running || id == EventId::kToolStarted;
        phase_ = "working";
        break;
      }
      case EventId::kToolResult:
        calls_.erase(JsonValue(data, "occurrence_id", ""));
        break;
      case EventId::kHostedToolActivity: {
        const std::string phase = JsonValue(data, "phase", "");
        const std::string key = JsonValue(data, "id", "");
        if (phase == "started" || phase == "searching")
          searches_.insert(key);
        else
          searches_.erase(key);
        break;
      }
      case EventId::kResponseRetry:
        retry_ = data;
        break;
      case EventId::kResponseFinished:
        phase_ = "working";
        searches_.clear();
        break;
      case EventId::kApprovalRequested:
      case EventId::kInteractionRequested:
        decisions_.insert(std::string(id == EventId::kApprovalRequested
                                          ? "approval/"
                                          : "interaction/") +
                          JsonValue(data, "id", ""));
        break;
      case EventId::kApprovalResolved:
      case EventId::kInteractionResolved:
        decisions_.erase(std::string(id == EventId::kApprovalResolved
                                         ? "approval/"
                                         : "interaction/") +
                         JsonValue(data, "id", ""));
        break;
      case EventId::kTurnCompleted:
      case EventId::kTurnStopped:
      case EventId::kSessionEnded:
        active_ = false;
        calls_.clear();
        decisions_.clear();
        searches_.clear();
        retry_ = nullptr;
        phase_ = "finishing";
        break;
      default:
        return false;
    }
  }
  json next = Project();
  if (next == status_) return false;
  status_ = std::move(next);
  ++revision_;
  return true;
}

}  // namespace uagent
