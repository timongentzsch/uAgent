// Copyright 2026 Timon Gentzsch
// The transcript as the terminal draws it: the host's tree of nodes, open
// to the level's depth below each turn. Every word comes from the node.

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/markdown.h"
#include "include/ui/presentation.h"

namespace uagent {
namespace {
// How many of a body's last lines an open node shows.
constexpr size_t kTailLines = 3;

std::string_view Field(const json& node, const char* key) {
  const std::string* value = JsonStringRef(node, key);
  return value ? std::string_view(*value) : std::string_view();
}

std::string BodyText(const json& node) {
  return JsonValue(JsonValue(node, "body", json::object()), "text", "");
}

// A command's output as a row shows it: its own colouring (ls --color, a
// test runner) is dropped, since TerminalSafe would spell each escape out.
std::string OutputText(std::string_view text) {
  std::string plain;
  plain.reserve(text.size());
  for (size_t at = 0; at < text.size();) {
    if (const size_t span = CsiSpan(text.substr(at))) {
      at += span;
      continue;
    }
    plain += text[at++];
  }
  return TerminalSafe(plain);
}

std::string TurnStatsLine(const json& summary) {
  const json usage = JsonValue(summary, "usage", json::object());
  auto n = [&](const char* key) { return JsonValue(usage, key, int64_t{0}); };
  const std::string line = TokenSplit(UsageFromJson(usage));
  const double rate = JsonValue(summary, "tokens_per_second", 0.0);
  const double first = JsonValue(summary, "ttt_ms", -1.0);
  auto counted = [](int64_t count, const char* unit) {
    return count ? FmtCount(count) + unit : "";
  };
  return AsciiGlyphs(JoinDot(
      {JsonValue(summary, "usage_reported", true) ? line : "usage not reported",
       counted(n("web_searches"), " searches"),
       JsonValue(usage, "cost_reported", false)
           ? FmtCost(JsonValue(usage, "cost", 0.0))
           : "",
       counted(JsonValue(summary, "tool_calls", int64_t{0}), " tools"),
       rate > 0 ? FmtCount(static_cast<int64_t>(rate)) + " tok/s" : "",
       first >= 0 ? "first " + FmtDuration(first / 1000) : "",
       FmtDuration(JsonValue(summary, "duration_ms", 0.0) / 1000)}));
}

// A closed node: "· Ran make · exit 2 · 1.4s", its mark and colour from
// its state. The headline's target is content and keeps its bytes.
std::string Row(const std::string& head, const json& node, size_t level,
                const char* style) {
  const std::string_view state = Field(node, "state");
  const std::string word =
      JsonValue(JsonValue(node, "result", json::object()), "word", "");
  return StyledBlock(
      std::string(kRowIndent * level, ' ') +
          RowMark(state == "running"  ? Mark::kCall
                  : state == "failed" ? Mark::kFailed
                  : state.empty()     ? Mark::kNote
                                      : Mark::kResult) +
          TerminalSafe(head) +
          (word.empty() ? "" : AsciiGlyphs(" · " + TerminalSafe(word))),
      style);
}

// A node's parts, one row each at `pad`.
std::string Parts(const json& node, const std::string& pad, const char* style) {
  std::string rows;
  if (const json* parts = JsonArray(node, "parts")) {
    for (const json& part : *parts) {
      const json id = part.value("id", json());
      const std::string_view kind = Field(part, "kind");
      const std::string line =
          kind == "file"
              ? JoinDot({JsonValue(part, "name", ""),
                         FmtBytes(JsonValue(part, "bytes", int64_t{0}))})
          : kind == "artifact" ? JoinDot({JsonValue(part, "path", ""),
                                          JsonValue(part, "type", "")})
                               : JoinDot({JsonValue(part, "label", ""),
                                          id.is_string() ? id.get<std::string>()
                                                         : JsonDump(id)});
      rows += StyledBlock(pad + AsciiGlyphs("↳ " + TerminalSafe(line)), style);
    }
  }
  return rows;
}

// What an open node adds under its row: the last lines of its body (a long
// body's end, where a command says how it went) and the parts it made.
std::string Opened(const json& node, size_t level, const char* style) {
  std::string rows;
  const std::string pad = std::string(kRowIndent * (level + 1), ' ');
  if (const json* body = JsonObject(node, "body")) {
    std::string text = JsonValue(*body, "text", "");
    const std::string tail = JsonValue(*body, "tail", "");
    const bool cut = JsonValue(*body, "truncated", false);
    if (cut && !tail.empty()) text = tail;
    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
      if (!Trim(line).empty()) lines.push_back(OutputText(line));
    }
    // Only the start of a long body is here: it says there is more.
    if (cut && tail.empty()) lines.push_back(AsciiGlyphs("…"));
    const bool diff = Field(*body, "kind") == "diff";
    for (size_t i = lines.size() > kTailLines ? lines.size() - kTailLines : 0;
         i < lines.size(); ++i) {
      rows += diff
                  ? pad + AsciiGlyphs("│ ") + ColorizeDiffLines(lines[i]) + "\n"
                  : StyledBlock(pad + AsciiGlyphs("│ ") + lines[i], style);
    }
  }
  return rows + Parts(node, pad, style);
}

// One node's own rows: messages and replies whole, a footer as the turn's
// statistics, every other node as its row and, open, what it holds.
void DrawRows(const json& node, size_t level, bool open, bool thinking) {
  const std::string_view kind = Field(node, "kind");
  const std::string pad(kRowIndent * level, ' ');
  const std::string text = TerminalSafe(BodyText(node));
  if (kind == "turn" || (kind == "message" && Field(node, "role") == "user")) {
    if (kind == "message" && text.empty()) return;
    WriteTerminalRecord(
        UserEchoRow(InputPrompt(), text) + "\n" +
        AttachmentDeliveryRows(JsonValue(node, "deliveries", json::array())));
  } else if (kind == "thinking") {
    // A thought reads as text, fainter than a reply: whole where it is open,
    // its first lines where only its work is.
    std::string thought = Trim(text);
    if (thought.empty()) return;
    if (!open) {
      std::ranges::replace(thought, '\n', ' ');
      const size_t width = TerminalWidth(1);
      thought = DisplayTrunc(
          thought,
          kThoughtLines * (width > pad.size() ? width - pad.size() : 1));
    }
    std::string rows;
    std::istringstream input(thought);
    for (std::string line; std::getline(input, line);) {
      rows +=
          line.empty() ? "\n" : pad + MUTED() + ITAL() + line + RST() + "\n";
    }
    WriteTerminalRecord(rows);
  } else if (kind == "message" && level > 1) {
    std::string rows;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
      rows += line.empty() ? "\n" : pad + line + "\n";
    }
    rows += Parts(node, pad, DIM());
    if (!rows.empty()) WriteTerminalRecord(rows);
  } else if (kind == "message") {
    // A reply without text prints no bare speaker row.
    if (!text.empty()) {
      PrintMessageHeader();
      MdPrint(text);
      // Ends the reply's last line; a record would add a blank one.
      fputs("\n", stdout);
    }
    if (const std::string files = Parts(node, pad, DIM()); !files.empty()) {
      WriteTerminalRecord(files);
    }
  } else if (kind == "footer") {
    WriteTerminalRecord(StyledBlock(
        TurnStatsLine(JsonValue(node, "summary", json::object())), DIM()));
  } else {
    const std::string_view state = Field(node, "state");
    const char* style = state == "failed"      ? RED()
                        : state == "cancelled" ? YEL()
                                               : DIM();
    const json headline = JsonValue(node, "headline", json::object());
    const std::string verb = JsonValue(headline, "verb", "");
    const std::string target = FirstLine(JsonValue(headline, "target", ""));
    std::string head =
        verb.empty() || target.empty() ? verb + target : verb + " " + target;
    // A work counts its thoughts only where thinking is shown.
    if (const int64_t thoughts = JsonValue(
            JsonValue(node, "result", json::object()), "thoughts", int64_t{0});
        thinking && thoughts) {
      head += ", " + Plural(thoughts, "thought");
    }
    WriteTerminalRecord(Row(head, node, level, style) +
                        (open ? Opened(node, level, style) : ""));
  }
}
}  // namespace

