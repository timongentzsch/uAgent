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

#include "include/agent/tool_protocol.h"
#include "include/api/openai_stream.h"
#include "include/api/types.h"
#include "include/core/checked.h"
#include "include/core/events.h"
#include "include/core/strings.h"
#include "include/transport/sse.h"

namespace uagent {

// Incremental SSE parser; emits provider-independent reasoning and answer
// streams.
struct StreamCtx {
  CURL* handle = nullptr;
  ChatResult* res = nullptr;
  std::string error_body;  // body when HTTP status >= 400
  int64_t status = 0;
  std::map<int, ToolCall> calls;  // keyed by stream index
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point last_byte;
  int64_t first_event_timeout_s = 300;
  int64_t idle_timeout_s = 300;
  size_t response_cap = 32 * 1024 * 1024;
  size_t received = 0;
  std::string timeout_reason;
  SseParser sse;

  // Hold content back while it could still be a text-protocol tool call, so
  // raw [uagent_tool_call] blocks never flash on screen. UNDECIDED until the
  // first non-whitespace bytes either match TT_OPEN (SUPPRESS) or don't
  // (PRINT).
  enum class Show { kUndecided, kPrint, kSuppress } show = Show::kUndecided;

  void MarkEvent() {
    res->semantic_progress = true;
    if (res->first_event_ms < 0) res->first_event_ms = ElapsedMs(started);
  }

  void OutputText(const std::string& value) {
    Event event{EventId::kAnswerDelta};
    event.text = value;
    Emit(std::move(event));
  }

  void OutputReasoning(const std::string& value) {
    Event event{EventId::kReasoningDelta};
    event.text = value;
    Emit(std::move(event));
  }

  void EmitContent(const std::string& c) {
    res->content += c;
    if (show == Show::kPrint) {
      OutputText(c);
      return;
    }
    if (show == Show::kSuppress) return;
    const std::string& full = res->content;
    size_t start = full.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return;  // only whitespace so far
    std::string_view vis(full.data() + start, full.size() - start);
    std::string_view open(kTtOpen);
    if (vis.size() >= open.size()) {
      show = vis.starts_with(open) ? Show::kSuppress : Show::kPrint;
      if (show == Show::kPrint) {
        OutputText(full);
      } else {
        res->suppressed = true;
      }
    } else if (!open.starts_with(vis)) {
      show = Show::kPrint;
      OutputText(full);
    }  // else: still a prefix of the tag — keep holding
  }

  // This runs inside a libcurl callback, so malformed server JSON is validated
  // explicitly and never crosses the C boundary.
  void HandleEvent(const SseEvent& event) {
    OpenAiStreamDelta delta = DecodeOpenAiStreamEvent(event.data, *res, calls);
    if (delta.activity) MarkEvent();
    if (!delta.reasoning.empty()) {
      res->reasoning += delta.reasoning;
      OutputReasoning(delta.reasoning);
    }
    if (!delta.content.empty()) EmitContent(delta.content);
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

  void Finish() { Drain(sse.Finish()); }

  // Surface a parser failure, else hand every completed event to the decoder.
  bool Drain(bool parsed) {
    if (!parsed) {
      res->error = sse.Error();
      return false;
    }
    for (const SseEvent& event : sse.TakeEvents()) HandleEvent(event);
    return true;
  }
};

inline bool CollectToolCalls(std::map<int, ToolCall>& streamed,
                             ChatResult& result) {
  std::set<std::string> ids;
  for (auto& [index, call] : streamed) {
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
    json arguments = json::parse(call.args, nullptr, false);
    if (call.name.empty() || arguments.is_discarded() ||
        !arguments.is_object()) {
      result.error = "invalid model tool call: incomplete function";
      return false;
    }
  }
  for (auto& [index, call] : streamed) {
    (void)index;
    result.tool_calls.push_back(std::move(call));
  }
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_STREAM_H_
