// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/api/citations.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/markdown.h"

namespace uagent {

std::string AttachmentDeliveryRows(const json& deliveries) {
  std::string rows;
  if (!deliveries.is_array()) return rows;
  for (const json& delivery : deliveries) {
    if (!delivery.is_object()) continue;
    std::string name = JsonValue(delivery, "name", "");
    std::string kind = JsonValue(delivery, "delivery", "");
    if (name.empty() && kind.empty()) continue;
    if (name.empty()) name = "attachment";
    if (kind.empty()) kind = "attached";
    // Chrome downgrades on non-UTF-8 terminals; the file name is user
    // content and stays byte-preserved.
    rows += std::string(DIM()) + "  " + TerminalSafe(name) +
            AsciiGlyphs(" · ") + TerminalSafe(kind) + RST() + "\n";
  }
  return rows;
}

namespace {

bool PrintSearchReceipt(int64_t searches, const json& annotations, bool details,
                        bool line_open) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (searches <= 0 && sources.empty()) return false;
  if (line_open) printf("\n");
  std::string source_summary = sources.empty()
                                   ? "source details unavailable"
                                   : Plural(sources.size(), "source");
  if (searches > 0) {
    printf("%s%s%s\n", DIM(),
           AsciiGlyphs("  ← web_search ×" + std::to_string(searches) + " · " +
                       source_summary)
               .c_str(),
           RST());
  } else {
    printf("%s%s%s%s\n", DIM(), AsciiGlyphs("  ← ").c_str(),
           source_summary.c_str(), RST());
  }
  if (!details) return true;
  for (const CitationEntry& source : sources) {
    const std::string& label = source.title.empty() ? source.url : source.title;
    printf("%s    %s%s%s%s\n", DIM(), TerminalSafe(label).c_str(),
           AsciiGlyphs(" · ").c_str(), TerminalSafe(source.url).c_str(), RST());
    if (!source.content.empty()) {
      printf("%s      %s%s\n", DIM(), TerminalSafe(source.content).c_str(),
             RST());
    }
  }
  return true;
}

void PrintCitationSources(const json& annotations) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (sources.empty()) return;
  printf("\n%sSources:%s\n", DIM(), RST());
  for (const CitationEntry& source : sources) {
    printf("%s- <%s>%s\n", DIM(), TerminalSafe(source.url).c_str(), RST());
  }
}

// A command's output as this row shows it: its own colouring (ls --color, a
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

// Every line of `text` moved `indent` columns right, as details sit under
// their row.
std::string Indented(std::string_view text, size_t indent) {
  const std::string pad(indent, ' ');
  std::string out = pad;
  for (size_t at = 0; at < text.size(); ++at) {
    out += text[at];
    if (text[at] == '\n' && at + 1 < text.size()) out += pad;
  }
  return out;
}

const char* DiffLineStyle(std::string_view line) {
  if (line.starts_with('+')) return GREEN();
  if (line.starts_with('-')) return RED();
  return "";
}