void PrintMessageHeader() {
  // The header is the binary name in ASCII, the same live and on replay: the
  // unicode mark renders as a random glyph on dumb PTYs. Plain mode names the
  // speaker as it names every other row.
  if (g_plain) printf("uagent:\n");
  if (!g_tty) return;
  printf("%suagent%s\n", BOLD(), RST());
}

void TerminalPresenter::Draw(const json& view) {
  drawn_.clear();
  DrawTree(view, false);
}

void TerminalPresenter::Update(const json& view, const json& changed) {
  // A step's calls, its group and its thinking settle together and are
  // drawn once the step ends, with the turn's work, in the tree's order.
  const std::string_view kind = Field(changed, "kind");
  if (kind == "turn" || kind == "message" || kind == "work" ||
      kind == "footer") {
    DrawTree(view, true);
  }
}

void TerminalPresenter::DrawTree(const json& view, bool live) {
  // A node's children in view order, which is the order they arrived in.
  const json* nodes = JsonArray(view, "nodes");
  if (!nodes) return;
  std::map<std::string_view, std::vector<const json*>> children;
  for (const json& node : *nodes) {
    children[Field(node, "parent")].push_back(&node);
  }
  const int depth = detail_->depth;
  // `level` is how far below a root a node sits; the work replying to a
  // message sits at that message's level. A node is open while its level is
  // within the depth.
  const auto draw = [&](const auto& self, const json& node,
                        size_t level) -> void {
    const std::string_view kind = Field(node, "kind");
    if ((JsonValue(node, "routine", false) && depth != kDepthAll) ||
        (kind == "thinking" && !thinking_)) {
      return;
    }
    const bool open = level <= static_cast<size_t>(depth);
    const std::string_view id = Field(node, "id");
    const bool settled = Field(node, "state") != "running" &&
                         !JsonValue(node, "streaming", false);
    // Live, a reply's text and open thinking were drawn as they streamed,
    // and a notice by its event. Anything still running is drawn as it
    // stands only in a whole drawing.
    const bool streamed =
        live && (kind == "notice" || (kind == "thinking" && open) ||
                 (kind == "message" && Field(node, "role") != "user" &&
                  node.contains("body")));
    if (!drawn_.contains(id) && (settled || !live)) {
      if (!streamed) {
        if (spinner_) spinner_->Stop();
        // A streamed reply may have left its last line open.
        if (std::exchange(line_open_, false)) fputs("\n", stdout);
        DrawRows(node, level, open, thinking_);
      }
      if (settled) drawn_.emplace(id);
    }
    const auto under = children.find(id);
    const bool message = kind == "message";
    if ((!open && !message) || under == children.end()) return;
    for (const json* child : under->second) {
      self(self, *child, message ? level : level + 1);
    }
  };
  if (const auto roots = children.find(std::string_view());
      roots != children.end()) {
    for (const json* root : roots->second) draw(draw, *root, 0);
  }
}

}  // namespace uagent
