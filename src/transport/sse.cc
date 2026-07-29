// Copyright 2026 Timon Gentzsch

#include "include/transport/sse.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace uagent {

bool SseParser::Feed(std::string_view bytes) {
  if (!error_.empty()) return false;
  for (char byte : bytes) {
    if (pending_cr_) {
      pending_cr_ = false;
      if (!ProcessLine()) return false;
      if (byte == '\n') continue;
    }
    if (byte == '\r') {
      pending_cr_ = true;
    } else if (byte == '\n') {
      if (!ProcessLine()) return false;
    } else if (!Append(line_, std::string_view(&byte, 1))) {
      return false;
    }
  }
  return true;
}

bool SseParser::Finish() {
  if (!error_.empty()) return false;
  if (pending_cr_) {
    pending_cr_ = false;
    if (!ProcessLine()) return false;
  } else if (!line_.empty()) {
    if (!ProcessLine()) return false;
  }
  return Dispatch();
}

std::vector<SseEvent> SseParser::TakeEvents() {
  std::vector<SseEvent> events;
  events.swap(events_);
  return events;
}

bool SseParser::ProcessLine() {
  if (line_.empty()) return Dispatch();
  if (line_.front() == ':') {
    line_.clear();
    return true;
  }

  size_t separator = line_.find(':');
  std::string_view field(
      line_.data(), separator == std::string::npos ? line_.size() : separator);
  std::string_view value;
  if (separator != std::string::npos) {
    value = std::string_view(line_).substr(separator + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
  }

  bool ok = true;
  if (field == "data") {
    if (has_data_) ok = Append(data_, "\n");
    if (ok) ok = Append(data_, value);
    has_data_ = ok;
  } else if (field == "event") {
    event_.clear();
    ok = Append(event_, value);
  } else if (field == "id" && value.find('\0') == std::string_view::npos) {
    last_event_id_.clear();
    ok = Append(last_event_id_, value);
  }
  line_.clear();
  return ok;
}

bool SseParser::Dispatch() {
  line_.clear();
  if (!has_data_) {
    event_.clear();
    return true;
  }
  if (events_.size() >= kMaxPendingEvents) {
    error_ = "too many pending SSE events";
    return false;
  }
  events_.push_back({event_.empty() ? "message" : std::move(event_),
                     std::move(data_), last_event_id_});
  ResetEvent();
  return true;
}

bool SseParser::Append(std::string& target, std::string_view value) {
  if (max_event_bytes_ > 0 &&
      (target.size() > max_event_bytes_ ||
       value.size() > max_event_bytes_ - target.size())) {
    error_ =
        "SSE event exceeded " + std::to_string(max_event_bytes_) + " bytes";
    return false;
  }
  target.append(value);
  return true;
}

void SseParser::ResetEvent() {
  event_.clear();
  data_.clear();
  has_data_ = false;
}

}  // namespace uagent
