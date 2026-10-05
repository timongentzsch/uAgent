// Copyright 2026 Timon Gentzsch

#include "include/tools/browser.h"

#include <poll.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "include/browser/browser.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/tools/image_result.h"
#include "include/transport/session.h"

namespace uagent {
namespace {
constexpr int kProbeIntervalMs = 250;
constexpr int kSettleMs = 8000;
// Unchanged probes in a row (about half a second) that count as settled.
constexpr int kQuietProbes = 2;

ToolResult Handover(const std::string& session_id, const std::string& reason,
                    const BrowserAsk& ask) {
  std::string interaction = session::RandomToken(16);
  json outcome = browser::Request({{"op", "request_human"},
                                   {"session_id", session_id},
                                   {"interaction_id", interaction}});
  if (auto error = JsonValue(outcome, "error", ""); !error.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError, error);
  }
  bool eof = false;
  std::string answer = ask(interaction, reason, &eof);
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
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "browser handover remains paused or was cancelled");
  }
  return ToolSuccess(
      "The user finished in the browser and handed it back. Observe the page "
      "before continuing.");
}

// Screenshot, page text and bot-wall evidence of the current tab. `lead`
// states what already happened, so a failed look never hides a done action.
ToolResult Observation(const std::string& session_id, const std::string& lead,
                       const ToolContext& context) {
  const std::string done = lead.empty() ? "" : lead + "\n";
  json outcome =
      browser::Request({{"op", "observe"}, {"session_id", session_id}});
  json current =
      outcome.contains("error")
          ? json::object()
          : browser::Request(
                {{"op", "agent_status"}, {"session_id", session_id}}, 1000);
  std::string failed = JsonValue(outcome, "error", "");
  if (failed.empty() && (!current.value("ok", false) ||
                         JsonValue(current, "generation", uint64_t{0}) !=
                             JsonValue(outcome, "generation", uint64_t{0}) ||
                         JsonValue(current, "session_id", "") != session_id)) {
    failed = "browser control changed; observe again";
  }
  if (failed.empty() && JsonValue(outcome, "image", "").empty()) {
    failed = "empty browser screenshot";
  }
  if (!failed.empty()) {
    if (lead.empty()) {
      return ToolFailure(ToolErrorCode::kRemoteError, failed);
    }
    return ToolSuccess(done + "The page could not be observed (" + failed +
                       "). Do not repeat the action; call observe.");
  }
  ToolResult attached = ToolImageResult(
      {{"data", JsonValue(outcome, "image", "")}, {"mimeType", "image/jpeg"}},
      context.call_id, "browser", kArtifactsDir);
  if (!attached.Ok()) {
    if (lead.empty()) return attached;
    return ToolSuccess(done + "The screenshot could not be saved (" +
                       attached.output + "). Do not repeat the action.");
  }
  const json* page = JsonObject(outcome, "page");
  if (!page) return attached;
  std::string block;
  if (const json* suspected = JsonObject(*page, "block")) {
    block = "SUSPECTED BLOCK (" + JsonValue(*suspected, "kind", "") +
            "): " + JsonValue(*suspected, "evidence", "") +
            ". If you can't proceed, call request_human with a reason, or "
            "use another source.\n";
  }
  attached.output =
      done + block + attached.output + "\nURL: " + JsonValue(*page, "url", "") +
      "\nTitle: " + JsonValue(*page, "title", "") + "\nVisible text:\n" +
      JsonValue(*page, "text", "") +
      "\nView ID: " + JsonValue(outcome, "view_id", "") + " (" +
      std::to_string(JsonValue(outcome, "width", 0)) + "x" +
      std::to_string(JsonValue(outcome, "height", 0)) + " CSS pixels)";
  return attached;
}