void PrintMessageHeader() {
  // Assistant header is the binary name in ASCII, identical on live turns
  // and --resume replay. Never the bare unicode mark: it renders as a
  // random glyph on dumb PTYs and mismatches the spinner labels. Plain mode
  // names the speaker as it names every other row.
  if (g_plain) printf("uagent:\n");
  if (!g_tty) return;
  printf("%suagent%s\n", BOLD(), RST());
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

// The inverse of PresentationJson: a notice as the event stream carries it.
PresentationRecord PresentationFromJson(const json& value) {
  const std::string status = JsonValue(value, "status", "");
  return {status == "succeeded"   ? PresentationStatus::kSucceeded
          : status == "failed"    ? PresentationStatus::kFailed
          : status == "cancelled" ? PresentationStatus::kCancelled
          : status == "warned"    ? PresentationStatus::kWarned
                                  : PresentationStatus::kNeutral,
          JsonValue(value, "title", ""), JsonValue(value, "minor", false)};
}

// Whether a call's stored status is a failure; a cancellation is not one.
bool Failed(std::string_view status) {
  return status == "failed" || status == "timed_out";
}

// The terminal spelling of a ToolView's input parts, the same vocabulary the
// browser renders: a command after "$", code as-is, fields as "label: value".
std::string InputPartsText(const json& view) {
  std::string text;
  const json* parts = JsonArray(view, "input");
  if (!parts) return text;
  for (const json& part : *parts) {
    const std::string kind = JsonValue(part, "kind", "");
    if (kind == "command") {
      text += "$ " + JsonValue(part, "text", "") + "\n";
    } else if (kind == "code") {
      const std::string label = JsonValue(part, "label", "");
      if (!label.empty()) text += label + ":\n";
      text += JsonValue(part, "text", "") + "\n";
    } else if (const json* rows = JsonArray(part, "rows")) {
      for (const json& row : *rows) {
        if (row.is_array() && row.size() == 2) {
          text += row[0].get<std::string>() + ": " + row[1].get<std::string>() +
                  "\n";
        }
      }
    }
  }
  return text;
}

// "→ Editing src/a.ts": the view's verb and target, as the web row reads
// while the call runs. A call without a verb was rejected, so it shows the
// arguments it sent; a multiline label (a script) is shown whole.
std::string CallRow(const json& block, bool open) {
  const json view = JsonValue(block, "view", json(nullptr));
  const json* activity = JsonObject(block, "activity");
  const std::string label =
      activity ? JsonValue(*activity, "label", "") : std::string();
  const std::string name = JsonValue(block, "name", "tool");
  // A skill is a procedure the rest of the turn follows, so it is worth
  // finding in the scrollback later; ◆ already marks that class of event.
  if (name == "skill" && !label.empty()) {
    return std::string(BOLD()) + RowMark(Mark::kSkill) + TerminalSafe(label) +
           RST() + "\n";
  }
  const json* verb = JsonArray(view, "verb");
  std::string body =
      RowMark(Mark::kCall) + (verb && !verb->empty() && (*verb)[0].is_string()
                                  ? TerminalSafe((*verb)[0].get<std::string>())
                                  : TerminalSafe(name));
  const std::string target = JsonValue(view, "target", "");
  if (label.find('\n') != std::string::npos) {
    body += '\n' + TerminalSafe(label);
  } else if (verb && !target.empty()) {
    body += ' ' + TerminalSafe(FirstLine(target));
  } else if (!label.empty()) {
    body += '(' + TerminalSafe(label) + ')';
  }
  std::string row = StyledBlock(body, BOLD());
  if (const std::string input = InputPartsText(view); open && !input.empty()) {
    row += StyledBlock(Indented(TerminalSafe(input), kDetailIndent), DIM());
  }
  return row;
}

// The output a row holds: all of it, or the start and the end of a long one,
// each marked where it was cut.
std::string ResultText(const json& block) {
  std::string text = JsonValue(block, "text", "");
  if (JsonValue(block, "truncated", false)) {
    text += "\n" + AsciiGlyphs("…") + JsonValue(block, "tail", "");
  }
  return text;
}

// A result in one line: its first line and how much follows, after the
// status of a call that did not succeed.
std::string ResultSummary(const json& block, std::string_view status) {
  const std::string text = JsonValue(block, "text", "");
  std::string summary = text.empty() ? "(empty)" : FirstLine(text);
  const size_t lines = static_cast<size_t>(std::ranges::count(text, '\n')) +
                       (text.empty() || text.back() == '\n' ? 0 : 1);
  // The first line is shown, so the count reports what is elided. A row
  // holds only the start of a long result, so of that it can say only that
  // there is more.
  if (JsonValue(block, "truncated", false)) {
    summary += " … · more than " + FmtCount(static_cast<int64_t>(text.size())) +
               " chars";
  } else if (lines > 1) {
    summary += " … · +" + std::to_string(lines - 1) + " lines · " +
               FmtCount(static_cast<int64_t>(text.size())) + " chars";
  }
  if (Failed(status) || status == "cancelled") {
    summary = std::string(status) + ": " + summary;
  }
  return summary;
}

// What stays visible under a result row: a command's last output lines
// (already printed in full when output is open) and the parts the call
// produced.
std::string ResultExtras(const json& block, bool open) {
  std::string text;
  if (!open && JsonValue(JsonValue(block, "view", json(nullptr)), "output",
                         "") == "tail") {
    std::vector<std::string> lines;
    std::istringstream input(ResultText(block));
    for (std::string line; std::getline(input, line);) {
      if (!Trim(line).empty()) lines.push_back(line);
    }
    constexpr size_t kTailLines = 3;
    for (size_t i = lines.size() > kTailLines ? lines.size() - kTailLines : 0;
         i < lines.size(); ++i) {
      text += std::string(DIM()) +
              Indented(OutputText(lines[i]), kDetailIndent) + RST() + "\n";
    }
  }
  if (const json* parts = JsonArray(block, "parts")) {
    for (const json& part : *parts) {
      const std::string kind = JsonValue(part, "kind", "");
      std::string line;
      if (kind == "file") {
        line = JsonValue(part, "name", "") + " · " +
               FmtBytes(JsonValue(part, "bytes", int64_t{0})) +
               " · open it in the web app";
      } else if (kind == "link") {
        const std::string to = JsonValue(part, "to", "");
        const std::string id = part.contains("id") && part["id"].is_string()
                                   ? part["id"].get<std::string>()
                                   : JsonDump(part.value("id", json()));
        line = to == "agent"      ? "/agents " + id
               : to == "activity" ? "/ps " + id
                                  : "memory " + id;
      }
      if (!line.empty()) {
        text +=
            std::string(DIM()) +
            Indented(AsciiGlyphs("↳ " + TerminalSafe(line)), kDetailIndent) +
            RST() + "\n";
      }
    }
  }
  return text;
}

// A finished call as the level lays it out: its group's row, its diff, or
// its name with the output or a summary of it.
std::string ResultRow(const json& block, const DetailPolicy& detail) {
  const std::string status = JsonValue(block, "status", "");
  const json* activity = JsonObject(block, "activity");
  if (const json* group = activity ? JsonObject(*activity, "group") : nullptr;
      group && detail.work != WorkRows::kCalls) {
    // A run of like calls is one row, drawn where the run starts.
    if (JsonValue(*group, "id", "") != JsonValue(block, "call_id", "")) {
      return "";
    }
    return StyledBlock(
        Indented(TerminalSafe(JsonValue(*group, "label", "")), kRowIndent),
        DIM());
  }
  // A change is told entirely by its diff, so the receipt is the whole row.
  // A long diff is kept whole: both sides, each line a marker longer.
  if (const std::string change = JsonValue(block, "change", "");
      status == "success" && !change.empty()) {
    std::istringstream input(
        ReadFile(JsonValue(block, "change_path", ""), 4 * EditFileBytes())
            .value_or(change));
    std::string line, output;
    if (std::getline(input, line)) {
      output = std::string(DIM()) + RowMark(Mark::kChange) + RST() + BOLD() +
               TerminalSafe(line) + RST() + "\n";
    }
    while (std::getline(input, line)) {
      const char* style = DiffLineStyle(line);
      if (!*style) style = DIM();
      if (!line.empty() && line[0] == '@') line = "@@ " + line.substr(1);
      output += std::string(style) +
                Indented(TerminalSafe(line), kDetailIndent) + RST() + "\n";
    }
    return output;
  }
  const bool failed = Failed(status);
  const char* style = failed ? RED() : status == "cancelled" ? YEL() : DIM();
  const std::string prefix =
      std::string(style) +
      Indented(RowMark(failed ? Mark::kFailed : Mark::kResult) +
                   TerminalSafe(JsonValue(block, "name", "tool")),
               kRowIndent);
  const std::string output = ResultText(block);
  const std::string extras = ResultExtras(block, detail.open);
  if (detail.open && output.find('\n') != std::string::npos) {
    return prefix + RST() + "\n" + Indented(OutputText(output), kDetailIndent) +
           "\n" + extras;
  }
  if (detail.open && !output.empty()) {
    return prefix + ": " + OutputText(output) + RST() + "\n" + extras;
  }
  return prefix + ": " + AsciiGlyphs(OutputText(ResultSummary(block, status))) +
         RST() + "\n" + extras;
}

}  // namespace

