// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_RESULT_H_
#define UAGENT_INCLUDE_MCP_RESULT_H_
// Turning MCP results into what the model and terminal see: text, saved
// images, and bounded tool names and descriptions.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/server.h"
#include "include/media.h"
#include "include/tools/files.h"

namespace uagent {

inline std::string McpImageResult(const json& content) {
  if (!content.contains("data") || !content["data"].is_string()) {
    return "error: MCP image is missing base64 data";
  }
  std::string mime = JsonValue(content, "mimeType", "image/png");
  std::string extension = ImageExtension(mime);
  if (extension.empty()) return "error: unsupported MCP image type " + mime;
  int64_t limit_mb =
      std::max(int64_t{1}, EnvLong("UAGENT_TERMINAL_IMAGE_MB", 10));
  std::string bytes;
  if (!Base64Decode(content["data"].get_ref<const std::string&>(), bytes,
                    static_cast<size_t>(limit_mb) * 1024 * 1024)) {
    return "error: MCP image is invalid or exceeds " +
           std::to_string(limit_mb) + " MB";
  }
  static std::atomic<uint64_t> sequence{0};
  std::string path = UagentDir("mcp") + "/image-" + UtcStamp("%Y%m%dT%H%M%SZ") +
                     "-" + std::to_string(getpid()) + "-" +
                     std::to_string(sequence++) + extension;
  std::string saved = ToolWritePrivateFile(path, bytes);
  if (saved.starts_with("error:")) return saved;
  std::string status = "[mcp image saved: " + path;
  if (g_tty && DetectTerminalImageProtocol() != TerminalImageProtocol::kNone) {
    std::string displayed = ToolShowImage(path);
    status +=
        displayed.starts_with("error:")
            ? "; display failed: " + displayed.substr(7)
            : "; displayed inline via " + std::string(TerminalImageProtocolName(
                                              DetectTerminalImageProtocol()));
  } else {
    status += "; use show_image in a supported terminal";
  }
  return status + "]";
}

// tools/call response -> bounded model-readable text. Binary images are saved
// and rendered locally; their base64 never enters model history.
inline std::string McpResultText(const McpServer& s, const json& resp) {
  if (!resp.contains("result")) {
    std::string msg = "unknown error";
    if (resp.contains("error") && resp["error"].is_object()) {
      msg = JsonValue(resp["error"], "message", msg);
    }
    return "error: mcp(" + s.name + "): " + msg;
  }
  const json& r = resp["result"];
  if (!r.is_object()) {
    return "error: mcp(" + s.name + "): result must be an object";
  }
  std::string text;
  if (r.contains("content") && r["content"].is_array()) {
    for (const json& c : r["content"]) {
      if (!text.empty()) text += '\n';
      std::string type;
      if (c.is_object() && c.contains("type") && c["type"].is_string()) {
        type = c["type"].get<std::string>();
      }
      if (type == "text" && c.is_object() && c.contains("text") &&
          c["text"].is_string()) {
        text += c["text"].get<std::string>();
      } else if (type == "image" && c.is_object()) {
        text += McpImageResult(c);
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
  if (text.empty()) text = "(empty result)";
  bool is_error = r.contains("isError") && r["isError"].is_boolean() &&
                  r["isError"].get<bool>();
  if (is_error && !text.starts_with("error")) text = "error: " + text;
  return text;
}

// cap without splitting a UTF-8 codepoint
inline std::string McpCapDesc(std::string d) {
  int64_t cap = EnvLong("UAGENT_MCP_DESC_CHARS", 400);
  return cap > 0 ? Utf8Trunc(std::move(d), static_cast<size_t>(cap)) : d;
}

// <server>_<tool>, restricted to the [A-Za-z0-9_-]{1,64} function-name charset
inline std::string McpToolName(const std::string& server,
                               const std::string& tool) {
  std::string n = server + "_" + tool;
  for (auto& c : n) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
      c = '_';
    }
  }
  if (n.size() > 64) n.resize(64);
  return n;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_RESULT_H_
