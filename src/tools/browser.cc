// Copyright 2026 Timon Gentzsch

#include "include/tools/browser.h"

#include <poll.h>

#include <cstdint>
#include <string>
#include <utility>

#include "include/app/session.h"
#include "include/browser/browser.h"
#include "include/cli.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/tools/image_result.h"

namespace uagent {
namespace {
constexpr int kBrowserRetryMs = 250;
constexpr int kBrowserWaitMs = 120000;

ToolResult Handover(const std::string& session_id, const std::string& reason) {
  std::string interaction = session::RandomToken(16);
  json outcome = browser::Request({{"op", "request_human"},
                                   {"session_id", session_id},
                                   {"interaction_id", interaction}});
  if (auto error = JsonValue(outcome, "error", ""); !error.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError, "error: " + error);
  }
  bool eof = false;
  std::string answer = ReadInteraction(
      {.id = interaction,
       .kind = "browser",
       .prompt =
           reason + ". Open the browser and choose Hand back when finished."},
      &eof);
  json status;
  for (int attempt = 0; attempt < 250; ++attempt) {
    status = browser::Request({{"op", "status"}});
    if (JsonValue(status, "mode", "") != "human") break;
    poll(nullptr, 0, 20);
  }
  if (eof || answer != "done" || JsonValue(status, "mode", "") != "agent" ||
      JsonValue(status, "session_id", "") != session_id) {
    browser::Request({{"op", "cancel_handover"},
                      {"session_id", session_id},
                      {"interaction_id", interaction}});
    return ToolFailure(
        ToolErrorCode::kRemoteError,
        "error: browser handover remains paused or was cancelled");
  }
  return ToolSuccess(
      "Human finished in the browser. Observe the page before continuing.");
}
}  // namespace

Tool BrowserTool(std::string session_id) {
  Tool tool;
  tool.name = "browser";
  tool.description =
      "Use the shared persistent Google Chrome in this web appliance. "
      "Open HTTP(S) pages, list or select tabs, observe screenshots, click, "
      "type into the focused field, press keys or scroll. Coordinates use "
      "the CSS-pixel viewport dimensions and view_id from the latest observe. "
      "The human controls the same browser. Never request credentials in "
      "chat; use request_human for login or MFA.";
  tool.parameters = {{"type", "object"},
                     {"properties",
                      {{"action",
                        {{"type", "string"},
                         {"enum",
                          {"status", "open", "tabs", "observe", "click", "type",
                           "press", "scroll", "request_human", "release"}}}},
                       {"url", {{"type", "string"}}},
                       {"target_id", {{"type", "string"}}},
                       {"view_id", {{"type", "string"}}},
                       {"x", {{"type", "integer"}}},
                       {"y", {{"type", "integer"}}},
                       {"delta_y", {{"type", "integer"}}},
                       {"text", {{"type", "string"}}},
                       {"key", {{"type", "string"}}},
                       {"reason", {{"type", "string"}}}}},
                     {"required", {"action"}},
                     {"additionalProperties", false}};
  tool.mutates = [](const json& args) {
    std::string action = JsonValue(args, "action", "");
    if (action == "tabs") return !JsonValue(args, "target_id", "").empty();
    return action != "status" && action != "observe" &&
           action != "request_human" && action != "release";
  };
  tool.needs_approval = tool.mutates;
  tool.capabilities = Capability(ToolCapability::kInspect) |
                      Capability(ToolCapability::kMutate) |
                      Capability(ToolCapability::kExternal);
  tool.summary = [](const json& args) {
    auto action = JsonValue(args, "action", "browser");
    return action == "open" ? "browser open " + JsonValue(args, "url", "")
                            : "browser " + action;
  };
  tool.header = [](const json& args) {
    const std::string action = JsonValue(args, "action", "");
    if (action == "open") {
      return json{{"verb", {"Opening", "Opened"}},
                  {"target", JsonValue(args, "url", "")}};
    }
    if (action == "type") {
      return json{{"verb", {"Typing", "Typed"}},
                  {"target", FirstLine(JsonValue(args, "text", ""))}};
    }
    const json verb = action == "observe" ? json{"Looking at", "Looked at"}
                      : action == "click" ? json{"Clicking in", "Clicked in"}
                      : action == "press"
                          ? json{"Pressing a key in", "Pressed a key in"}
                      : action == "scroll" ? json{"Scrolling", "Scrolled"}
                      : action == "request_human"
                          ? json{"Asking you to use", "Asked you to use"}
                      : action == "release" ? json{"Releasing", "Released"}
                                            : json{"Checking", "Checked"};
    return json{{"verb", verb}, {"target", "the browser"}};
  };
  tool.run = [session_id = std::move(session_id)](const json& args,
                                                  const ToolContext& context) {
    const std::string action = JsonValue(args, "action", "");
    json command = args;
    command["op"] = action == "status" ? "agent_status" : action;
    command["session_id"] = session_id;
    if (action == "request_human") {
      return Handover(session_id, JsonValue(args, "reason",
                                            "Please finish in the browser"));
    }
    // While the human drives, wait for them to hand back or close the
    // viewer; each retry tells their viewer that the agent is waiting.
    json outcome = browser::Request(command, 30000);
    for (int waited = 0; JsonValue(outcome, "error", "") ==
                         "human controls the browser; wait for Done";
         waited += kBrowserRetryMs) {
      if (waited >= kBrowserWaitMs || AbortRequested() || context.Expired()) {
        return ToolFailure(
            ToolErrorCode::kRemoteError,
            "error: the user is still using the browser; try again later");
      }
      poll(nullptr, 0, kBrowserRetryMs);
      outcome = browser::Request(command, 30000);
    }
    if (auto error = JsonValue(outcome, "error", ""); !error.empty()) {
      return ToolFailure(ToolErrorCode::kRemoteError, "error: " + error);
    }
    if (action == "observe") {
      json current = browser::Request(
          {{"op", "agent_status"}, {"session_id", session_id}}, 1000);
      if (!current.value("ok", false) ||
          JsonValue(current, "generation", uint64_t{0}) !=
              JsonValue(outcome, "generation", uint64_t{0}) ||
          JsonValue(current, "session_id", "") != session_id) {
        return ToolFailure(ToolErrorCode::kRemoteError,
                           "error: browser control changed; observe again");
      }
      std::string image = JsonValue(outcome, "image", "");
      if (image.empty()) {
        return ToolFailure(ToolErrorCode::kRemoteError,
                           "error: empty browser screenshot");
      }
      ToolResult attached =
          ToolImageResult({{"data", image}, {"mimeType", "image/jpeg"}},
                          context.call_id, "browser", kArtifactsDir);
      if (attached.Ok()) {
        if (const json* page = JsonObject(outcome, "page")) {
          attached.output +=
              "\nURL: " + JsonValue(*page, "url", "") +
              "\nTitle: " + JsonValue(*page, "title", "") +
              "\nVisible text:\n" + JsonValue(*page, "text", "") +
              "\nView ID: " + JsonValue(outcome, "view_id", "") + " (" +
              std::to_string(JsonValue(outcome, "width", 0)) + "x" +
              std::to_string(JsonValue(outcome, "height", 0)) + " CSS pixels)";
        }
      }
      return attached;
    }
    if (action == "status" || action == "tabs") {
      return ToolSuccess(JsonDump(outcome));
    }
    return ToolSuccess(action + " completed. Use observe to inspect the page.");
  };
  return tool;
}
}  // namespace uagent