std::string ColorizeDiffLines(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  size_t begin = 0;
  while (begin < text.size()) {
    size_t end = text.find('\n', begin);
    bool newline = end != std::string_view::npos;
    if (!newline) end = text.size();
    std::string_view line = text.substr(begin, end - begin);
    const char* style = DiffLineStyle(line);
    if (*style) output += style;
    output += line;
    if (*style) output += RST();
    if (newline) output += '\n';
    begin = end + (newline ? 1 : 0);
  }
  return output;
}

struct TerminalPresenter::State {
  State(const Event& event, bool reasoning)
      : render(event.render), full_reasoning(reasoning) {}

  void Text(std::string_view value) {
    if (!render) return;
    if (!header_printed) {
      PrintMessageHeader();
      header_printed = true;
    }
    if (in_reasoning) {
      markdown.Control(RST());
      if (line_open) markdown.FeedPlain("\n");
      in_reasoning = false;
      line_open = false;
    }
    if (!content_started) {
      markdown.Control(RST());
      content_started = true;
    }
    markdown.Feed(value);
    SetLineOpen(value);
  }

  // A trailing newline closes the row; anything else leaves it open.
  void SetLineOpen(std::string_view value) {
    if (value.empty()) return;
    line_open = value.back() != '\n' && value.back() != '\r';
  }

