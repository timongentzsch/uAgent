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

// Elements whose content is code, styling or site furniture rather than the
// text a reader came for: dropped whole.
constexpr std::array<std::string_view, 6> kDropped = {
    "script", "style", "svg", "nav", "footer", "aside"};

// Indentation inside these is the meaning, not markup, so their text is kept
// exactly as written.
constexpr std::array<std::string_view, 1> kPre = {"pre"};

// Table cells read as columns rather than lines: without a separator the
// figureseither side of a boundary would run together into one number.
constexpr std::array<std::string_view, 2> kCells = {"td", "th"};

// Everything else that reads as a line break once the tags are gone. Inline
// elements are absent on purpose, so words are not split across lines.
constexpr std::array<std::string_view, 14> kBlocks = {
    "br", "p",  "div", "li", "tr", "h1", "h2",
    "h3", "h4", "h5",  "h6", "ul", "ol", "table"};

constexpr std::array<std::pair<std::string_view, char>, 6> kEntities = {
    {{"&amp;", '&'},
     {"&lt;", '<'},
     {"&gt;", '>'},
     {"&quot;", '"'},
     {"&#39;", '\''},
     {"&nbsp;", ' '}}};

// Decodes the entity starting at `at` into `c`, advancing past it. Shared so
// tidied and verbatim text agree on what an entity means.
bool DecodeEntity(const std::string& text, size_t& at, char& c) {
  const auto* entity = std::find_if(
      kEntities.begin(), kEntities.end(), [&](const auto& candidate) {
        return text.compare(at, candidate.first.size(), candidate.first) == 0;
      });
  if (entity == kEntities.end()) return false;
  c = entity->second;
  at += entity->first.size() - 1;
  return true;
}

bool Listed(const auto& names, std::string_view name) {
  return std::any_of(names.begin(), names.end(), [&](std::string_view listed) {
    return name.size() == listed.size() &&
           std::equal(name.begin(), name.end(), listed.begin(),
                      [](char left, char lowered) {
                        return tolower(static_cast<unsigned char>(left)) ==
                               lowered;
                      });
  });
}

// The tag name of `<...>` at `open`, without a leading slash; case folds at
// comparison time so no lowercased copy is made.
std::string_view TagName(const std::string& html, size_t open) {
  size_t at = open + 1;
  if (at < html.size() && html[at] == '/') ++at;
  size_t end = at;
  while (end < html.size() &&
         (isalnum(static_cast<unsigned char>(html[end])) || html[end] == '!')) {
    ++end;
  }
  return std::string_view(html).substr(at, end - at);
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

// One space per whitespace run, one blank line per newline run, no trailing
// spaces, entities decoded in the same pass. A page's indentation is markup,
// not meaning.
std::string Tidy(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  int newlines = 0;
  bool space = false;
  for (size_t at = 0; at < text.size(); ++at) {
    char c = text[at];
    if (c == '&') DecodeEntity(text, at, c);
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

// Entities decoded, every other byte kept: what <pre> content needs.
std::string Verbatim(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at < text.size(); ++at) {
    char c = text[at];
    if (c == '&') DecodeEntity(text, at, c);
    out += c;
  }
  return out;
}

// The `>` that closes the tag opened at `open`, ignoring any inside a quoted
// attribute value. Pages embed JSON in attributes, and stopping at the first
// `>` would spill the remainder of it into the text as markup.
size_t TagEnd(const std::string& html, size_t open) {
  char quote = '\0';
  for (size_t at = open + 1; at < html.size(); ++at) {
    char c = html[at];
    if (quote != '\0') {
      if (c == quote) quote = '\0';
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return at;
    }
  }
  return std::string::npos;
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
  std::string out;
  std::string text;
  out.reserve(html.size() / 2);
  // Prose is collapsed; <pre> is not, so the pending run is flushed under
  // whichever rule was in force while it was collected.
  bool preformatted = false;
  auto flush = [&] {
    out += preformatted ? Verbatim(text) : Tidy(text);
    text.clear();
  };
  // Tidy trims each run it is given, so the break around a <pre> block has to
  // be written between runs rather than inside one.
  auto boundary = [&] {
    if (!out.empty()) out += "\n\n";
  };
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
    std::string_view name = TagName(html, at);
    bool closing = at + 1 < html.size() && html[at + 1] == '/';
    size_t end;
    if (Listed(kDropped, name)) {
      end = SkipElement(html, at, name);
    } else {
      bool heading = name.size() == 2 && (name[0] | 0x20) == 'h' &&
                     name[1] >= '1' && name[1] <= '6';
      if (Listed(kPre, name)) {
        flush();
        preformatted = !closing;
        boundary();
      } else if (!closing && heading) {
        // Markdown depth, so a reader and a model both see the outline.
        text += '\n';
        text.append(static_cast<size_t>(name[1] - '0'), '#');
        text += ' ';
      } else if (!closing && Listed(kCells, name)) {
        text += " | ";
      } else if (Listed(kBlocks, name)) {
        text += '\n';
      }
      end = TagEnd(html, at);
    }
    // An unterminated tag means the rest of the document is markup.
    if (end == std::string::npos) break;
    at = end + 1;
  }
  flush();
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
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
          // A document is not a dead end: this tool only turns markup into
          // text, while the model reads PDFs and images directly once the
          // bytes are on disk.
          return ToolFailure(
              ToolErrorCode::kUnavailable,
              "error: web_fetch cannot read " +
                  (page.content_type.empty() ? "this content type"
                                             : page.content_type) +
                  "; download it with run and read it with attach");
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
