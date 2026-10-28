// Copyright 2026 Timon Gentzsch

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/browser/browser.h"
#include "include/browser/runtime.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "src/browser/runtime_internal.h"

namespace uagent::browser {
namespace {
// Where an element is now, brought into view and the same over two frames,
// since a page may animate it there (a hidden tab draws no frames and is not
// waited for). Nothing when it left the page or has no size; whether it is
// still moving after ten frames; and what lies on top when something else
// would take the click, so the model can deal with that. `held` leaves it
// where it is, and says when it is out of view there.
constexpr const char* kElementScript = R"js((async (e, held) => {
  if (!e || !e.isConnected) return null;
  if (!held) {
    e.scrollIntoView({block: 'center', inline: 'center', behavior: 'instant'});
  }
  // Its box, and the box of each frame it sits in, any of which may move.
  const box = () => {
    let at = JSON.stringify(e.getBoundingClientRect());
    for (let w = e.ownerDocument.defaultView; w.frameElement; w = w.parent) {
      at += JSON.stringify(w.frameElement.getBoundingClientRect());
    }
    return at;
  };
  let was = box(), still = 0;
  for (let i = 0; i < 10 && still < 2 && !document.hidden; i++) {
    await new Promise(requestAnimationFrame);
    const now = box();
    still = now === was ? still + 1 : 0;
    was = now;
  }
  if (still < 2 && !document.hidden) return {moving: true};
  const r = e.getBoundingClientRect();
  if (!e.isConnected || r.width < 1 || r.height < 1) return null;
  // Its own root says what lies on top of it there; then the shadow host or
  // frame it sits in must be on top in its root, the frame moving the point
  // by where it is, up to the page itself.
  let x = r.x + r.width / 2, y = r.y + r.height / 2, target = e;
  if (held && (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight)) {
    return {away: true};
  }
  for (;;) {
    const root = target.getRootNode();
    const hit = root.elementFromPoint(x, y);
    if (!hit) return null;
    if (!(target.contains(hit) || hit.contains(target))) {
      const words = (hit.innerText || '').replace(/\s+/g, ' ').trim().slice(0, 40);
      return {covered: hit.tagName.toLowerCase() + (hit.id ? '#' + hit.id : '') +
          (hit.classList[0] ? '.' + hit.classList[0] : '') +
          (words ? ' "' + words + '"' : '')};
    }
    if (root.host) {
      target = root.host;
      continue;
    }
    const frame = root.defaultView && root.defaultView.frameElement;
    if (!frame) break;
    // A frame drawn at another size (scaled, turned) maps no point simply.
    const f = frame.getBoundingClientRect();
    if (Math.abs(f.width - frame.offsetWidth) > 1 ||
        Math.abs(f.height - frame.offsetHeight) > 1) return {transformed: true};
    const pad = getComputedStyle(frame);
    x += f.x + frame.clientLeft + parseFloat(pad.paddingLeft);
    y += f.y + frame.clientTop + parseFloat(pad.paddingTop);
    target = frame;
  }
  return {x: Math.round(x), y: Math.round(y)};
}))js";

// The element a look numbered, as the page holds it (runtime_page.cc). The
// look becomes the last one used, as in looks_, so the looks its actions add
// do not push it out.
std::string Numbered(const std::string& look, int number) {
  return "(((m, k) => { const kept = m?.get(k); if (kept) { m.delete(k); "
         "m.set(k, kept); } return kept; })(window.__uagentLooks, " +
         JsonDump(look) + ") || [])[" + std::to_string(number - 1) + "]";
}

// Whether the field typed text reaches is one the screenshots mask
// (kSecretScript), followed into shadow roots and frames: "frame" for a
// frame of another site and "unknown" for anything but a known text field,
// such as one inside a closed shadow root, since neither can be told.
constexpr const char* kSecretFieldScript = R"js((() => {
  let e = document.activeElement;
  for (;;) {
    if (e && e.shadowRoot && e.shadowRoot.activeElement) {
      e = e.shadowRoot.activeElement;
    } else if (e && /^I?FRAME$/.test(e.tagName)) {
      let inner = null;
      try { inner = e.contentDocument; } catch (_) {}
      if (!inner) return 'frame';
      e = inner.activeElement;
    } else break;
  }
  if (!e || !(e.tagName === 'INPUT' || e.tagName === 'TEXTAREA' ||
              e.isContentEditable)) return 'unknown';
  return e.tagName === 'INPUT' && (e.hasAttribute('data-uagent-secret') ||
    e.type === 'password' || /password/i.test(e.getAttribute('autocomplete') || ''));
})())js";

