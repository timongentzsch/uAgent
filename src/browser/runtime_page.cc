// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <string>
#include <utility>

#include "include/browser/browser.h"
#include "include/browser/runtime.h"
#include "include/core/fs.h"
#include "include/core/platform.h"
#include "src/browser/runtime_internal.h"

namespace uagent::browser {
namespace {
// One cheap snapshot of the document; equal consecutive snapshots of a
// loaded page mean it settled. The document identity changes on navigation.
constexpr const char* kProbeScript =
    "(()=>{const t=document.body?document.body.innerText:'';let h=0;"
    "for(let i=0;i<Math.min(t.length,4096);i++)h=(h*31+t.charCodeAt(i))|0;"
    "return {ready:document.readyState,doc:String(performance.timeOrigin),"
    "sig:t.length+':'+h+':'+document.getElementsByTagName('*').length}})()";

// The page, for a look (`acting`) or for reading. Both get its address,
// title, how long its text is, and narrow, high-signal evidence of a bot
// wall: an error status, a challenge title, a large visible challenge frame,
// or a short page that asks for human verification. Advisory only; the agent
// decides.
//
// A look also gets what can be acted on, numbered and kept on the page so a
// click can name one by its number: in the document, its open shadow roots
// and the frames of its own site. A field says whether it holds text, never
// what; a control says when it is disabled, required, expanded or not.
//
// Reading gets text from `offset`, or with `find` the lines that contain it
// (from match `offset` on) and the links that do.
std::string PageScript() {
  const std::string chars = std::to_string(kPageTextChars);
  return std::string(R"js(((offset, find, acting) => {
  const t = document.body ? document.body.innerText : '';
  const title = document.title || '';
  let block = null;
  const nav = performance.getEntriesByType('navigation')[0];
  const status = nav && nav.responseStatus || 0;
  if (status === 403 || status === 429) {
    block = {kind: status === 429 ? 'rate_limited' : 'access_denied',
             evidence: 'HTTP ' + status};
  }
  if (!block && /^(just a moment|attention required|security check|access denied)/i
          .test(title.trim())) {
    block = {kind: 'bot_check', evidence: 'title "' + title.slice(0, 80) + '"'};
  }
  if (!block) for (const frame of document.querySelectorAll('iframe[src]')) {
    if (!/challenges\.cloudflare\.com|hcaptcha\.com|google\.com\/recaptcha|captcha-delivery\.com|px-captcha/
             .test(frame.src)) continue;
    const r = frame.getBoundingClientRect();
    if (r.width * r.height >= 0.15 * innerWidth * innerHeight ||
        (r.width > 0 && t.length < 400)) {
      block = {kind: 'captcha',
               evidence: 'challenge frame ' + new URL(frame.src).host};
      break;
    }
  }
  if (!block && t.length < 1500) {
    const m = /unusual (traffic|behaviou?r)|verify you are (a )?human|are you a robot|not a robot|press (&|and) hold/i
                  .exec(t);
    if (m) block = {kind: 'bot_check', evidence: 'page says "' + m[0] + '"'};
  }
  const page = {url: location.href.split(/[?#]/)[0], title, chars: t.length,
                block};
  const plain = s => (s || '').replace(/\s+/g, ' ').trim();
  if (acting) {
    const kept = window.__uagentElements = [];
    const rows = [];
    const actable =
        'a[href],button,select,textarea,summary,input:not([type=hidden]),' +
        '[role=button],[role=link],[role=tab],[role=menuitem],' +
        '[role=checkbox],[role=radio],[role=option],[role=switch],' +
        '[contenteditable=true]';
    // Document order, each open shadow root and shown frame of the site in
    // place.
    const walk = function* (root) {
      const nodes = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT);
      for (let n = nodes.nextNode(); n; n = nodes.nextNode()) {
        if (n.matches(actable)) yield n;
        if (n.shadowRoot) yield* walk(n.shadowRoot);
        if (n.tagName === 'IFRAME' || n.tagName === 'FRAME') {
          let inner = null;
          try { inner = n.contentDocument; } catch (_) {}
          const box = n.getBoundingClientRect();
          if (inner && box.width >= 1 && box.height >= 1 &&
              getComputedStyle(n).visibility !== 'hidden') yield* walk(inner);
        }
      }
    };
    for (const n of walk(document)) {
      const r = n.getBoundingClientRect();
      if (r.width < 1 || r.height < 1) continue;
      const style = n.ownerDocument.defaultView.getComputedStyle(n);
      if (style.visibility === 'hidden' || style.display === 'none') continue;
      const tag = n.tagName.toLowerCase();
      const kind = n.getAttribute('role') ||
          (tag === 'a' ? 'link' : tag === 'input' ? (n.type || 'text') : tag);
      const name = plain(
          n.getAttribute('aria-label') ||
          (n.labels && n.labels[0] && n.labels[0].innerText) ||
          (tag !== 'select' && n.innerText) || n.placeholder || n.title ||
          n.alt || n.name ||
          ((n.type === 'submit' || n.type === 'button') && n.value)).slice(0, 80);
      const expanded = n.getAttribute('aria-expanded');
      const state =
          (n.type === 'checkbox' || n.type === 'radio'
              ? (n.checked ? ' (checked)' : '')
              : tag === 'select'
                    ? ' (' + plain(n.selectedOptions[0] &&
                                   n.selectedOptions[0].text).slice(0, 40) + ')'
                    : tag === 'input' || tag === 'textarea'
                          ? (n.value ? ' (filled)' : ' (empty)') : '') +
          (n.required || n.getAttribute('aria-required') === 'true'
               ? ' (required)' : '') +
          (expanded === 'true' ? ' (expanded)'
               : expanded === 'false' ? ' (collapsed)' : '') +
          (n.matches(':disabled') || n.getAttribute('aria-disabled') === 'true'
               ? ' (disabled)' : '');
      kept.push(n);
      rows.push('[' + kept.length + '] ' + kind + ' "' + name + '"' + state);
      if (kept.length >= 100) break;
    }
    return {...page, text: t.slice(0, )js") +
         chars +
         R"js(), elements: rows.join('\n')};
  }
  const wanted = find.toLowerCase();
  const links = new Map();
  for (const a of document.querySelectorAll('a[href]')) {
    if (links.size >= 100) break;
    if (typeof a.href !== 'string' || !/^https?:/.test(a.href)) continue;
    const name = plain(a.innerText || a.getAttribute('aria-label'));
    if (links.has(a.href) ||
        (find ? !(name + ' ' + a.href).toLowerCase().includes(wanted)
              : offset > 0)) continue;
    links.set(a.href, name.slice(0, 80));
  }
  const list = [...links].map(([to, name]) => name + ' -> ' + to).join('\n');
  if (!find) {
    return {...page, text: t.slice(offset, offset + )js" +
         chars +
         R"js(), links: list};
  }
  // A long line is cut around what was searched for, not before it.
  const hits = [];
  for (const line of t.split('\n')) {
    const at = line.toLowerCase().indexOf(wanted);
    if (at < 0) continue;
    const from = Math.max(0, Math.min(at - 100, line.length - 300));
    hits.push(line.slice(from, from + 300).trim());
  }
  return {...page, matches: hits.length,
          text: hits.slice(offset, offset + 200).join('\n'), links: list};
}))js";
}
}  // namespace

// The web page tabs, capped: id, clean URL, title and the tab that opened it.
json Runtime::PageTargets() {
  json targets = Call("Target.getTargets");
  if (auto reason = CdError(targets); !reason.empty()) {
    return {{"error", reason}};
  }
  json pages = json::array();
  const json* result = JsonObject(targets, "result");
  const json* infos = result ? JsonArray(*result, "targetInfos") : nullptr;
  for (const auto& info : infos ? *infos : json::array()) {
    if (pages.size() >= kListedTabs) break;
    std::string url = JsonValue(info, "url", "");
    if (JsonValue(info, "type", "") != "page" ||
        !(url.starts_with("https://") || url.starts_with("http://") ||
          url == "about:blank")) {
      continue;
    }
    pages.push_back({{"id", JsonValue(info, "targetId", "")},
                     {"url", url.substr(0, url.find_first_of("?#"))},
                     {"title", JsonValue(info, "title", "")},
                     {"opener", JsonValue(info, "openerId", "")}});
  }
  return pages;
}

// One short look after an action: follows the single tab it opened, lists
// any others, and snapshots the document so the caller can wait for it to
// settle without holding the service for the whole wait.
json Runtime::Probe() {
  json pages = PageTargets();
  if (pages.contains("error")) return pages;
  json opened = json::array(), switched;
  std::string follow;
  int linked = 0;
  for (const auto& page : pages) {
    const std::string id = JsonValue(page, "id", "");
    if (known_targets_.count(id) || id == target_) continue;
    opened.push_back({{"id", id}, {"url", page["url"]}});
    if (JsonValue(page, "opener", "") == action_target_) {
      ++linked;
      follow = id;
    }
  }
  if (linked == 1 && target_ == action_target_) {
    std::string error;
    if (auto reason =
            CdError(Call("Target.activateTarget", {{"targetId", follow}}));
        !reason.empty()) {
      return {{"error", reason}};
    }
    if (!AttachPage(follow, error)) return {{"error", error}};
    for (auto it = opened.begin(); it != opened.end(); ++it) {
      if (JsonValue(*it, "id", "") == follow) {
        switched = *it;
        opened.erase(it);
        break;
      }
    }
  }
  json state = Call("Runtime.evaluate",
                    {{"expression", kProbeScript}, {"returnByValue", true}},
                    page_session_);
  const json* response = JsonObject(state, "result");
  const json* remote = response ? JsonObject(*response, "result") : nullptr;
  const json* value = remote ? JsonObject(*remote, "value") : nullptr;
  json result = value ? *value : json::object();
  result["ok"] = true;
  result["loading"] = loading_;
  result["opened"] = std::move(opened);
  if (!switched.is_null()) result["switched"] = std::move(switched);
  return result;
}

json Runtime::Read(const json& command) {
  json page =
      Call("Runtime.evaluate",
           {{"expression",
             PageScript() + "(" +
                 std::to_string(std::max(JsonValue(command, "offset", 0), 0)) +
                 "," + JsonDump(JsonValue(command, "find", "")) + ",false)"},
            {"returnByValue", true}},
           page_session_);
  if (auto reason = CdError(page); !reason.empty()) {
    return {{"error", reason}};
  }
  const json* value = JsonObject(page, "result");
  value = value ? JsonObject(*value, "result") : nullptr;
  value = value ? JsonObject(*value, "value") : nullptr;
  if (!value) return {{"error", "the page could not be read"}};
  moved_ = false;
  json read = *value;
  read["ok"] = true;
  return read;
}

json Runtime::Observe(bool image) {
  json metrics = Call("Page.getLayoutMetrics", json::object(), page_session_);
  if (auto reason = CdError(metrics); !reason.empty()) {
    return {{"error", reason}};
  }
  const json* result = JsonObject(metrics, "result");
  const json* viewport =
      result ? JsonObject(*result, "cssVisualViewport") : nullptr;
  double width = viewport ? JsonValue(*viewport, "clientWidth", 0.0) : 0;
  double height = viewport ? JsonValue(*viewport, "clientHeight", 0.0) : 0;
  if (!viewport || width < 1 || width > 10000 || height < 1 || height > 10000) {
    return {{"error", "Chrome did not report its viewport"}};
  }
  // Capture exactly the visible CSS viewport at one image pixel per CSS
  // pixel, so screenshot coordinates are click coordinates. The clip is in
  // device-independent pixels relative to the document.
  double zoom = JsonValue(*viewport, "zoom", 1.0);
  if (zoom <= 0) zoom = 1;
  std::string picture;
  if (image) {
    // No picture unless the mask is known to be in place.
    json masked = Call("Runtime.evaluate", {{"expression", kSecretScript}},
                       page_session_);
    const json* ran = JsonObject(masked, "result");
    if (!CdError(masked).empty() || !ran || ran->contains("exceptionDetails")) {
      return {{"error", CdError(masked).empty()
                            ? "the page kept password fields from being masked"
                            : CdError(masked)}};
    }
    json shot = Call("Page.captureScreenshot",
                     {{"format", "jpeg"},
                      {"quality", 70},
                      {"clip",
                       {{"x", JsonValue(*viewport, "pageX", 0.0) * zoom},
                        {"y", JsonValue(*viewport, "pageY", 0.0) * zoom},
                        {"width", width * zoom},
                        {"height", height * zoom},
                        {"scale", 1 / zoom}}}},
                     page_session_);
    const json* taken = JsonObject(shot, "result");
    if (!taken) {
      return {{"error",
               CdError(shot).empty() ? "screenshot failed" : CdError(shot)}};
    }
    picture = JsonValue(*taken, "data", "");
  }
  json page = Call(
      "Runtime.evaluate",
      {{"expression", PageScript() + "(0,\"\",true)"}, {"returnByValue", true}},
      page_session_);
  json details = json::object();
  if (const json* response = JsonObject(page, "result")) {
    if (const json* remote = JsonObject(*response, "result")) {
      if (const json* value = JsonObject(*remote, "value")) details = *value;
    }
  }
  observation_ = RandomToken(12);
  view_width_ = static_cast<int>(width);
  view_height_ = static_cast<int>(height);
  return {{"ok", true},
          {"image", std::move(picture)},
          {"page", details},
          {"generation", generation_},
          {"view_id", observation_},
          {"width", view_width_},
          {"height", view_height_}};
}
}  // namespace uagent::browser