  void FeedReasoning(std::string_view value, const std::string& style) {
    size_t begin = 0;
    while (begin < value.size()) {
      size_t newline = value.find_first_of("\r\n", begin);
      if (newline == std::string::npos) {
        markdown.FeedPlain(value.substr(begin));
        break;
      }
      size_t end = newline + 1;
      if (value[newline] == '\r' && end < value.size() && value[end] == '\n') {
        ++end;
      }
      markdown.FeedPlain(value.substr(begin, end - begin));
      // The persistent composer resets SGR while painting its status row.
      markdown.Control(style.c_str());
      begin = end;
    }
  }

  void Reasoning(std::string_view value) {
    if (!render) return;
    if (!full_reasoning) return;
    if (!header_printed) {
      PrintMessageHeader();
      header_printed = true;
    }
    std::string style = std::string(RST()) + MUTED() + ITAL();
    if (!in_reasoning) {
      if (content_started && line_open) markdown.FeedPlain("\n");
      markdown.Control(RST());
      markdown.Control(DIM());
      markdown.FeedPlain(RowMark(Mark::kNote) + "Thinking\n");
      line_open = false;
      in_reasoning = true;
    }
    markdown.Control(style.c_str());
    FeedReasoning(value, style);
    SetLineOpen(value);
  }

  void Finish() {
    if (in_reasoning) {
      markdown.Control(RST());
      if (line_open) markdown.FeedPlain("\n");
      line_open = false;
    }
    markdown.Flush();
  }

  bool header_printed = false;
  bool render = false;
  bool full_reasoning = false;
  bool in_reasoning = false;
  bool content_started = false;
  bool line_open = false;
  MdStream markdown;
};

TerminalPresenter::TerminalPresenter() = default;
TerminalPresenter::~TerminalPresenter() { Finish(); }