// A select, or a date, time or color field, takes no typed text: Chrome
// draws it as a list or as segments, so inserted text lands nowhere and
// typing would report a value it never set. The focused one (followed as
// kSecretFieldScript does) is set as the page's own script would set it, and
// told of it through input and change; a value it refuses is said, with the
// form it takes. Anything else gives null and is typed into.
constexpr const char* kControlScript = R"js((text) => {
  let e = document.activeElement;
  for (;;) {
    if (e && e.shadowRoot && e.shadowRoot.activeElement) {
      e = e.shadowRoot.activeElement;
    } else if (e && /^I?FRAME$/.test(e.tagName)) {
      let inner = null;
      try { inner = e.contentDocument; } catch (_) {}
      if (!inner) return null;
      e = inner.activeElement;
    } else break;
  }
  const forms = {date: 'YYYY-MM-DD', time: 'HH:MM, 24-hour',
                 'datetime-local': 'YYYY-MM-DDTHH:MM', month: 'YYYY-MM',
                 week: 'YYYY-Www', color: '#rrggbb', range: 'a number'};
  const select = e && e.tagName === 'SELECT';
  if (!select && !(e && e.tagName === 'INPUT' && e.type in forms)) return null;
  const view = e.ownerDocument.defaultView;
  const want = text.trim();
  let value = want;
  if (select) {
    const label = (o) => o.label.trim().replace(/\s+/g, ' ').toLowerCase();
    const options = [...e.options];
    const match = options.find((o) => label(o) === want.toLowerCase()) ||
                  options.find((o) => o.value === want);
    if (!match || match.disabled) {
      return {error: 'this list has no ' + (match ? 'enabled ' : '') +
              'option ' + JSON.stringify(want) + '; its options: ' +
              options.slice(0, 20).map((o) => JSON.stringify(o.label.trim()))
                  .join(', ')};
    }
    value = match.value;
  }
  const proto = select ? view.HTMLSelectElement : view.HTMLInputElement;
  Object.getOwnPropertyDescriptor(proto.prototype, 'value').set.call(e, value);
  if (!select && want && !e.value) {
    return {error: 'a ' + e.type + ' field takes ' + forms[e.type] + '; ' +
            JSON.stringify(want) + ' is not one'};
  }
  e.dispatchEvent(new view.Event('input', {bubbles: true}));
  e.dispatchEvent(new view.Event('change', {bubbles: true}));
  return {set: true};
})js";

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

