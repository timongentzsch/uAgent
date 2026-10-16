// Copyright 2026 Timon Gentzsch

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "include/browser/browser.h"
#include "include/browser/runtime.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "src/browser/runtime_internal.h"

namespace uagent::browser {
namespace {
// Where a numbered element is now, brought into view and still for two
// frames, since a page may animate it there. Nothing when it left the page
// or has no size; what lies on top when something else would take the
// click, so the model can deal with that first.
constexpr const char* kElementScript = R"js((async n => {
  const e = (window.__uagentElements || [])[n - 1];
  if (!e || !e.isConnected) return null;
  e.scrollIntoView({block: 'center', inline: 'center', behavior: 'instant'});
  let r = e.getBoundingClientRect(), still = 0;
  for (let i = 0; i < 10 && still < 2; i++) {
    await new Promise(requestAnimationFrame);
    const now = e.getBoundingClientRect();
    still = now.x === r.x && now.y === r.y ? still + 1 : 0;
    r = now;
  }
  const x = Math.round(r.x + r.width / 2), y = Math.round(r.y + r.height / 2);
  if (r.width < 1 || r.height < 1) return null;
  const hit = document.elementFromPoint(x, y);
  if (hit && (e.contains(hit) || hit.contains(e))) return {x, y};
  if (!hit) return null;
  const words = (hit.innerText || '').replace(/\s+/g, ' ').trim().slice(0, 40);
  return {covered: hit.tagName.toLowerCase() + (hit.id ? '#' + hit.id : '') +
      (hit.classList[0] ? '.' + hit.classList[0] : '') +
      (words ? ' "' + words + '"' : '')};
}))js";

// Whether the focused field is one the screenshots mask (kSecretScript).
constexpr const char* kSecretFieldScript = R"js((e => !!e &&
  e.tagName === 'INPUT' && (e.hasAttribute('data-uagent-secret') ||
  e.type === 'password' || /password/i.test(e.getAttribute('autocomplete') || ''))
)(document.activeElement))js";

// types what `text` says: the name alone reaches the page's own listeners
// and nothing else, so Enter would not send a form nor Backspace delete.
struct Key {
  const char* name;
  int code;
  const char* text;
};
constexpr Key kKeys[] = {
    {"Enter", 13, "\r"},    {"Tab", 9, ""},        {"Escape", 27, ""},
    {"Backspace", 8, ""},   {"Delete", 46, ""},    {"Space", 32, " "},
    {"ArrowUp", 38, ""},    {"ArrowDown", 40, ""}, {"ArrowLeft", 37, ""},
    {"ArrowRight", 39, ""}, {"PageUp", 33, ""},    {"PageDown", 34, ""},
    {"Home", 36, ""},       {"End", 35, ""}};

bool Coordinate(const json& command, const char* name, int& result) {
  result = JsonValue(command, name, -1);
  return result >= 0 && result <= 10000;
}

// X keysyms.
constexpr uint32_t kKeyEscape = 0xFF1B, kKeyDown = 0xFF54, kKeyTab = 0xFF09;