void TerminalPresenter::Consume(const Event& event) noexcept {
  switch (event.id) {
    case EventId::kTurnStarted:
      Finish();
      // A turn that never completed still owes its row.
      fputs(WorkLine().c_str(), stdout);
      spinner_ = std::make_unique<TerminalSpinner>(false);
      break;
    case EventId::kToolResult:
      if (spinner_) spinner_->Stop();
      break;  // The grouped result presentation follows result bookkeeping.
    case EventId::kResponseSources:
      Finish();
      if (event.render) {
        if (detail_->work == WorkRows::kTurn) {
          ++steps_;
        } else {
          PrintSearchReceipt(JsonValue(event.data, "searches", int64_t{0}),
                             event.data["annotations"], detail_->open,
                             JsonValue(event.data, "line_open", false));
        }
        if (JsonValue(event.data, "citations", false)) {
          PrintCitationSources(event.data["annotations"]);
        }
      }
      break;
    case EventId::kTurnCompleted:
      Finish();
      if (event.render) {
        std::string footer =
            (JsonValue(event.data, "line_open", false) ? "\n" : "") +
            WorkLine() + RST() + DIM() + TurnStatsLine(event.data) + RST() +
            "\n";
        fputs(footer.c_str(), stdout);
      }
      break;
    case EventId::kResponseStarted:
      Finish();
      state_ =
          std::make_unique<State>(event, detail_->reasoning == Thinking::kOpen);
      break;
    case EventId::kReasoningDelta:
      if (state_ && state_->full_reasoning && spinner_) spinner_->Stop();
      if (state_ && JsonValue(event.data, "corrected", false)) {
        state_->Reasoning("\n[Updated provider reasoning]\n");
      }
      if (state_) state_->Reasoning(event.text);
      break;
    case EventId::kAnswerDelta:
      if (spinner_) spinner_->Stop();
      if (state_) state_->Text(event.text);
      break;
    case EventId::kTurnStatus: {
      const std::string phase = JsonValue(event.data, "phase", "idle");
      const bool animate =
          event.render && phase != "idle" && phase != "finishing" &&
          phase != "decision" && phase != "responding" &&
          !(phase == "thinking" && state_ && state_->full_reasoning);
      if (!animate) {
        if (spinner_) spinner_->Stop();
      } else {
        const std::string label =
            TerminalSafe(JsonValue(event.data, "turn_status", "Working"));
        if (!spinner_) spinner_ = std::make_unique<TerminalSpinner>(false);
        spinner_->SetLabel(label);
        spinner_->Start();
      }
      break;
    }
    case EventId::kResponseFinished:
      Finish();
      break;
    default:
      if (const auto& notice = event.presentation;
          event.render && notice && (detail_->minor || !notice->minor)) {
        if (spinner_) spinner_->Stop();
        WriteTerminalRecord(Note(
            notice->status == PresentationStatus::kFailed   ? Tone::kError
            : notice->status == PresentationStatus::kWarned ? Tone::kWarn
                                                            : Tone::kNeutral,
            TerminalSafe(notice->title)));
      }
      break;
  }
}

void TerminalPresenter::Consume(const AppEvent& received) noexcept {
  for (int index = 0; index <= static_cast<int>(EventId::kResponseSources);
       ++index) {
    auto id = static_cast<EventId>(index);
    if (received.type != PolicyFor(id).app_type) continue;
    Event event{id, received.data};
    std::string text = JsonValue(
        received.data,
        received.data.contains("append_text") ? "append_text" : "text", "");
    event.text = text;
    event.render = true;
    if (const json* value = JsonObject(received.data, "presentation")) {
      event.presentation = PresentationFromJson(*value);
    }
    Consume(event);
    break;
  }
}

