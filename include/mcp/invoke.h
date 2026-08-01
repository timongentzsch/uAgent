// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_INVOKE_H_
#define UAGENT_INCLUDE_MCP_INVOKE_H_
// One bounded MCP invocation path, including lean Chrome composition that
// keeps navigation and screenshot observation inside the same model step.

#include <filesystem>
#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/media.h"
#include "include/tools/tool.h"

namespace uagent {

inline json McpCallArguments(const json& arguments,
                             bool include_snapshot_by_default) {
  json normalized = arguments;
  if (include_snapshot_by_default && normalized.is_object() &&
      !normalized.contains("includeSnapshot")) {
    normalized["includeSnapshot"] = true;
  }
  return normalized;
}

inline ToolResult McpInvokeRemote(McpServer& server,
                                  const std::string& remote_name,
                                  const json& arguments, int64_t timeout,
                                  const ToolContext& context,
                                  bool include_snapshot) {
  if (!server.alive) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: mcp server " + server.name +
                           " has exited (stderr: " + McpLogPath(server.name) +
                           ")");
  }
  json response;
  if (RunCancellable([&] {
        response = McpRpc(
            server, "tools/call",
            {{"name", remote_name},
             {"arguments", McpCallArguments(arguments, include_snapshot)}},
            context.RemainingSeconds(timeout), true);
      })) {
    return ToolCancelled("error: call cancelled by user");
  }
  return McpResultText(server, response);
}

inline bool ChromeSlimTool(const McpServer& server,
                           const std::string& remote_name) {
  return JsonValue(server.config, "__uagent_builtin", "") == kChromeMcpName &&
         JsonValue(server.config, "__uagent_toolset", "slim") == "slim" &&
         (remote_name == "navigate" || remote_name == "screenshot");
}

inline constexpr const char* kChromePageStateScript = R"js((() => {
  const clean = (value, limit) => String(value ?? "").replace(/\s+/g, " ").trim().slice(0, limit);
  const selector = 'a,button,input,select,textarea,[role="button"],[role="link"],[contenteditable="true"]';
  const controls = [...document.querySelectorAll(selector)].slice(0, 40).map((element, index) => ({
    index,
    tag: element.tagName.toLowerCase(),
    id: element.id || undefined,
    name: element.getAttribute("name") || undefined,
    role: element.getAttribute("role") || undefined,
    label: clean(element.getAttribute("aria-label") || element.innerText || element.getAttribute("placeholder"), 120),
    href: element.href || undefined,
    disabled: Boolean(element.disabled)
  }));
  return {
    url: location.href,
    title: document.title,
    text: clean(document.body?.innerText, 3000),
    controls
  };
})())js";

inline ToolResult AttachChromeScreenshot(ToolResult result) {
  if (result.output.find('\n') != std::string::npos) return result;
  std::string path = Trim(result.output);
  std::string extension =
      AsciiLower(std::filesystem::path(path).extension().string());
  if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
      extension != ".webp" && extension != ".gif") {
    return result;
  }
  ToolResult attached = Attachments().Add(path);
  if (!attached.Ok()) {
    return ToolFailure(attached.error,
                       result.output + "\n" + std::move(attached.output));
  }
  result.output += "\n" + attached.output;
  return result;
}

inline ToolResult McpInvokeTool(McpServer& server,
                                const std::string& remote_name,
                                const json& arguments, int64_t timeout,
                                const ToolContext& context,
                                bool include_snapshot) {
  ToolResult result = McpInvokeRemote(server, remote_name, arguments, timeout,
                                      context, include_snapshot);
  if (!result.Ok() || !ChromeSlimTool(server, remote_name)) return result;

  if (remote_name == "navigate") {
    if (include_snapshot) return result;
    ToolResult state =
        McpInvokeRemote(server, "evaluate",
                        {{"script", kChromePageStateScript}}, timeout, context,
                        /*include_snapshot=*/false);
    if (state.Ok()) {
      result.output +=
          "\n[page state; controls use the listed DOM index]\n" + state.output;
    } else {
      result.output += "\n[page state unavailable: " + state.output + "]";
    }
    return result;
  }

  return AttachChromeScreenshot(std::move(result));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_INVOKE_H_
