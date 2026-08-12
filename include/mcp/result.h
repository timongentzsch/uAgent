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
#include "include/core/strings.h"
#include "include/mcp/server.h"
#include "include/media.h"
#include "include/tools/files.h"
#include "include/tools/tool.h"

namespace uagent {

inline ToolResult McpImageResult(const json& content) {
  if (!content.contains("data") || !content["data"].is_string()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: MCP image is missing base64 data");
  }
  std::string mime = JsonValue(content, "mimeType", "image/png");
  std::string extension = ImageExtension(mime);
  if (extension.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: unsupported MCP image type " + mime);
  }
  int64_t limit_mb = TerminalImageLimitMb();
  std::string bytes;
  if (!Base64Decode(content["data"].get_ref<const std::string&>(), bytes,
                    static_cast<size_t>(limit_mb) * 1024 * 1024)) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: MCP image is invalid or exceeds " +
                           std::to_string(limit_mb) + " MB");
  }
  static std::atomic<uint64_t> sequence{0};
  std::string path =
      UagentDir(kMcpDir) + "/image-" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
      std::to_string(getpid()) + "-" + std::to_string(sequence++) + extension;
  ToolResult saved = ToolWritePrivateFile(path, bytes);
  if (!saved.Ok()) return saved;
  // A tool result is text-only, so the image cannot travel back inside it.
  // Queue it instead: it rides in on the next request and the model can
  // actually look at it. Printing it to the terminal was never what made it
  // readable — that only showed it to the human, on every single call.
  ToolResult attached = Attachments().Add(path);
  if (!attached.Ok()) {
    std::string reason = std::move(attached.output);
    constexpr std::string_view kErrorPrefix = "error: ";
    if (reason.starts_with(kErrorPrefix)) reason.erase(0, kErrorPrefix.size());
    return ToolSuccess("[mcp image saved: " + path +
                       "; not attached: " + reason + "]");
  }
  return ToolSuccess(
      "[mcp image saved: " + path +
      "; attached — readable in your next step. Use show_image to put it on "
      "the user's terminal]");
}

// tools/call response -> bounded model-readable text. Binary images are saved
// and queued separately; their base64 never enters the tool result history.
inline ToolResult McpResultText(const McpServer& s, const json& resp) {
  if (!resp.contains("result")) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: mcp(" + s.name +
                           "): " + JsonErrorMessage(resp, "unknown error"));
  }
  const json& r = resp["result"];
  if (!r.is_object()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: mcp(" + s.name + "): result must be an object");
  }
  std::string text;
  ToolErrorCode local_error = ToolErrorCode::kNone;
  if (r.contains("content") && r["content"].is_array()) {
    for (const json& c : r["content"]) {
      if (!text.empty()) text += '\n';
      std::string type = JsonValue(c, "type", "");
      if (type == "text" && c.contains("text") && c["text"].is_string()) {
        text += c["text"].get<std::string>();
      } else if (type == "image") {
        ToolResult image = McpImageResult(c);
        text += image.output;
        if (!image.Ok() && local_error == ToolErrorCode::kNone) {
          local_error = image.error;
        }
      } else {
        text +=
            "[mcp " + (type.empty() ? "content" : type) + "]\n" + JsonDump(c);
      }
    }
  }
  if (r.contains("structuredContent")) {
    if (!text.empty()) text += '\n';
    text += "[mcp structuredContent]\n" + JsonDump(r["structuredContent"]);
  }
  bool is_error = r.contains("isError") && r["isError"].is_boolean() &&
                  r["isError"].get<bool>();
  if (text.empty()) {
    text = is_error ? "error: mcp(" + s.name +
                          ") returned isError without diagnostic text"
                    : "(empty result)";
  }
  if (is_error && !text.starts_with("error")) text = "error: " + text;
  if (is_error) {
    return ToolFailure(ToolErrorCode::kRemoteError, std::move(text));
  }
  if (local_error != ToolErrorCode::kNone) {
    return ToolFailure(local_error, std::move(text));
  }
  return ToolSuccess(std::move(text));
}

// cap without splitting a UTF-8 codepoint
inline std::string McpCapDesc(std::string d) {
  int64_t cap = McpDescriptionChars();
  return cap > 0 ? Utf8Trunc(std::move(d), static_cast<size_t>(cap)) : d;
}

// <server>_<tool>, restricted to the [A-Za-z0-9_-]{1,64} function-name charset
inline std::string McpToolName(const std::string& server,
                               const std::string& tool) {
  return SanitizeComponent(server + "_" + tool, 64);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_RESULT_H_
