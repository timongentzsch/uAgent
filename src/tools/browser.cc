// Copyright 2026 Timon Gentzsch

#include "include/tools/browser.h"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/browser/browser.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/tools/image_result.h"
#include "include/transport/session.h"

namespace uagent {
namespace {
// What the tool can do. An action that acts on the page changes it and is
// followed by a look at what came of it; the others ask or hand over.
struct Action {
  const char* name;
  bool acts;
  const char* doing;
  const char* done;
};
constexpr Action kActions[] = {
    {"status", false, "Checking", "Checked"},
    {"open", true, "Opening", "Opened"},
    {"tabs", false, "Checking", "Checked"},
    {"observe", false, "Looking at", "Looked at"},
    {"read", false, "Reading", "Read"},
    {"click", true, "Clicking in", "Clicked in"},
    {"type", true, "Typing", "Typed"},
    {"press", true, "Pressing a key in", "Pressed a key in"},
    {"fill_saved", true, "Taking a saved login in", "Took a saved login in"},
    {"scroll", true, "Scrolling", "Scrolled"},
    {"back", true, "Going back in", "Went back in"},
    {"request_human", false, "Asking you to use", "Asked you to use"},
    {"release", false, "Releasing", "Released"}};

const Action* FindAction(const json& args) {
  const std::string name = JsonValue(args, "action", "");
  const auto* found = std::ranges::find(kActions, name, &Action::name);
  return found == std::end(kActions) ? nullptr : found;
}

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

// How much of a page's text one result carries; kPageScript cuts there.
constexpr int64_t kPageTextChars = 12000;

// What a result says first when the page looks like a bot wall.
std::string BlockLine(const json& page) {
  const json* suspected = JsonObject(page, "block");
  if (!suspected) return "";
  return "SUSPECTED BLOCK (" + JsonValue(*suspected, "kind", "") +
         "): " + JsonValue(*suspected, "evidence", "") +
         ". If you can't proceed, call request_human with a reason, or use "
         "another source.\n";
}

// One request to the browser on behalf of this conversation.
json Ask(const std::string& session_id, json command, const std::string& op) {
  command["op"] = op;
  command["session_id"] = session_id;
  return browser::Request(command, 30000);
}

// What read answers: the text asked for, and the links that go with it.
// `done` states what happened before, as in Observation.
ToolResult Reading(const json& read, const json& args,
                   const std::string& done) {
  const int offset = std::max(JsonValue(args, "offset", 0), 0);
  const std::string text = JsonValue(read, "text", "");
  const std::string links = JsonValue(read, "links", "");
  const std::string what =
      read.contains("matches")
          ? "Lines containing it (" +
                std::to_string(JsonValue(read, "matches", 0)) + " in all" +
                (offset ? ", from match " + std::to_string(offset) : "") + ")"
          : "Page text (from character " + std::to_string(offset) + " of " +
                std::to_string(JsonValue(read, "chars", 0)) + ")";
  return ToolSuccess(
      done + BlockLine(read) + "URL: " + JsonValue(read, "url", "") +
      "\nTitle: " + JsonValue(read, "title", "") + "\n" + what + ":\n" + text +
      (links.empty() ? "" : "\nLinks:\n" + links));
}

// The current tab as text: what can be acted on, numbered, the page's text
// and bot-wall evidence; with `look`, the screenshot too. `lead` states what
// already happened, so a failed look never hides a done action.
ToolResult Observation(const std::string& session_id, const std::string& lead,
                       const ToolContext& context, bool look) {
  const std::string done = lead.empty() ? "" : lead + "\n";
  json outcome = browser::Request(
      {{"op", "observe"}, {"session_id", session_id}, {"image", look}});
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
  if (failed.empty() && look && JsonValue(outcome, "image", "").empty()) {
    failed = "empty browser screenshot";
  }
  if (!failed.empty()) {
    if (lead.empty()) {
      return ToolFailure(ToolErrorCode::kRemoteError, failed);
    }
    return ToolSuccess(done + "The page could not be observed (" + failed +
                       "). Do not repeat the action; call observe.");
  }
  ToolResult attached =
      look ? ToolImageResult({{"data", JsonValue(outcome, "image", "")},
                              {"mimeType", "image/jpeg"}},
                             context.call_id, "browser", kArtifactsDir)
           : ToolSuccess("");
  if (!attached.Ok()) {
    if (lead.empty()) return attached;
    return ToolSuccess(done + "The screenshot could not be saved (" +
                       attached.output + "). Do not repeat the action.");
  }
  const json* page = JsonObject(outcome, "page");
  if (!page) return attached;
  const std::string text = JsonValue(*page, "text", "");
  const int64_t chars = JsonValue(*page, "chars", int64_t{0});
  const std::string elements = JsonValue(*page, "elements", "");
  const std::string more =
      chars > kPageTextChars
          ? " (the first " + std::to_string(kPageTextChars) + " of " +
                std::to_string(chars) + " characters; read with offset=" +
                std::to_string(kPageTextChars) + " continues)"
          : "";
  // A canvas, a chart or an image says nothing here: the picture does.
  const std::string sparse =
      !look && elements.empty() && text.size() < 200
          ? "\nThis page shows little as text; observe returns a screenshot."
          : "";
  attached.output = done + BlockLine(*page) + attached.output +
                    (look ? "\n" : "") + "URL: " + JsonValue(*page, "url", "") +
                    "\nTitle: " + JsonValue(*page, "title", "") +
                    (elements.empty() ? "" : "\nElements:\n" + elements) +
                    "\nPage text" + more + ":\n" + text + sparse +
                    "\nView ID: " + JsonValue(outcome, "view_id", "") + " (" +
                    std::to_string(JsonValue(outcome, "width", 0)) + "x" +
                    std::to_string(JsonValue(outcome, "height", 0)) +
                    " CSS pixels)";
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
      "and return it as text: its numbered elements, its text and a view_id. "
      "click takes element (a number from the latest result) and that "
      "view_id. Fill a field in one call: type with element and view_id "
      "clicks it first, replace=true overwrites what it holds, and key is "
      "pressed after. read returns more: page text from offset (in "
      "characters), or with find the lines and links that contain it, "
      "whatever their case, 200 lines at a time from match offset. open "
      "takes the same offset or find and then answers as read does, for a "
      "page you only need to read; several such calls in one step read "
      "several pages. observe adds a screenshot, for a page that text does "
      "not describe; where no element number fits, click takes x and y in "
      "its CSS pixels, and scroll takes them with delta_y, each with the "
      "view_id. A tab opened by your action becomes active automatically; "
      "open with new_tab=true keeps the current page; tabs lists tabs, "
      "switches to target_id, and with close=true closes it instead, unless "
      "it is the current one. A result that says SUSPECTED BLOCK means a bot "
      "check or rate limit: don't hammer it. Chrome holds the user's saved "
      "logins: on a sign-in page click the field, call fill_saved to take "
      "the one Chrome offers for it, then submit what Chrome filled. Where "
      "the user has several accounts, type the start of the wanted "
      "account's name into the field first: Chrome then offers the ones "
      "that match. Never type or repeat a password. For a login Chrome has "
      "not saved, MFA, captchas, bot checks or payment confirmation, call "
      "request_human with a reason naming the site and step; it waits until "
      "the user hands back. Never ask for credentials in chat.";
  json names = json::array();
  for (const Action& action : kActions) names.push_back(action.name);
  tool.parameters = {
      {"type", "object"},
      {"properties",
       {{"action", {{"type", "string"}, {"enum", std::move(names)}}},
        {"url", {{"type", "string"}}},
        {"target_id", {{"type", "string"}}},
        {"view_id", {{"type", "string"}}},
        {"element", {{"type", "integer"}}},
        {"offset", {{"type", "integer"}}},
        {"find", {{"type", "string"}}},
        {"x", {{"type", "integer"}}},
        {"y", {{"type", "integer"}}},
        {"delta_y", {{"type", "integer"}}},
        {"text", {{"type", "string"}}},
        {"replace", {{"type", "boolean"}}},
        {"new_tab", {{"type", "boolean"}}},
        {"close", {{"type", "boolean"}}},
        {"key",
         {{"type", "string"},
          {"enum",
           {"Enter", "Tab", "Escape", "Backspace", "Delete", "Space", "ArrowUp",
            "ArrowDown", "ArrowLeft", "ArrowRight", "PageUp", "PageDown",
            "Home", "End"}}}},
        {"reason", {{"type", "string"}}}}},
      {"required", {"action"}},
      {"additionalProperties", false}};
  // Switching tabs changes what the user sees too; an unknown action is
  // taken for one that acts.
  tool.mutates = [](const json& args) {
    const Action* action = FindAction(args);
    return !action || action->acts ||
           (std::string_view(action->name) == "tabs" &&
            !JsonValue(args, "target_id", "").empty());
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
    const Action* action = FindAction(args);
    const std::string name = action ? action->name : "";
    return json{
        {"verb",
         {action ? action->doing : "Checking",
          action ? action->done : "Checked"}},
        {"target", name == "open"   ? JsonValue(args, "url", "")
                   : name == "type" ? FirstLine(JsonValue(args, "text", ""))
                                    : "the browser"}};
  };
  // Text is counted in characters and results in bytes; a page in a script
  // of several bytes a character still arrives whole, with its elements.
  tool.result_chars = 4 * kPageTextChars + 12288;
  tool.run = [session_id = std::move(session_id), ask = std::move(ask)](
                 const json& args, const ToolContext& context) {
    const std::string action = JsonValue(args, "action", "");
    if (action == "request_human") {
      return Handover(session_id,
                      JsonValue(args, "reason", "Please finish in the browser"),
                      ask);
    }
    if (action == "observe") {
      return Observation(session_id, "", context, /*look=*/true);
    }
    const Action* row = FindAction(args);
    if (!row || !row->acts) {
      json answer =
          Ask(session_id, args, action == "status" ? "agent_status" : action);
      if (auto error = JsonValue(answer, "error", ""); !error.empty()) {
        return ToolFailure(ToolErrorCode::kRemoteError, error);
      }
      return action == "read" ? Reading(answer, args, "")
                              : ToolSuccess(JsonDump(answer));
    }
    // type may click its field first and press a key after: one call, in
    // that order, stopping at the first that fails.
    std::vector<std::string> ops = {action};
    if (action == "type") {
      if (args.contains("element") || args.contains("x")) {
        ops.insert(ops.begin(), "click");
      }
      if (args.contains("key")) ops.emplace_back("press");
    }
    std::string done;
    bool whole = true;
    for (const std::string& op : ops) {
      const std::string error =
          !done.empty() && (AbortRequested() || context.Expired())
              ? "interrupted"
              : JsonValue(Ask(session_id, args, op), "error", "");
      if (!error.empty()) {
        if (done.empty()) {
          return ToolFailure(ToolErrorCode::kRemoteError, error);
        }
        done += op + " not done: " + error + ".\n";
        whole = false;
        break;
      }
      done += op + " done.\n";
    }
    done += Settle(session_id, context);
    // open may read the page it opened, once that has settled.
    if (action == "open" && whole &&
        (args.contains("find") || args.contains("offset"))) {
      json read = Ask(session_id, args, "read");
      if (!read.contains("error")) return Reading(read, args, done);
    }
    done.pop_back();
    return Observation(session_id, done, context, /*look=*/false);
  };
  return tool;
}
}  // namespace uagent
