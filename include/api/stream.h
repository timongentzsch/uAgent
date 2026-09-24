// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_STREAM_H_
#define UAGENT_INCLUDE_API_STREAM_H_
// Streaming transport state. Most API consumers should include api.h instead.

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/tool_protocol.h"
#include "include/api/types.h"
#include "include/api/wire.h"
#include "include/core/checked.h"
#include "include/core/events.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/transport/sse.h"

namespace uagent {

// Incremental SSE parser; emits provider-independent reasoning and answer
// streams.
struct StreamCtx {
  json event_context = json::object();
  std::function<void(const json&, size_t)> observe_progress;
  json last_usage;
  size_t last_response_bytes = 0;
  CURL* handle = nullptr;
  ChatResult* res = nullptr;
  std::string error_body;  // body when HTTP status >= 400
  int64_t status = 0;
  std::map<int, ToolCall> calls;  // keyed by stream index
  WireApi wire_api = WireApi::kChatCompletions;
  WireStreamState wire_state;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point last_byte;
  // 0 is unbounded here as everywhere else; a real request overwrites all
  // three from RuntimeConfig, which is where the tunable defaults live.
  int64_t first_event_timeout_s = 0;
  int64_t idle_timeout_s = 0;
  size_t response_cap = 0;
  size_t received = 0;
  std::string timeout_reason;
  SseParser sse;
  std::vector<SseEvent> events;  // reused across write callbacks
  struct ReasoningSpan {
    size_t offset = 0, bytes = 0;
    bool complete = false;
  };
  std::map<std::string, ReasoningSpan> reasoning_parts;
  bool tool_arguments_started = false;

  // Hold leading content back while it could still be any tool protocol, so
  // neither our fallback blocks nor malformed provider markup flashes as an
  // answer. Classification lives beside the protocol constants and is shared
  // with the final response validator.
  enum class Show { kUndecided, kPrint, kSuppress } show = Show::kUndecided;

  void MarkEvent() {
    res->semantic_progress = true;
    if (res->first_event_ms < 0) res->first_event_ms = ElapsedMs(started);
  }

  void OutputText(const std::string& value) {
    json data = event_context;
    data["text"] = value;
    data["offset"] = res->content.size() - value.size();
    Event event{EventId::kAnswerDelta, std::move(data)};
    event.text = value;
    Emit(std::move(event));
  }

  void OutputReasoning(const ReasoningDelta& fragment) {
    if (fragment.text.empty() && !reasoning_parts.contains(fragment.part)) {
      return;
    }
    auto [found, inserted] = reasoning_parts.try_emplace(fragment.part);
    ReasoningSpan& span = found->second;
    if (span.complete && !fragment.complete) return;
    std::string addition = fragment.text;
    size_t erase = 0;
    std::string separator;
    if (inserted) {
      if (!res->reasoning.empty()) separator = "\n\n";
      span.offset = res->reasoning.size();
    } else if (fragment.snapshot) {
      const std::string_view previous(res->reasoning.data() + span.offset,
                                      span.bytes);
      if (addition == previous && span.complete == fragment.complete) return;
      if (addition.starts_with(previous)) {
        addition.erase(0, span.bytes);
      } else {
        erase = span.bytes;
      }
    } else if (span.complete) {
      return;
    }
    const size_t offset = erase ? span.offset : span.offset + span.bytes;
    const bool reset = offset != res->reasoning.size() || erase > 0;
    const std::string patch = separator + addition;
    res->reasoning.replace(offset, erase, patch);
    for (auto& [key, other] : reasoning_parts) {
      if (key != fragment.part && other.offset >= offset) {
        other.offset = other.offset - erase + patch.size();
      }
    }
    if (inserted) span.offset += separator.size();
    span.bytes = span.bytes - erase + addition.size();
    span.complete = fragment.complete;
    json data = event_context;
    data["text"] = reset ? res->reasoning : patch;
    if (reset) data["append_text"] = patch;  // Append-only terminal stream.
    if (erase) data["corrected"] = true;
    data["offset"] = reset ? 0 : offset;
    data["reset"] = reset;
    data["part"] = fragment.part;
    data["reasoning_kind"] = fragment.kind;
    // Captions consume new text only; a corrected final snapshot resets its
    // bounded line reader. Transcript reconciliation never enters replay.
    const bool complete_line =
        fragment.complete && span.bytes <= kActivityLineBytes;
    data["reasoning_text"] =
        complete_line ? res->reasoning.substr(span.offset, span.bytes)
                      : addition;
    data["reasoning_reset"] = erase > 0 || complete_line;
    data["complete"] = fragment.complete;
    Event event{EventId::kReasoningDelta, std::move(data)};
    event.text = patch;
    Emit(std::move(event));
  }