// Presses keys on the browser's display, as a person at it would, each with
// the wait in milliseconds that follows it. Chrome's own lists (the saved
// logins it offers) take keys from there only, never from the debugging
// channel, which reaches the page alone.
bool PressOnDisplay(std::initializer_list<std::pair<uint32_t, int>> keys) {
  Fd rfb = ConnectUnix(RfbPath());
  if (!rfb) return false;
  std::string bytes;
  const auto take = [&](size_t count) {
    bytes.assign(count, '\0');
    for (size_t have = 0; have < count;) {
      pollfd ready{rfb.Get(), POLLIN, 0};
      if (poll(&ready, 1, kRfbIoMs) <= 0) {
        return false;
      }
      const ssize_t n = read(rfb.Get(), bytes.data() + have, count - have);
      if (n <= 0) return false;
      have += static_cast<size_t>(n);
    }
    return true;
  };
  const auto send = [&](std::string_view data) {
    return WriteAllWithin(rfb.Get(), data, kRfbIoMs);
  };
  using std::string_view_literals::operator""sv;
  // Version, the one security type the display is started with (none), its
  // result, then a shared session and the server's description of itself.
  if (!take(12) || !send("RFB 003.008\n"sv) || !take(1) ||
      !take(static_cast<unsigned char>(bytes[0])) || !send("\x01"sv) ||
      !take(4) || bytes != "\0\0\0\0"sv || !send("\x01"sv) || !take(24)) {
    return false;
  }
  uint32_t name = 0;
  for (size_t index = 20; index < 24; ++index) {
    name = name << 8 | static_cast<unsigned char>(bytes[index]);
  }
  if (name > 4096 || !take(name)) return false;
  for (const auto& [key, wait] : keys) {
    for (const char down : {'\1', '\0'}) {
      const char event[] = {'\4',
                            down,
                            '\0',
                            '\0',
                            static_cast<char>(key >> 24),
                            static_cast<char>(key >> 16),
                            static_cast<char>(key >> 8),
                            static_cast<char>(key)};
      if (!send(std::string_view(event, sizeof event))) return false;
      poll(nullptr, 0, 50);
    }
    poll(nullptr, 0, wait);
  }
  return true;
}
}  // namespace

