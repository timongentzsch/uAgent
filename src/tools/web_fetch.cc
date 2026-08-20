// Copyright 2026 Timon Gentzsch

#include "include/tools/web_fetch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

// Elements whose content is code or styling rather than text: dropped whole.
constexpr std::array<std::string_view, 3> kDropped = {"script", "style", "svg"};

// Everything else that reads as a line break once the tags are gone. Inline
// elements are absent on purpose, so words are not split across lines.
constexpr std::array<std::string_view, 14> kBlocks = {
    "br", "p",  "div", "li", "tr", "h1", "h2",
    "h3", "h4", "h5",  "h6", "ul", "ol", "table"};

constexpr std::array<std::pair<std::string_view, std::string_view>, 6>
    kEntities = {{{"&amp;", "&"},
                  {"&lt;", "<"},
                  {"&gt;", ">"},
                  {"&quot;", "\""},
                  {"&#39;", "'"},
                  {"&nbsp;", " "}}};

bool Listed(const auto& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// The tag name of `<...>` at `open`, lowercased and without a leading slash.
std::string TagName(const std::string& html, size_t open) {
  size_t at = open + 1;
  if (at < html.size() && html[at] == '/') ++at;
  size_t end = at;
  while (end < html.size() &&
         (isalnum(static_cast<unsigned char>(html[end])) || html[end] == '!')) {
    ++end;
  }
  return AsciiLower(html.substr(at, end - at));
}

// Skip past a `<name ...>...</name>` pair, or past the opening tag alone when
// the document never closes it. The search is case-insensitive in place: a
// page can carry dozens of these, and lowercasing a copy of it for each one
// would cost more than the whole conversion.
size_t SkipElement(const std::string& html, size_t open,
                   std::string_view name) {
  std::string close = "</" + std::string(name);
  auto found =
      std::search(html.begin() + static_cast<ptrdiff_t>(open), html.end(),
                  close.begin(), close.end(), [](char left, char right) {
                    return tolower(static_cast<unsigned char>(left)) ==
                           tolower(static_cast<unsigned char>(right));
                  });
  return html.find('>', found == html.end()
                            ? open
                            : static_cast<size_t>(found - html.begin()));
}

void Decode(std::string& text) {
  for (const auto& [entity, glyph] : kEntities) {
    ReplaceAll(text, std::string(entity), std::string(glyph));
  }
}

// One space per whitespace run, one blank line per newline run, no trailing
// spaces. A page's indentation is markup, not meaning.
std::string Tidy(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  int newlines = 0;
  bool space = false;
  for (char c : text) {
    if (c == '\n') {
      ++newlines;
      space = false;
      continue;
    }
    if (isspace(static_cast<unsigned char>(c))) {
      space = true;
      continue;
    }
    if (!out.empty()) {
      if (newlines > 0) {
        out.append(std::min(newlines, 2), '\n');
      } else if (space) {
        out += ' ';
      }
    }
    newlines = 0;
    space = false;
    out += c;
  }
  return out;
}

// Types this tool can turn into text: markup is converted, the rest already
// reads as text. Anything else is bytes the model cannot use.
bool Textual(const std::string& type) {
  for (std::string_view kind : {"text/", "json", "xml"}) {
    if (type.find(kind) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

std::string HtmlToText(const std::string& html) {
  std::string text;
  text.reserve(html.size() / 2);
  for (size_t at = 0; at < html.size();) {
    if (html[at] != '<') {
      text += html[at++];
      continue;
    }
    if (html.compare(at, 4, "<!--") == 0) {
      size_t end = html.find("-->", at);
      at = end == std::string::npos ? html.size() : end + 3;
      continue;
    }
    std::string name = TagName(html, at);
    size_t end;
    if (Listed(kDropped, name)) {
      end = SkipElement(html, at, name);
    } else {
      if (Listed(kBlocks, name)) text += '\n';
      end = html.find('>', at);
    }
    // An unterminated tag means the rest of the document is markup.
    if (end == std::string::npos) break;
    at = end + 1;
  }
  Decode(text);
  return Tidy(text);
}

Tool WebFetchTool(Api& api) {
  Tool t = MakeTool(
      "web_fetch",
      "Read one http(s) URL as text. Use it when a specific page is the "
      "answer — a search result worth verifying, a doc page, a changelog — "
      "not to crawl. Pages needing a login or scripting need the browser "
      "skill instead.",
      json::parse(R"json({"type":"object","properties":{
          "url":{"type":"string","description":"absolute http or https URL"}},
          "required":["url"]})json"),
      [&api](const json& a, const ToolContext& context) -> ToolResult {
        std::string url = Trim(JsonValue(a, "url", ""));
        std::string scheme = AsciiLower(url.substr(0, url.find(':') + 1));
        if (scheme != "http:" && scheme != "https:") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: web_fetch needs an absolute http(s) URL");
        }
        Api side(api.config);
        WebResponse page = side.GetUrl(
            url, context.RemainingSeconds(api.config.tool_timeout_s),
            static_cast<size_t>(WebFetchBytes()));
        if (AbortRequested()) {
          return ToolCancelled("error: fetch cancelled by user");
        }
        if (!page.error.empty()) {
          return ToolFailure(ToolErrorCode::kRemoteError,
                             "error: web_fetch " + page.error);
        }
        bool html = page.content_type.find("html") != std::string::npos;
        if (!html && !Textual(page.content_type)) {
          return ToolFailure(
              ToolErrorCode::kUnavailable,
              "error: web_fetch cannot read " + (page.content_type.empty()
                                                     ? "this content type"
                                                     : page.content_type));
        }
        // Non-markup arrives readable; reflowing it would only destroy the
        // indentation that carries meaning in JSON, XML and plain text.
        std::string text = html ? HtmlToText(page.body) : page.body;
        if (Trim(text).empty()) {
          return ToolFailure(
              ToolErrorCode::kUnavailable,
              "error: web_fetch found no text at " + TerminalSafe(url));
        }
        std::string head = "[" + TerminalSafe(url);
        if (page.truncated) head += "; truncated at the byte cap";
        return ToolSuccess(head + "]\n" + std::move(text));
      });
  t.capabilities = Capability(ToolCapability::kInspect) |
                   Capability(ToolCapability::kExternal);
  t.needs_approval = [](const json&) { return true; };
  t.parallel_safe = true;
  t.summary = [](const json& a) { return JsonValue(a, "url", ""); };
  return t;
}

}  // namespace uagent