// Waits for the page an action changed to settle: loaded, and the same
// document and content on two probes in a row. Each probe is a short request,
// so the human's viewer and takeover are never blocked behind the wait.
// Returns what the wait learned about tabs and time, as lines for the model.
std::string Settle(const std::string& session_id, const ToolContext& context) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kSettleMs);
  std::string previous, notes;
  int quiet = 0;
  json opened = json::array();
  for (;;) {
    json probe =
        browser::Request({{"op", "probe"}, {"session_id", session_id}}, 5000);
    if (auto error = JsonValue(probe, "error", ""); !error.empty()) {
      return notes + "Waiting for the page stopped: " + error + ".\n";
    }
    if (const json* tab = JsonObject(probe, "switched")) {
      notes +=
          "It opened a new tab, now active: " + JsonValue(*tab, "url", "") +
          "\n";
    }
    if (const json* tabs = JsonArray(probe, "opened")) opened = *tabs;
    const std::string state =
        JsonValue(probe, "doc", "") + "|" + JsonValue(probe, "sig", "");
    const bool loaded = JsonValue(probe, "ready", "") == "complete" &&
                        !probe.value("loading", false);
    quiet = loaded && state == previous ? quiet + 1 : 0;
    if (quiet >= kQuietProbes) break;
    previous = state;
    if (std::chrono::steady_clock::now() >= deadline) {
      notes += "The page was still changing after " +
               std::to_string(kSettleMs / 1000) + " s.\n";
      break;
    }
    if (AbortRequested() || context.Expired()) break;
    poll(nullptr, 0, kProbeIntervalMs);
  }
  if (!opened.empty()) {
    notes += "Other new tabs:";
    for (const auto& tab : opened) {
      notes += " " + JsonValue(tab, "id", "") + " " + JsonValue(tab, "url", "");
    }
    notes += " (tabs target_id=… switches)\n";
  }
  return notes;
}
}  // namespace

Tool BrowserTool(std::string session_id, BrowserAsk ask) {
  Tool tool;
  tool.name = "browser";
  tool.description =
      "Use the shared persistent Google Chrome in this web appliance; the "
      "user sees and can drive the same browser. open, click, type (into the "
      "focused field), press, scroll and back wait for the page to settle "
      "and return a fresh screenshot, page text and view_id; coordinates are "
      "CSS pixels of that screenshot, and click/scroll need its view_id. A "
      "tab opened by your action becomes active automatically; tabs lists "
      "tabs and switches with target_id. A result starting with SUSPECTED "
      "BLOCK means a bot check or rate limit: don't hammer it. Chrome holds "
      "the user's saved logins: on a sign-in page click the field and, if "
      "it stays empty, press ArrowDown then Enter to take the saved one, "
      "then submit what Chrome filled. Never type or repeat a password. For "
      "a login Chrome has not saved, MFA, captchas, bot checks or payment "
      "confirmation, call request_human with a reason naming the site and "
      "step; it waits until the user hands back. Never ask for credentials "
      "in chat.";
  tool.parameters = {
      {"type", "object"},
      {"properties",
       {{"action",
         {{"type", "string"},
          {"enum",
           {"status", "open", "tabs", "observe", "click", "type", "press",
            "scroll", "back", "request_human", "release"}}}},
        {"url", {{"type", "string"}}},
        {"target_id", {{"type", "string"}}},
        {"view_id", {{"type", "string"}}},
        {"x", {{"type", "integer"}}},
        {"y", {{"type", "integer"}}},
        {"delta_y", {{"type", "integer"}}},
        {"text", {{"type", "string"}}},
        {"key",
         {{"type", "string"},
          {"enum",
           {"Enter", "Tab", "Escape", "Backspace", "ArrowDown", "ArrowUp"}}}},
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
  tool.intent = "research";
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
                      : action == "back" ? json{"Going back in", "Went back in"}
                      : action == "request_human"
                          ? json{"Asking you to use", "Asked you to use"}
                      : action == "release" ? json{"Releasing", "Released"}
                                            : json{"Checking", "Checked"};
    return json{{"verb", verb}, {"target", "the browser"}};
  };
  tool.run = [session_id = std::move(session_id), ask = std::move(ask)](
                 const json& args, const ToolContext& context) {
    const std::string action = JsonValue(args, "action", "");
    if (action == "request_human") {
      return Handover(session_id,
                      JsonValue(args, "reason", "Please finish in the browser"),
                      ask);
    }
    if (action == "observe") return Observation(session_id, "", context);
    json command = args;
    command["op"] = action == "status" ? "agent_status" : action;
    command["session_id"] = session_id;
    json outcome = browser::Request(command, 30000);
    if (auto error = JsonValue(outcome, "error", ""); !error.empty()) {
      return ToolFailure(ToolErrorCode::kRemoteError, error);
    }
    if (action == "status" || action == "tabs" || action == "release") {
      return ToolSuccess(JsonDump(outcome));
    }
    std::string lead = action + " done.\n" + Settle(session_id, context);
    lead.pop_back();
    return Observation(session_id, lead, context);
  };
  return tool;
}
}  // namespace uagent
