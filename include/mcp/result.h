// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_RESULT_H_
#define UAGENT_INCLUDE_MCP_RESULT_H_
// Turning MCP results into what the model and terminal see: text, saved
// images, and bounded tool names and descriptions.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/mcp/server.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "include/tools/image_result.h"
#include "include/tools/tool.h"

namespace uagent {

// tools/call response -> bounded text; image base64 never enters the history.
inline ToolResult McpResultText(const McpServer& s, const json& resp,
                                const std::string& source_call_id = {}) {
  if (!resp.contains("result")) {
    return ToolFailure(
        ToolErrorCode::kRemoteError,
        "mcp(" + s.name + "): " + JsonErrorMessage(resp, "unknown error"));
  }
  const json& r = resp["result"];
  if (!r.is_object()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "mcp(" + s.name + "): result must be an object");
  }
  if (JsonValue(r, "resultType", "complete") != "complete") {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "MCP request requires unsupported input or "
                       "exceeded continuation limits");
  }
  std::string text;
  std::string hint;  // where saved images went, for the model
  ToolErrorCode local_error = ToolErrorCode::kNone;
  if (r.contains("content") && r["content"].is_array()) {
    for (const json& c : r["content"]) {
      if (!text.empty()) text += '\n';
      std::string type = JsonValue(c, "type", "");
      if (type == "text" && c.contains("text") && c["text"].is_string()) {
        text += c["text"].get<std::string>();
      } else if (type == "image") {
        ToolResult image = ToolImageResult(c, source_call_id, "mcp", kMcpDir);
        text += image.output;
        if (!image.hint.empty()) {
          hint += (hint.empty() ? "" : "\n") + image.hint;
        }
        if (!image.Ok() && local_error == ToolErrorCode::kNone) {
          local_error = image.error;
        }
      } else {
        text +=
            "[mcp " + (type.empty() ? "content" : type) + "]\n" + JsonDump(c);
      }
    }
  }
  json parts = nullptr;
  if (r.contains("structuredContent")) {
    const json& structured = r["structuredContent"];
    const std::string encoded = JsonDump(structured);
    // MCP servers can also echo structuredContent in a text block. Display
    // the typed result once, while retaining both representations below.
    if (text.size() + encoded.size() <= kPreviewChars) {
      parts = GenericInputParts(structured);
      if (!parts.empty() && !text.empty()) {
        const json echo = json::parse(text, nullptr, false);
        if (echo.is_discarded() || echo != structured) {
          parts.insert(parts.begin(), CodePart(text, "", ""));
        }
      }
    }
    if (!text.empty()) text += '\n';
    text += "[mcp structuredContent]\n" + encoded;
  }
  bool is_error = r.contains("isError") && r["isError"].is_boolean() &&
                  r["isError"].get<bool>();
  if (text.empty()) {
    text = is_error ? "error: mcp(" + s.name +
                          ") returned isError without diagnostic text" +
                          McpStderrHint(s.name)
                    : "(empty result)";
  }
  if (is_error && !text.starts_with("error")) {
    text = std::string(kToolErrorPrefix) + text;
  }
  ToolResult result =
      is_error ? ToolFailure(ToolErrorCode::kRemoteError, std::move(text))
      : local_error != ToolErrorCode::kNone
          ? ToolFailure(local_error, std::move(text))
          : ToolSuccess(std::move(text));
  if (result.Ok() && parts.is_array() && !parts.empty()) {
    result.parts = std::move(parts);
    result.body_kind = "fields";
  }
  result.hint = std::move(hint);
  return result;
}

inline std::string McpCapDesc(std::string d) {
  return Utf8Trunc(std::move(d), kMcpDescriptionChars);
}

// <server>_<tool>, restricted to the [A-Za-z0-9_-]{1,64} function-name charset
inline std::string McpToolName(const std::string& server,
                               const std::string& tool) {
  return SanitizeComponent(server + "_" + tool, 64);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_RESULT_H_