void TerminalPresenter::Block(const json& block) {
  // A chat participant's pass or wait is shown to nobody.
  if (JsonValue(block, "silent", false)) return;
  const std::string kind = JsonValue(block, "kind", "");
  const std::string text = TerminalSafe(JsonValue(block, "text", ""));
  if (kind == "user" || kind == "attachment") {
    if (kind == "user") WriteTerminalRecord(WorkLine());
    // The view already dropped the "Attached:" path trailer; attachments
    // render as the delivery gallery, like history replay and the web.
    WriteTerminalRecord(
        UserEchoRow(InputPrompt(), text) + "\n" +
        AttachmentDeliveryRows(JsonValue(block, "deliveries", json::array())));
  } else if (kind == "assistant") {
    // Mirror the stored-transcript printer and the live presenter: the mark
    // only prints with text (tool-only turns show rows, never a bare mark)
    // and the answer is line-terminated. Tool rows are blocks of their own.
    const std::string reasoning =
        detail_->reasoning == Thinking::kOpen
            ? TerminalSafe(JsonValue(block, "reasoning", ""))
            : "";
    if (!text.empty() || !reasoning.empty()) PrintMessageHeader();
    if (!reasoning.empty()) {
      // The rows the live presenter drew while the model was thinking.
      WriteTerminalRecord(
          std::string(DIM()) + RowMark(Mark::kNote) + "Thinking" + RST() +
          "\n" +
          StyledBlock(reasoning, (std::string(MUTED()) + ITAL()).c_str()));
    }
    if (!text.empty()) {
      MdPrint(text);
      WriteTerminalRecord("\n");
    }
  } else if (kind == "turn_summary") {
    WriteTerminalRecord(
        WorkLine() +
        TurnStatsLine(JsonValue(block, "summary", json::object())) + "\n");
  } else if (kind == "compaction") {
    // The row the live "compacted" notice drew.
    WriteTerminalRecord(Note(Tone::kNeutral, "compacted"));
  } else if (kind == "activity") {
    const json memory = JsonValue(block, "memory", json::object());
    if (detail_->minor || !JsonValue(memory, "minor", false)) {
      WriteTerminalRecord(Note(Tone::kNeutral, text));
    }
  } else if (kind == "tool_result") {
    ToolRow(block);
  }
}

void TerminalPresenter::ToolRow(const json& block) {
  if (spinner_) spinner_->Stop();
  const std::string id = JsonValue(block, "id", "");
  const std::string status = JsonValue(block, "status", "running");
  // Where a turn's work is one row, what succeeded is counted instead of
  // printed and only a failure keeps its row.
  const bool counted = detail_->work == WorkRows::kTurn;
  // A live row arrives as its call and again once it finished; each half is
  // drawn once, so the scrollback reads as the same row from history does.
  const auto [row, called] = rows_.try_emplace(id, false);
  if (called && !counted) WriteTerminalRecord(CallRow(block, detail_->open));
  if (status == "running" || std::exchange(row->second, true)) return;
  if (counted && !Failed(status)) {
    ++steps_;
    if (const json* activity = JsonObject(block, "activity");
        activity && JsonValue(*activity, "category", "") == "edit") {
      // Files, not edits: two edits of one file are one file.
      const std::string target =
          JsonValue(JsonValue(block, "view", json(nullptr)), "target", "");
      edited_.insert(target.empty() ? id : target);
    }
    return;
  }
  if (const std::string drawn = ResultRow(block, *detail_); !drawn.empty()) {
    WriteTerminalRecord(drawn);
  }
}

std::string TerminalPresenter::WorkLine() {
  rows_.clear();
  if (!steps_) return "";
  std::string line = "Worked · " + Plural(steps_, "step");
  if (!edited_.empty()) {
    line += " · edited " + Plural(edited_.size(), "file");
  }
  steps_ = 0;
  edited_.clear();
  return StyledBlock(Indented(AsciiGlyphs(line), kRowIndent), DIM());
}

void TerminalPresenter::Finish() noexcept {
  if (spinner_) spinner_->Stop();
  if (!state_) return;
  state_->Finish();
  state_.reset();
}

}  // namespace uagent