// One action on the attached page.
json Runtime::Act(const std::string& op, const json& command) {
  // Remember the tabs that exist before an action, so the probes after it
  // can tell which tab the action itself opened.
  known_targets_.clear();
  action_target_ = target_;
  if (json pages = PageTargets(); pages.is_array()) {
    for (const auto& page : pages) {
      known_targets_.insert(JsonValue(page, "id", ""));
    }
  }
  json reply;
  if (op == "open") {
    std::string url = JsonValue(command, "url", "");
    // A file of the agent's own arrives as its markup and is written into a
    // blank page. It never gets a file:// origin, from which a page could
    // pull in the profile and every other local file.
    const std::string html = JsonValue(command, "html", "");
    if (!html.empty()) {
      url = "about:blank";
    } else if (!(url.starts_with("https://") || url.starts_with("http://")) ||
               url.size() > 4096) {
      return {{"error", "URL must be HTTP(S)"}};
    }
    observation_.clear();
    if (JsonValue(command, "new_tab", false)) {
      json created = Call("Target.createTarget", {{"url", "about:blank"}});
      const json* made = JsonObject(created, "result");
      std::string error;
      if (!made || !AttachPage(JsonValue(*made, "targetId", ""), error)) {
        return {{"error", made ? error : "Chrome did not open a tab"}};
      }
    }
    reply = Call("Page.navigate", {{"url", url}}, page_session_);
    if (const json* result = JsonObject(reply, "result")) {
      if (auto failed = JsonValue(*result, "errorText", ""); !failed.empty()) {
        return {{"error", "navigation failed: " + failed}};
      }
    }
    if (!html.empty() && CdError(reply).empty()) {
      reply = Call("Page.setDocumentContent",
                   {{"frameId", target_}, {"html", html}}, page_session_);
    }
  } else if (op == "back") {
    json history =
        Call("Page.getNavigationHistory", json::object(), page_session_);
    const json* result = JsonObject(history, "result");
    const json* entries = result ? JsonArray(*result, "entries") : nullptr;
    const int current = result ? JsonValue(*result, "currentIndex", -1) : -1;
    if (!entries || current < 1 ||
        current >= static_cast<int>(entries->size())) {
      return {{"error", CdError(history).empty() ? "there is no previous page"
                                                 : CdError(history)}};
    }
    // The same rule as open: history can hold chrome:// or file:// pages a
    // person visited, and back must not be a way to reach them.
    const std::string url = JsonValue(
        (*entries)[static_cast<size_t>(current - 1)], "url", std::string());
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
      return {{"error", "the previous page is not HTTP(S)"}};
    }
    observation_.clear();
    reply = Call(
        "Page.navigateToHistoryEntry",
        {{"entryId",
          JsonValue((*entries)[static_cast<size_t>(current - 1)], "id", 0)}},
        page_session_);
  } else if (op == "click") {
    if (observation_.empty() ||
        observation_ != JsonValue(command, "view_id", "")) {
      // The model has to pass the view_id its latest observe returned.
      return {{"error", observation_.empty()
                            ? "observe the current tab before clicking"
                            : "click needs view_id from the latest observe"}};
    }
    int x, y;
    if (const int element = JsonValue(command, "element", 0); element > 0) {
      json where = Call("Runtime.evaluate",
                        {{"expression", std::string(kElementScript) + "(" +
                                            std::to_string(element) + ")"},
                         {"returnByValue", true},
                         {"awaitPromise", true}},
                        page_session_);
      const json* found = JsonObject(where, "result");
      found = found ? JsonObject(*found, "result") : nullptr;
      found = found ? JsonObject(*found, "value") : nullptr;
      if (!found) return {{"error", "that element is gone; look again"}};
      if (const std::string over = JsonValue(*found, "covered", "");
          !over.empty()) {
        return {{"error", "element " + std::to_string(element) +
                              " is covered by " + over +
                              "; deal with that first"}};
      }
      x = JsonValue(*found, "x", -1);
      y = JsonValue(*found, "y", -1);
    } else if (!Coordinate(command, "x", x) || !Coordinate(command, "y", y)) {
      return {{"error", "click requires an element, or x and y"}};
    }
    if (x < 0 || y < 0 || x >= view_width_ || y >= view_height_) {
      return {{"error", "coordinates exceed the observed viewport"}};
    }
    observation_.clear();
    // Onto the point first, as a pointer gets there: menus open on hover.
    json event = {{"type", "mouseMoved"}, {"x", x}, {"y", y}};
    for (const char* type : {"mousePressed", "mouseReleased"}) {
      if (auto reason =
              CdError(Call("Input.dispatchMouseEvent", event, page_session_));
          !reason.empty()) {
        return {{"error", reason}};
      }
      event = {{"type", type},
               {"x", x},
               {"y", y},
               {"button", "left"},
               {"clickCount", 1},
               {"buttons", std::string_view(type) == "mousePressed" ? 1 : 0}};
    }
    reply = Call("Input.dispatchMouseEvent", event, page_session_);
  } else if (op == "type") {
    std::string value = JsonValue(command, "text", "");
    if (value.size() > 32768) return {{"error", "text exceeds limit"}};
    json secret =
        Call("Runtime.evaluate",
             {{"expression", kSecretFieldScript}, {"returnByValue", true}},
             page_session_);
    const json* field = JsonObject(secret, "result");
    field = field ? JsonObject(*field, "result") : nullptr;
    if (field && JsonValue(*field, "value", false)) {
      return {{"error",
               "this is a password field: use fill_saved, or request_human"}};
    }
    observation_.clear();
    if (JsonValue(command, "replace", false)) {
      // What the field holds becomes the selection, which the text replaces.
      // Unselected, the text would be added to it instead.
      json chosen = Call(
          "Input.dispatchKeyEvent",
          {{"type", "rawKeyDown"}, {"commands", json::array({"selectAll"})}},
          page_session_);
      if (auto reason = CdError(chosen); !reason.empty()) {
        return {{"error", reason}};
      }
    }
    reply = Call("Input.insertText", {{"text", value}}, page_session_);
  } else if (op == "fill_saved") {
    // The first login Chrome offers under the focused field. A look at the
    // page closes Chrome's list, so it is closed by now: Escape makes sure,
    // Down opens it, and after the moment Chrome ignores input to a list it
    // has just shown, Down and Tab choose. Tab, since with no list it only
    // moves on, where Return would send the form. How long the field's text
    // is, before and after, says whether Chrome filled it; the text itself
    // is never read.
    json field =
        Call("Runtime.evaluate", {{"expression", "document.activeElement"}},
             page_session_);
    const json* held = JsonObject(field, "result");
    held = held ? JsonObject(*held, "result") : nullptr;
    const std::string id = held ? JsonValue(*held, "objectId", "") : "";
    const auto length = [&] {
      json measured = Call("Runtime.callFunctionOn",
                           {{"objectId", id},
                            {"functionDeclaration",
                             "function(){return typeof this.value==='string'?"
                             "this.value.length:-1}"},
                            {"returnByValue", true}},
                           page_session_);
      const json* result = JsonObject(measured, "result");
      result = result ? JsonObject(*result, "result") : nullptr;
      return result ? JsonValue(*result, "value", -1) : -1;
    };
    const int before = id.empty() ? -1 : length();
    observation_.clear();
    if (!PressOnDisplay({{kKeyEscape, 300},
                         {kKeyDown, 1000},
                         {kKeyDown, 200},
                         {kKeyTab, 300}})) {
      return {{"error", "cannot reach the browser's display"}};
    }
    if (before >= 0 && length() == before) {
      return {
          {"error",
           "Chrome filled nothing: no saved login matches this field, or it "
           "already holds one"}};
    }
    reply = json::object();
  } else if (op == "press") {
    const std::string name = JsonValue(command, "key", "");
    const Key* key = std::ranges::find_if(
        kKeys, [&](const Key& known) { return name == known.name; });
    if (key == std::end(kKeys)) return {{"error", "unsupported key"}};
    observation_.clear();
    const std::string typed = key->text;
    json event = {{"type", typed.empty() ? "rawKeyDown" : "keyDown"},
                  {"key", typed == " " ? typed : name},
                  {"code", name},
                  {"windowsVirtualKeyCode", key->code}};
    if (!typed.empty()) event["text"] = typed;
    reply = Call("Input.dispatchKeyEvent", event, page_session_);
    if (CdError(reply).empty()) {
      event["type"] = "keyUp";
      event.erase("text");
      reply = Call("Input.dispatchKeyEvent", event, page_session_);
    }
  } else if (op == "scroll") {
    if (observation_.empty() ||
        observation_ != JsonValue(command, "view_id", "")) {
      // The model has to pass the view_id its latest observe returned.
      return {{"error", observation_.empty()
                            ? "observe the current tab before scrolling"
                            : "scroll needs view_id from the latest observe"}};
    }
    int x = JsonValue(command, "x", 640), y = JsonValue(command, "y", 400);
    int delta = JsonValue(command, "delta_y", 0);
    if (delta < -3000 || delta > 3000) {
      return {{"error", "invalid scroll amount"}};
    }
    if (x < 0 || y < 0 || x >= view_width_ || y >= view_height_) {
      return {{"error", "coordinates exceed the observed viewport"}};
    }
    observation_.clear();
    reply = Call("Input.dispatchMouseEvent",
                 {{"type", "mouseWheel"},
                  {"x", x},
                  {"y", y},
                  {"deltaX", 0},
                  {"deltaY", delta}},
                 page_session_);
  } else {
    return {{"error", "unsupported browser action"}};
  }
  const std::string error = CdError(reply);
  if (!error.empty()) return {{"error", error}};
  if (op == "open") moved_ = false;
  return {{"ok", true}};
}

// A dialog takes the keys a person would give it: Enter accepts, Escape
// dismisses, and text typed before Enter answers a prompt. A look reports
// it, since the page behind it cannot be read until it closes.
json Runtime::Dialog(const std::string& op, const json& command) {
  const std::string type = JsonValue(dialog_, "type", "");
  if (op == "probe" || op == "observe" || op == "read") {
    return {{"ok", true}, {"dialog", dialog_}};
  }
  if (op == "type" && type == "prompt") {
    dialog_["answer"] = JsonValue(command, "text", "");
    return {{"ok", true}};
  }
  const std::string key = JsonValue(command, "key", "");
  if (op != "press" || (key != "Enter" && key != "Escape")) {
    return {{"error", "a " + type +
                          " dialog is open: press Enter to accept "
                          "it or Escape to dismiss it first"}};
  }
  json answer = {{"accept", key == "Enter"}};
  if (key == "Enter" && dialog_.contains("answer")) {
    answer["promptText"] = dialog_["answer"];
  }
  if (auto reason =
          CdError(Call("Page.handleJavaScriptDialog", answer, page_session_));
      !reason.empty()) {
    return {{"error", reason}};
  }
  dialog_ = nullptr;
  return {{"ok", true}};
}
}  // namespace uagent::browser
