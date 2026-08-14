// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_STREAM_H_
#define UAGENT_INCLUDE_API_STREAM_H_
// Streaming transport state. Most API consumers should include api.h instead.

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "include/agent/tool_protocol.h"
#include "include/api/openai_stream.h"
#include "include/api/types.h"
#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/transport/sse.h"

namespace uagent {

// Incremental SSE parser; prints content and muted italic reasoning as it
// streams.
struct StreamCtx {
  CURL* handle = nullptr;
  ChatResult* res = nullptr;
  std::string error_body;  // body when HTTP status >= 400
  int64_t status = 0;
  bool in_reasoning = false;
  bool content_started = false;
  std::map<int, ToolCall> calls;  // keyed by stream index
  TerminalSpinner* spinner = nullptr;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point last_byte;
  int64_t first_event_timeout_s = 300;
  int64_t idle_timeout_s = 300;
  size_t response_cap = 32 * 1024 * 1024;
  size_t received = 0;
  std::string timeout_reason;
  bool render_output = true;
  bool full_reasoning = true;
  std::string reasoning_preview;
  SseParser sse;
  MdStream md;  // renders streamed content as ANSI-styled markdown (TTY only)

  // Hold content back while it could still be a text-protocol tool call, so
  // raw [uagent_tool_call] blocks never flash on screen. UNDECIDED until the
  // first non-whitespace bytes either match TT_OPEN (SUPPRESS) or don't
  // (PRINT).
  enum class Show { kUndecided, kPrint, kSuppress } show = Show::kUndecided;

  void MarkEvent() {
    res->semantic_progress = true;
    if (res->first_event_ms < 0) res->first_event_ms = ElapsedMs(started);
  }

  void BeginOutput() {  // stop the spinner before any visible bytes
    if (render_output && spinner) spinner->Stop();
  }

  void OutputText(const std::string& c) {
    if (!render_output) return;
    BeginOutput();
    if (in_reasoning) {
      md.Control(RST());
      md.FeedPlain("\n");
      in_reasoning = false;
    }
    if (!content_started) {
      md.Control(RST());
      content_started = true;
    }
    md.Feed(c);
  }

  void OutputReasoning(const std::string& r) {
    if (!render_output) return;
    if (!full_reasoning) {
      reasoning_preview += r;
      size_t line = reasoning_preview.find_last_of("\r\n");
      if (line != std::string::npos && line + 1 < reasoning_preview.size()) {
        reasoning_preview.erase(0, line + 1);
      }
      std::string preview = OneLine(reasoning_preview);
      constexpr size_t kPreviewBytes = 120;
      if (preview.size() > kPreviewBytes) {
        size_t start = preview.size() - kPreviewBytes;
        while (start < preview.size() &&
               (static_cast<unsigned char>(preview[start]) & 0xc0) == 0x80) {
          ++start;
        }
        preview = "…" + preview.substr(start);
      }
      if (spinner && !preview.empty()) {
        spinner->SetLabel("thinking · " + TerminalSafe(preview));
      }
      if (reasoning_preview.size() > 512) {
        reasoning_preview.erase(0, reasoning_preview.size() - 512);
      }
      return;
    }
    BeginOutput();
    if (!in_reasoning) {
      std::string style = std::string(RST()) + MUTED() + ITAL();
      md.Control(style.c_str());
    }
    in_reasoning = true;
    md.FeedPlain(r);
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
    std::string vis = full.substr(start);
    size_t tl = strlen(kTtOpen);
    if (vis.size() >= tl) {
      show = vis.compare(0, tl, kTtOpen) == 0 ? Show::kSuppress : Show::kPrint;
      if (show == Show::kPrint) {
        OutputText(full);
      } else {
        res->suppressed = true;
      }
    } else if (std::string(kTtOpen).compare(0, vis.size(), vis) != 0) {
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
    (void)index;
    if (call.id.empty()) {
      result.error = "invalid model tool call: missing id";
      return false;
    }
    if (!ids.insert(call.id).second) {
      result.error = "invalid model tool call: duplicate id";
      return false;
    }
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