// Text is typed key by key, as a keyboard would: fields that listen for keys
// (autocomplete, masked and code inputs) see each one, and with no pause
// between keys a short field still fills at once. Longer text, and text with
// a character no key types (a control, an emoji), goes in at once instead.
constexpr size_t kKeyedChars = 256;
std::vector<std::string> KeyedCharacters(const std::string& text) {
  std::vector<std::string> keys;
  if (text.size() > kKeyedChars) return {};
  for (size_t at = 0; at < text.size();) {
    const auto lead = static_cast<unsigned char>(text[at]);
    const size_t width = lead < 0x80 ? 1 : lead >= 0xE0 ? 3 : 2;
    if (lead < 0x20 || lead == 0x7F || (lead >= 0x80 && lead < 0xC2) ||
        lead >= 0xF0 || at + width > text.size()) {
      return {};
    }
    keys.push_back(text.substr(at, width));
    at += width;
  }
  return keys;
}

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
bool PressOnDisplay(const std::string& socket,
                    std::initializer_list<std::pair<uint32_t, int>> keys) {
  Fd rfb = ConnectUnix(socket);
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
  if (op != "open") {
    if (json refused = Elsewhere(command, ""); !refused.is_null()) {
      return refused;
    }
  }
  // What the action downloads lands in its conversation's folder.
  if (const std::string into = JsonValue(command, "downloads", "");
      !into.empty() && into != download_path_ &&
      CdError(Call("Browser.setDownloadBehavior", {{"behavior", "allow"},
                                                   {"downloadPath", into},
                                                   {"eventsEnabled", true}}))
          .empty()) {
    download_path_ = into;
  }
  // A number names what any look at this page numbered, which is still that
  // element; a point is one of the latest look's alone.
  const std::string view = JsonValue(command, "view_id", "");
  const int element = JsonValue(command, "element", 0);
  const auto unseen = [&]() -> std::string {
    if (observation_.empty()) return "observe the current tab, then " + op;
    if (view == observation_) return "";
    if (const auto used = std::ranges::find(looks_, view);
        element > 0 && used != looks_.end()) {
      // Kept by use, not by age: a batch of actions on one look adds a look
      // per action, which would otherwise push the batch's own look out.
      std::rotate(used, used + 1, looks_.end());
      return "";
    }
    return op + (element > 0 ? " needs view_id from a look at this page"
                             : " needs view_id from the latest observe");
  };
  // Where element `number` is now, or why it cannot be acted on there;
  // `held` finds it without bringing it into view.
  const auto locate = [&](int number, int& x, int& y,
                          bool held = false) -> std::string {
    const std::string named = "element " + std::to_string(number);
    json where = Evaluate(
        {{"expression", std::string(kElementScript) + "(" +
                            Numbered(view, number) + (held ? ",true)" : ")")},
         {"returnByValue", true},
         {"awaitPromise", true}});
    const json* found = RemoteValue(where);
    if (!found) return "that element is gone; look again";
    if (JsonValue(*found, "moving", false)) {
      return named + " is still moving; look again";
    }
    if (JsonValue(*found, "away", false)) {
      return named +
             " is out of view while the drag's start is in it; drag by x "
             "and y, or in steps";
    }
    if (JsonValue(*found, "transformed", false)) {
      return named +
             " is in a frame drawn scaled or turned; act by x and y from a "
             "screenshot";
    }
    if (const std::string over = JsonValue(*found, "covered", "");
        !over.empty()) {
      return named + " is covered by " + over + "; deal with that first";
    }
    x = JsonValue(*found, "x", -1);
    y = JsonValue(*found, "y", -1);
    return "";
  };
  // A page left behind takes its looks with it.
  const auto navigate = [&](const char* method, const json& params) {
    observation_.clear();
    looks_.clear();
    leaving_ = true;
    json sent = Call(method, params, page_session_);
    leaving_ = false;
    return sent;
  };
  const auto outside = [&](int x, int y) {
    return x < 0 || y < 0 || x >= view_width_ || y >= view_height_;
  };
  constexpr const char* kOutside = "coordinates exceed the observed viewport";
  // Pointer actions act on what a look showed.
  if (op == "click" || op == "drag" || op == "scroll" || op == "upload") {
    if (const std::string why = unseen(); !why.empty()) {
      return {{"error", why}};
    }
  }
  // A click may open a file chooser: held back, it never shows a window the
  // agent cannot answer, and upload fills its field instead.
  struct Release {
    Runtime* runtime;
    ~Release() {
      if (runtime) {
        runtime->Call("Page.setInterceptFileChooserDialog",
                      {{"enabled", false}}, runtime->page_session_);
      }
    }
  } release{nullptr};
  if (op == "click") {
    chooser_ = 0;
    if (CdError(Call("Page.setInterceptFileChooserDialog", {{"enabled", true}},
                     page_session_))
            .empty()) {
      release.runtime = this;
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
    if (JsonValue(command, "new_tab", false)) {
      json created = Call("Target.createTarget", {{"url", "about:blank"}});
      const json* made = JsonObject(created, "result");
      std::string error;
      if (!made || !AttachPage(JsonValue(*made, "targetId", ""), error)) {
        return {{"error", made ? error : "Chrome did not open a tab"}};
      }
    }
    reply = navigate("Page.navigate", {{"url", url}});
    if (const json* result = JsonObject(reply, "result")) {
      if (auto failed = JsonValue(*result, "errorText", ""); !failed.empty()) {
        return {{"error", "navigation failed: " + failed}};
      }
    }
    if (!html.empty() && CdError(reply).empty()) {
      reply = Call("Page.setDocumentContent",
                   {{"frameId", target_}, {"html", html}}, page_session_);
    }
  } else if (op == "back" || op == "forward") {
    const bool back = op == "back";
    const std::string which = back ? "previous" : "next";
    json history =
        Call("Page.getNavigationHistory", json::object(), page_session_);
    const json* result = JsonObject(history, "result");
    const json* entries = result ? JsonArray(*result, "entries") : nullptr;
    const int current = result ? JsonValue(*result, "currentIndex", -1) : -1;
    const int wanted = current + (back ? -1 : 1);
    if (!entries || current < 0 || wanted < 0 ||
        wanted >= static_cast<int>(entries->size())) {
      return {{"error", CdError(history).empty()
                            ? "there is no " + which + " page"
                            : CdError(history)}};
    }
    // The same rule as open: history can hold chrome:// or file:// pages a
    // person visited, and back must not be a way to reach them.
    const json& entry = (*entries)[static_cast<size_t>(wanted)];
    const std::string url = JsonValue(entry, "url", std::string());
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
      return {{"error", "the " + which + " page is not HTTP(S)"}};
    }
    reply = navigate("Page.navigateToHistoryEntry",
                     {{"entryId", JsonValue(entry, "id", 0)}});
  } else if (op == "reload") {
    reply = navigate("Page.reload", json::object());
  } else if (op == "click") {
    const std::string button = JsonValue(command, "button", "left");
    const int pressed = button == "left"     ? 1
                        : button == "right"  ? 2
                        : button == "middle" ? 4
                                             : 0;
    const int count = JsonValue(command, "count", 1);
    if (!pressed || count < 1 || count > 3) {
      return {{"error",
               "click takes button left, right or middle, and count 1 to 3"}};
    }
    int x = -1, y = -1;
    if (element > 0) {
      if (const std::string why = locate(element, x, y); !why.empty()) {
        return {{"error", why}};
      }
    } else if (!Coordinate(command, "x", x) || !Coordinate(command, "y", y)) {
      return {{"error", "click requires an element, or x and y"}};
    }
    if (outside(x, y)) return {{"error", kOutside}};
    observation_.clear();
    // Onto the point first, as a pointer gets there: menus open on hover,
    // and what opens may cover the element, so it is found again after.
    if (auto reason = CdError(Call("Input.dispatchMouseEvent",
                                   {{"type", "mouseMoved"}, {"x", x}, {"y", y}},
                                   page_session_));
        !reason.empty()) {
      return {{"error", reason}};
    }
    if (element > 0) {
      const int was_x = x, was_y = y;
      if (const std::string why = locate(element, x, y); !why.empty()) {
        return {{"error", why}};
      }
      if (x != was_x || y != was_y) {
        return {{"error", "element " + std::to_string(element) +
                              " moved when the pointer reached it; look "
                              "again"}};
      }
    }
    // A double click is a click, then one counted as the second.
    for (int click = 1; click <= count; ++click) {
      json event = {{"type", "mousePressed"},
                    {"x", x},
                    {"y", y},
                    {"button", button},
                    {"clickCount", click},
                    {"buttons", pressed}};
      if (auto reason =
              CdError(Call("Input.dispatchMouseEvent", event, page_session_));
          !reason.empty()) {
        return {{"error", reason}};
      }
      event["type"] = "mouseReleased";
      event["buttons"] = 0;
      reply = Call("Input.dispatchMouseEvent", event, page_session_);
      if (!CdError(reply).empty()) break;
    }
  } else if (op == "drag") {
    // It starts at element (or x and y), brought into view, and ends at
    // to_element (or to_x and to_y) wherever that then is.
    int x = -1, y = -1, to_x = -1, to_y = -1;
    const int to = JsonValue(command, "to_element", 0);
    if (element > 0) {
      if (const std::string why = locate(element, x, y); !why.empty()) {
        return {{"error", why}};
      }
    } else if (!Coordinate(command, "x", x) || !Coordinate(command, "y", y)) {
      return {{"error", "drag starts at an element, or x and y"}};
    }
    if (to > 0) {
      if (const std::string why = locate(to, to_x, to_y, true); !why.empty()) {
        return {{"error", why}};
      }
    } else if (!Coordinate(command, "to_x", to_x) ||
               !Coordinate(command, "to_y", to_y)) {
      return {{"error", "drag ends at to_element, or to_x and to_y"}};
    }
    if (outside(x, y) || outside(to_x, to_y)) return {{"error", kOutside}};
    observation_.clear();
    // A slider or a sortable list follows the pointer. The page's own
    // drag-and-drop would hand the drag to the system, which never sees
    // these events: Chrome holds it back instead, and it is dropped where
    // the pointer ends.
    dragged_ = nullptr;
    Call("Input.setInterceptDrags", {{"enabled", true}}, page_session_);
    const auto pointer = [&](const char* type, int at_x, int at_y, int held) {
      return CdError(Call("Input.dispatchMouseEvent",
                          {{"type", type},
                           {"x", at_x},
                           {"y", at_y},
                           {"button", held ? "left" : "none"},
                           {"buttons", held},
                           {"clickCount", 1}},
                          page_session_));
    };
    std::string reason = pointer("mouseMoved", x, y, 0);
    if (reason.empty()) reason = pointer("mousePressed", x, y, 1);
    // In steps, as a hand moves: a page may start a drag only once the
    // pointer has travelled.
    constexpr int kSteps = 8;
    for (int step = 1; reason.empty() && step <= kSteps; ++step) {
      reason = pointer("mouseMoved", x + (to_x - x) * step / kSteps,
                       y + (to_y - y) * step / kSteps, 1);
    }
    if (reason.empty() && dragged_.is_object()) {
      for (const char* type : {"dragEnter", "dragOver", "drop"}) {
        reason = CdError(
            Call("Input.dispatchDragEvent",
                 {{"type", type}, {"x", to_x}, {"y", to_y}, {"data", dragged_}},
                 page_session_));
        if (!reason.empty()) break;
      }
    }
    // The button comes up even after a failure, so nothing stays held.
    reply = Call("Input.dispatchMouseEvent",
                 {{"type", "mouseReleased"},
                  {"x", to_x},
                  {"y", to_y},
                  {"button", "left"},
                  {"buttons", 0},
                  {"clickCount", 1}},
                 page_session_);
    Call("Input.setInterceptDrags", {{"enabled", false}}, page_session_);
    if (!reason.empty()) return {{"error", reason}};
  } else if (op == "type") {
    std::string value = JsonValue(command, "text", "");
    if (value.size() > 32768) return {{"error", "text exceeds limit"}};
    json control = Evaluate({{"expression", std::string("(") + kControlScript +
                                                ")(" + JsonDump(value) + ")"},
                             {"returnByValue", true}});
    if (const json* set = RemoteValue(control)) {
      if (const std::string why = JsonValue(*set, "error", ""); !why.empty()) {
        return {{"error", why}};
      }
      observation_.clear();
      return {{"ok", true}};
    }
    json secret =
        Evaluate({{"expression", kSecretFieldScript}, {"returnByValue", true}});
    const json* field = RemoteObject(secret);
    const json verdict =
        field && field->contains("value") ? (*field)["value"] : json();
    if (verdict != false) {
      return {{"error", verdict == "frame"
                            ? "the field is in another site's frame, whose "
                              "kind cannot be told: request_human"
                        : verdict == true
                            ? "this is a password field: use fill_saved, or "
                              "request_human"
                            : "cannot tell which field has focus; click the "
                              "field, or request_human"}};
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
    const std::vector<std::string> keys = KeyedCharacters(value);
    if (keys.empty()) {
      reply = Call("Input.insertText", {{"text", value}}, page_session_);
    }
    for (const std::string& character : keys) {
      // The key a US layout has for a letter, digit or space; 0 otherwise.
      const char ascii = character.size() == 1 ? character[0] : '\0';
      const int code = ascii >= 'a' && ascii <= 'z' ? ascii - 'a' + 'A'
                       : (ascii >= 'A' && ascii <= 'Z') ||
                               (ascii >= '0' && ascii <= '9') || ascii == ' '
                           ? ascii
                           : 0;
      json event = {{"type", "keyDown"},
                    {"key", character},
                    {"text", character},
                    {"windowsVirtualKeyCode", code}};
      reply = Call("Input.dispatchKeyEvent", event, page_session_);
      if (!CdError(reply).empty()) break;
      event["type"] = "keyUp";
      event.erase("text");
      reply = Call("Input.dispatchKeyEvent", event, page_session_);
      if (!CdError(reply).empty()) break;
    }
  } else if (op == "fill_saved") {
    // The first login Chrome offers under the focused field. A look at the
    // page closes Chrome's list, so it is closed by now: Escape makes sure,
    // Down opens it, and after the moment Chrome ignores input to a list it
    // has just shown, Down and Tab choose. Tab, since with no list it only
    // moves on, where Return would send the form. How long the field's text
    // is, before and after, says whether Chrome filled it; the text itself
    // is never read.
    json field = Evaluate({{"expression", "document.activeElement"}});
    const json* held = RemoteObject(field);
    const std::string id = held ? JsonValue(*held, "objectId", "") : "";
    const auto length = [&] {
      json measured = Call("Runtime.callFunctionOn",
                           {{"objectId", id},
                            {"functionDeclaration",
                             "function(){return typeof this.value==='string'?"
                             "this.value.length:-1}"},
                            {"returnByValue", true}},
                           page_session_);
      const json* result = RemoteObject(measured);
      return result ? JsonValue(*result, "value", -1) : -1;
    };
    const int before = id.empty() ? -1 : length();
    observation_.clear();
    if (!PressOnDisplay(RfbSocket(), {{kKeyEscape, 300},
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
    // A key of kKeys or one character, after any of Alt, Control, Meta and
    // Shift joined to it by +, as in Control+A.
    std::string name = JsonValue(command, "key", "");
    int modifiers = 0;
    for (size_t plus = name.find('+');
         plus != std::string::npos && plus + 1 < name.size();
         plus = name.find('+')) {
      const std::string held = name.substr(0, plus);
      const int bit = held == "Alt"       ? 1
                      : held == "Control" ? 2
                      : held == "Meta"    ? 4
                      : held == "Shift"   ? 8
                                          : 0;
      if (!bit) return {{"error", "unsupported key"}};
      modifiers |= bit;
      name.erase(0, plus + 1);
    }
    const Key* key = std::ranges::find_if(
        kKeys, [&](const Key& known) { return name == known.name; });
    std::string typed, code = name;
    int number = 0;
    if (key != std::end(kKeys)) {
      typed = key->text;
      number = key->code;
    } else if (name.size() == 1 && name[0] > ' ' && name[0] < 0x7F) {
      const auto ascii = static_cast<unsigned char>(name[0]);
      const char upper = static_cast<char>(std::toupper(ascii));
      if (modifiers & 8) name = std::string(1, upper);
      typed = name;
      code = std::isalpha(ascii)   ? std::string("Key") + upper
             : std::isdigit(ascii) ? std::string("Digit") + name
                                   : "";
      number = std::isalnum(ascii) ? upper : 0;
    } else {
      return {{"error", "unsupported key"}};
    }
    // With Control, Alt or Meta held it is a shortcut, which types nothing.
    if (modifiers & 7) typed.clear();
    observation_.clear();
    json event = {{"type", typed.empty() ? "rawKeyDown" : "keyDown"},
                  {"key", typed == " " ? typed : name},
                  {"code", code},
                  {"windowsVirtualKeyCode", number},
                  {"modifiers", modifiers}};
    if (!typed.empty()) event["text"] = typed;
    reply = Call("Input.dispatchKeyEvent", event, page_session_);
    if (CdError(reply).empty()) {
      event["type"] = "keyUp";
      event.erase("text");
      reply = Call("Input.dispatchKeyEvent", event, page_session_);
    }
  } else if (op == "scroll") {
    int x = JsonValue(command, "x", 640), y = JsonValue(command, "y", 400);
    int delta = JsonValue(command, "delta_y", 0);
    if (delta < -3000 || delta > 3000) {
      return {{"error", "invalid scroll amount"}};
    }
    if (outside(x, y)) return {{"error", kOutside}};
    observation_.clear();
    reply = Call("Input.dispatchMouseEvent",
                 {{"type", "mouseWheel"},
                  {"x", x},
                  {"y", y},
                  {"deltaX", 0},
                  {"deltaY", delta}},
                 page_session_);
  } else if (op == "upload") {
    const std::string file = JsonValue(command, "file", "");
    // A file field takes the file itself; anything else is clicked, and the
    // file field whose chooser that opens takes it.
    json held = Evaluate({{"expression", Numbered(view, element)}});
    const json* found = RemoteObject(held);
    const std::string id = found ? JsonValue(*found, "objectId", "") : "";
    if (id.empty()) return {{"error", "that element is gone; look again"}};
    json files = {{"files", json::array({file})}, {"objectId", id}};
    json kind = Call("Runtime.callFunctionOn",
                     {{"objectId", id},
                      {"functionDeclaration",
                       "function(){return this.tagName==='INPUT'&&"
                       "this.type==='file'}"},
                      {"returnByValue", true}},
                     page_session_);
    const json* field = RemoteObject(kind);
    if (!field || !JsonValue(*field, "value", false)) {
      if (json clicked = Act("click", command); clicked.contains("error")) {
        return clicked;
      }
      for (int waited = 0; !chooser_ && waited < 2000; waited += 100) {
        poll(nullptr, 0, 100);
        Evaluate({{"expression", "0"}});
      }
      if (!chooser_) {
        return {{"error", "element " + std::to_string(element) +
                              " opened no file chooser; upload into the "
                              "file field itself"}};
      }
      files = {{"files", json::array({file})}, {"backendNodeId", chooser_}};
    }
    observation_.clear();
    reply = Call("DOM.setFileInputFiles", files, page_session_);
  } else {
    return {{"error", "unsupported browser action"}};
  }
  const std::string error = CdError(reply);
  if (!error.empty()) return {{"error", error}};
  if (op == "open") moved_ = false;
  if (op == "click" && chooser_) return {{"ok", true}, {"chooser", true}};
  return {{"ok", true}};
}

// A dialog takes the keys a person would give it: Enter accepts, Escape
// dismisses, and text typed before Enter answers a prompt. A look reports
// it, since the page behind it cannot be read until it closes.
// An action approved for one site is done on that site only: the page may
// have moved since it was approved, by itself or by a person. `url` is the
// page's when known; else the attached tab is asked.
json Runtime::Elsewhere(const json& command, std::string url) {
  // On a page that is no web site, the look it was approved on is the page,
  // as long as it shows the same document.
  if (const std::string look = JsonValue(command, "look", "");
      !look.empty() && std::ranges::find(looks_, look) == looks_.end()) {
    return {{"error",
             "the page changed since the look this was approved on; observe "
             "it, then act again"}};
  }
  const std::string site = JsonValue(command, "origin", "");
  if (site.empty()) return nullptr;
  if (url.empty()) {
    json info = Call("Target.getTargetInfo", {{"targetId", target_}});
    const json* result = JsonObject(info, "result");
    result = result ? JsonObject(*result, "targetInfo") : nullptr;
    url = result ? JsonValue(*result, "url", "") : "";
  }
  if (Origin(url) == site) return nullptr;
  return {{"error", "the page is on " + url.substr(0, url.find_first_of("?#")) +
                        " now, not on " + site +
                        " where this was approved; observe it, then act "
                        "again"}};
}

json Runtime::Dialog(const std::string& op, const json& command) {
  json& open = *OpenDialog();
  if (op == "type" || op == "press") {
    if (moved_) {
      return {{"error",
               "another conversation used the browser since; observe before "
               "answering its dialog"}};
    }
    if (json refused = Elsewhere(command, JsonValue(open, "url", ""));
        !refused.is_null()) {
      return refused;
    }
  }
  const std::string type = JsonValue(open, "type", "");
  const std::string key = JsonValue(command, "key", "");
  if (op == "type" && type == "prompt") {
    open["answer"] = JsonValue(command, "text", "");
    return {{"ok", true}};
  }
  if (op == "press" && (key == "Enter" || key == "Escape")) {
    json answer = {{"accept", key == "Enter"}};
    if (key == "Enter" && open.contains("answer")) {
      answer["promptText"] = open["answer"];
    }
    if (auto reason =
            CdError(Call("Page.handleJavaScriptDialog", answer, page_session_));
        !reason.empty()) {
      return {{"error", reason}};
    }
    dialogs_.erase(page_session_);
    return {{"ok", true}};
  }
  if (op != "probe" && op != "observe" && op != "read") {
    return {{"error", "a " + type +
                          " dialog is open: press Enter to accept "
                          "it or Escape to dismiss it first"}};
  }
  // A look at the dialog is a look at the page as it is now.
  if (op == "observe") moved_ = false;
  return {{"ok", true}, {"dialog", open}};
}
}  // namespace uagent::browser