  void EmitContent(const std::string& c) {
    res->content += c;
    if (show == Show::kPrint) {
      OutputText(c);
      return;
    }
    if (show == Show::kSuppress) return;
    LeadingToolMarkup classification = ClassifyLeadingToolMarkup(res->content);
    if (classification == LeadingToolMarkup::kProse) {
      show = Show::kPrint;
      OutputText(res->content);
    } else if (classification == LeadingToolMarkup::kCall) {
      show = Show::kSuppress;
      res->suppressed = true;
    }  // else: still a possible marker prefix — keep holding
  }

  // This runs inside a libcurl callback, so malformed server JSON is validated
  // explicitly and never crosses the C boundary.
  void HandleEvent(const SseEvent& event) {
    WireStreamDelta delta =
        DecodeWireStreamEvent(wire_api, event.data, *res, calls, wire_state);
    // Semantic progress and rendering are independent: a hosted search feeds
    // the timeout the same way every other event does, and additionally says
    // what it was so a presenter need not infer it from silence.
    if (delta.activity) MarkEvent();
    if (res->first_token_ms < 0 &&
        (!delta.content.empty() || !delta.reasoning.empty())) {
      res->first_token_ms = ElapsedMs(started);
    }
    if (delta.hosted_tool) {
      json data = HostedToolJson(*delta.hosted_tool);
      data.update(event_context);
      Emit(Event{EventId::kHostedToolActivity, std::move(data)});
    }
    for (const ReasoningDelta& fragment : delta.reasoning_parts) {
      OutputReasoning(fragment);
    }
    if (delta.tool_arguments && !tool_arguments_started) {
      tool_arguments_started = true;
      Emit(Event{EventId::kToolArguments, event_context});
    }
    if (!delta.content.empty()) EmitContent(delta.content);
    if (observe_progress) {
      size_t bytes = SaturatingAdd(res->content.size(), res->reasoning.size());
      for (const auto& [index, call] : calls) {
        bytes = SaturatingAdd(
            bytes, SaturatingAdd(call.name.size(), call.args.size()));
      }
      if (bytes != last_response_bytes || res->usage != last_usage) {
        last_response_bytes = bytes;
        last_usage = res->usage;
        observe_progress(last_usage, bytes);
      }
    }
  }

  size_t Feed(const char* data, size_t len) {
    last_byte = std::chrono::steady_clock::now();
    std::optional<size_t> total = CheckedAdd(received, len);
    if (!total) {
      res->error = "response size overflow";
      return 0;
    }
    received = *total;
    if (response_cap > 0 && received > response_cap) {
      res->error =
          "response exceeded " + std::to_string(response_cap) + " bytes";
      return 0;
    }
    if (status == 0) curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 400) {
      error_body.append(data, len);
      return len;
    }
    if (!Drain(sse.Feed(std::string_view(data, len)))) return 0;
    return len;
  }

  void Finish() {
    Drain(sse.Finish());
    if (show != Show::kUndecided || res->content.empty()) return;
    if (ClassifyLeadingToolMarkup(res->content, /*complete=*/true) ==
        LeadingToolMarkup::kCall) {
      show = Show::kSuppress;
      res->suppressed = true;
      return;
    }
    show = Show::kPrint;
    OutputText(res->content);
  }

  // Surface a parser failure, else hand every completed event to the decoder.
  bool Drain(bool parsed) {
    if (!parsed) {
      res->error = sse.Error();
      return false;
    }
    sse.TakeEvents(events);
    for (const SseEvent& event : events) HandleEvent(event);
    return true;
  }
};

inline bool CollectToolCalls(std::map<int, ToolCall>& streamed,
                             ChatResult& result) {
  std::set<std::string> ids;
  for (auto it = streamed.begin(); it != streamed.end();) {
    auto current = it++;
    int index = current->first;
    ToolCall& call = current->second;
    json arguments = json::parse(call.args, nullptr, false);
    if (call.name.empty() || arguments.is_discarded() ||
        !arguments.is_object()) {
      const bool truncated =
          it == streamed.end() &&
          (result.stop_cause == ResponseStopCause::kLength ||
           result.stop_cause == ResponseStopCause::kInputLimit);
      if (truncated) {
        DebugLog(
            "partial_tool_call_dropped",
            {{"stream_index", index}, {"finish_reason", result.finish_reason}});
        streamed.erase(current);
        break;
      }
      result.error = "invalid model tool call: incomplete function";
      return false;
    }
    std::string original = call.id;
    std::string base =
        original.empty() ? "uagent-call-" + std::to_string(index) : original;
    std::string candidate = base;
    int suffix = 2;
    while (ids.contains(candidate)) {
      candidate = base + "-" + std::to_string(suffix++);
    }
    if (candidate != original) {
      call.id = candidate;
      DebugLog("tool_call_id_normalized",
               {{"stream_index", index},
                {"original", original},
                {"normalized", candidate},
                {"reason", original.empty() ? "missing" : "duplicate"}});
    }
    ids.insert(candidate);
  }
  for (auto& [index, call] : streamed) {
    (void)index;
    result.tool_calls.push_back(std::move(call));
  }
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_STREAM_H_
