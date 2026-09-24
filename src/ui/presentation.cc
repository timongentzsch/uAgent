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
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/ui/conversation.h"
#include "include/ui/interactive.h"

namespace uagent {

bool PrintSearchReceipt(int64_t searches, const json& annotations, bool details,
                        bool line_open) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (searches <= 0 && sources.empty()) return false;
  if (line_open) printf("\n");
  std::string source_summary =
      sources.empty() ? "source details unavailable"
                      : std::to_string(sources.size()) + " source" +
                            (sources.size() == 1 ? "" : "s");
  if (searches > 0) {
    printf("%s  ← web_search ×%s · %s%s\n", DIM(),
           std::to_string(searches).c_str(), source_summary.c_str(), RST());
  } else {
    printf("%s  ← %s%s\n", DIM(), source_summary.c_str(), RST());
  }
  if (!details) return true;
  for (const CitationEntry& source : sources) {
    const std::string& label = source.title.empty() ? source.url : source.title;
    printf("%s    %s · %s%s\n", DIM(), TerminalSafe(label).c_str(),
           TerminalSafe(source.url).c_str(), RST());
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

namespace {

// Shared by the poll and plain tool-result ladders. The notice ladder above
// maps kWarned instead of kCancelled and stays separate on purpose.
const char* ResultStyle(PresentationStatus status) {
  if (status == PresentationStatus::kFailed) return RED();
  if (status == PresentationStatus::kCancelled) return YEL();
  return DIM();
}

}  // namespace

const char* DiffLineStyle(std::string_view line) {
  if (line.starts_with('+')) return GREEN();
  if (line.starts_with('-')) return RED();
  return "";
}

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

void PrintMessageHeader() {
  if (!g_tty) return;
  // Assistant header is the binary name in ASCII, identical on live turns
  // and --resume replay. Never the bare unicode mark: it renders as a
  // random glyph on dumb PTYs and mismatches the spinner labels.
  printf("%suagent%s\n", BOLD(), RST());
}

struct TerminalPresenter::State {
  explicit State(const Event& event)
      : render(event.render), full_reasoning(event.verbose) {}

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
      markdown.FeedPlain("· Thinking\n");
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

std::string TurnStatsLine(const json& summary) {
  const json usage = JsonValue(summary, "usage", json::object());
  auto n = [&](const char* key) { return JsonValue(usage, key, int64_t{0}); };
  std::string line = FmtCount(n("input")) + " in";
  for (auto [key, label] : {std::pair{"cache_read", " cached"},
                            std::pair{"cache_write", " cache write"}}) {
    if (n(key)) line += " (+" + FmtCount(n(key)) + label + ")";
  }
  line += " · " + FmtCount(n("output")) + " out";
  if (n("reasoning")) line += " (+" + FmtCount(n("reasoning")) + " reasoning)";
  if (!JsonValue(summary, "usage_reported", true)) line = "usage not reported";
  if (n("web_searches")) {
    line += " · " + FmtCount(n("web_searches")) + " searches";
  }
  if (JsonValue(usage, "cost_reported", false)) {
    line += " · " + FmtCost(JsonValue(usage, "cost", 0.0));
  }
  int64_t tools = JsonValue(summary, "tool_calls", int64_t{0});
  if (tools) line += " · " + FmtCount(tools) + " tools";
  double rate = JsonValue(summary, "tokens_per_second", 0.0);
  if (rate > 0) line += " · " + FmtCount(static_cast<int64_t>(rate)) + " tok/s";
  double first = JsonValue(summary, "ttt_ms", -1.0);
  if (first >= 0) line += " · first " + FmtDuration(first / 1000);
  return AsciiGlyphs(
      line + " · " +
      FmtDuration(JsonValue(summary, "duration_ms", 0.0) / 1000));
}

void TerminalPresenter::Consume(const Event& event) noexcept {
  switch (event.id) {
    case EventId::kTurnStarted:
      Finish();
      spinner_ = std::make_unique<TerminalSpinner>(false);
      break;
    case EventId::kToolResult:
      if (spinner_) spinner_->Stop();
      break;  // The grouped result presentation follows result bookkeeping.
    case EventId::kResponseSources:
      Finish();
      if (event.render) {
        PrintSearchReceipt(JsonValue(event.data, "searches", int64_t{0}),
                           event.data["annotations"], event.verbose,
                           JsonValue(event.data, "line_open", false));
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
            std::string(RST()) + DIM() + TurnStatsLine(event.data) + RST() +
            "\n";
        fputs(footer.c_str(), stdout);
      }
      break;
    case EventId::kResponseStarted:
      Finish();
      state_ = std::make_unique<State>(event);
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
    case EventId::kActivityStatus: {
      const std::string phase = JsonValue(event.data, "phase", "idle");
      const bool animate =
          event.render && phase != "idle" && phase != "finishing" &&
          phase != "decision" && phase != "responding" &&
          !(phase == "thinking" && state_ && state_->full_reasoning);
      if (!animate) {
        if (spinner_) spinner_->Stop();
      } else {
        const std::string label =
            TerminalSafe(JsonValue(event.data, "activity", "Working"));
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
      if (event.render && event.presentation) {
        if (spinner_) spinner_->Stop();
        PrintPresentation(*event.presentation);
      }
      break;
  }
}

void TerminalPresenter::Consume(const AppEvent& received) noexcept {
  for (int index = 0; index <= static_cast<int>(EventId::kPresentation);
       ++index) {
    auto id = static_cast<EventId>(index);
    if (received.type != PolicyFor(id).app_type) continue;
    Event event{id, received.data};
    std::string text = JsonValue(
        received.data,
        received.data.contains("append_text") ? "append_text" : "text", "");
    event.text = text;
    event.render = true;
    event.verbose = JsonValue(received.data, "verbose", false);
    if (const json* value = JsonObject(received.data, "presentation")) {
      PresentationRecord record;
      auto kind = JsonValue(*value, "kind", "");
      record.kind = kind == "tool_call"     ? PresentationKind::kToolCall
                    : kind == "tool_result" ? PresentationKind::kToolResult
                                            : PresentationKind::kNotice;
      auto status = JsonValue(*value, "status", "");
      record.status = status == "succeeded"   ? PresentationStatus::kSucceeded
                      : status == "failed"    ? PresentationStatus::kFailed
                      : status == "cancelled" ? PresentationStatus::kCancelled
                      : status == "warned"    ? PresentationStatus::kWarned
                                              : PresentationStatus::kNeutral;
      record.title = JsonValue(*value, "title", "");
      record.summary = JsonValue(*value, "summary", "");
      record.detail = JsonValue(*value, "detail", "");
      record.change = JsonValue(*value, "change", "");
      record.multiline = JsonValue(*value, "multiline", false);
      record.id = JsonValue(*value, "id", "");
      record.skill = JsonValue(*value, "skill", false);
      record.poll = JsonValue(*value, "poll", false);
      record.activity = JsonValue(*value, "activity", json::object());
      if (const json* artifacts = JsonArray(*value, "artifacts")) {
        for (const auto& artifact : *artifacts) {
          record.artifacts.push_back({JsonValue(artifact, "kind", ""),
                                      JsonValue(artifact, "path", ""),
                                      JsonValue(artifact, "bytes", size_t{0})});
        }
      }
      event.presentation = std::move(record);
    }
    Consume(event);
    break;
  }
}

void TerminalPresenter::Block(const json& block) {
  const std::string kind = JsonValue(block, "kind", "");
  const std::string text = TerminalSafe(JsonValue(block, "text", ""));
  if (kind == "user" || kind == "attachment") {
    // Stored text keeps the "Attached:" path trailer for the model
    // payload; live rows render the delivery gallery instead, like history
    // replay and the web client do.
    const json deliveries = JsonValue(block, "deliveries", json::array());
    const json files = JsonValue(block, "files", json::array());
    const bool attached =
        !deliveries.empty() || (files.is_array() && !files.empty());
    const std::string echo =
        attached
            ? TerminalSafe(StripAttachedTrailer(JsonValue(block, "text", "")))
            : text;
    WriteTerminalRecord(UserEchoRow(InputPrompt(), echo) + "\n" +
                        AttachmentDeliveryRows(deliveries));
  } else if (kind == "assistant") {
    // Mirror the stored-transcript printer and the live presenter: the mark
    // only prints with text (tool-only turns show rows, never a bare mark),
    // the answer is line-terminated, and tool rows replay the exact live
    // record from facts instead of being dropped.
    if (!JsonValue(block, "text", "").empty()) {
      PrintMessageHeader();
      MdPrint(text);
      WriteTerminalRecord("\n");
    }
    if (const json* tools = JsonArray(block, "tools")) {
      for (const json& tool : *tools) {
        const json* replay = JsonObject(tool, "replay");
        if (!replay) continue;
        PresentationRecord record;
        record.kind = PresentationKind::kToolCall;
        record.id = JsonValue(tool, "call_id", "");
        record.activity = JsonValue(tool, "activity", json::object());
        record.title = JsonValue(*replay, "title", "");
        record.summary = JsonValue(*replay, "summary", "");
        record.detail = JsonValue(*replay, "detail", "");
        record.multiline = JsonValue(*replay, "multiline", false);
        record.skill = JsonValue(tool, "name", "") == "skill";
        record.poll = JsonValue(*replay, "poll", false);
        PrintPresentation(record);
      }
    }
  } else if (kind == "turn_summary") {
    WriteTerminalRecord(
        TurnStatsLine(JsonValue(block, "summary", json::object())) + "\n");
  } else if (kind == "compaction") {
    WriteTerminalRecord("Context compacted\n");
  } else if (kind == "activity") {
    WriteTerminalRecord("· " + text + "\n");
  } else if (kind == "tool_result") {
    if (const json* replay = JsonObject(block, "replay")) {
      // Same row the live printer drew: recorded title/summary plus the
      // block's activity (groups), change (diffs) and final status.
      PresentationRecord record;
      record.kind = PresentationKind::kToolResult;
      record.id = JsonValue(block, "call_id", "");
      record.activity = JsonValue(block, "activity", json::object());
      record.title =
          JsonValue(*replay, "title", JsonValue(block, "name", "tool"));
      record.summary = JsonValue(*replay, "summary", "");
      record.change = JsonValue(block, "change", "");
      const std::string status = JsonValue(block, "status", "");
      record.status = status == "success"     ? PresentationStatus::kSucceeded
                      : status == "cancelled" ? PresentationStatus::kCancelled
                      : status == "failed" || status == "timed_out"
                          ? PresentationStatus::kFailed
                          : PresentationStatus::kNeutral;
      PrintPresentation(record);
      return;
    }
    json activity = JsonValue(block, "activity", json::object());
    WriteTerminalRecord(
        TerminalSafe(JsonValue(activity, "label",
                               JsonValue(block, "name", "Activity"))) +
        " · " + JsonValue(block, "status", "") + "\n");
  }
}

void TerminalPresenter::Finish() noexcept {
  if (spinner_) spinner_->Stop();
  if (!state_) return;
  state_->Finish();
  state_.reset();
}

void PrintPresentation(const PresentationRecord& record) noexcept {
  if (record.kind == PresentationKind::kNotice) {
    const char* color = record.status == PresentationStatus::kFailed   ? RED()
                        : record.status == PresentationStatus::kWarned ? YEL()
                                                                       : DIM();
    WriteTerminalRecord(StyledBlock(TerminalSafe(record.title), color));
    return;
  }
  if (record.kind == PresentationKind::kToolCall) {
    // A skill is a procedure the rest of the turn follows, so it is worth
    // finding in the scrollback later; ◆ already marks that class of event.
    if (record.skill && !record.summary.empty()) {
      WriteTerminalRecord(std::string(BOLD()) + AsciiGlyphs("◆ skill ") +
                          TerminalSafe(record.summary) + RST() + "\n");
      return;
    }
    if (record.poll) return;
    std::string category = JsonValue(record.activity, "category", "");
    std::string prefix = category == "explore"  ? "Exploring · "
                         : category == "change" ? "Editing · "
                                                : "";
    std::string body = AsciiGlyphs("→ ") + prefix + TerminalSafe(record.title);
    if (record.multiline && !record.detail.empty()) {
      body += '\n' + TerminalSafe(record.detail);
    } else if (!record.summary.empty()) {
      body += '(' + TerminalSafe(record.summary) + ')';
    }
    WriteTerminalRecord(StyledBlock(body, BOLD()));
    return;
  }
  if (record.kind != PresentationKind::kToolResult) return;

  if (const auto group = record.activity.find("group");
      group != record.activity.end()) {
    if (JsonValue(*group, "id", "") == record.id) {
      WriteTerminalRecord(StyledBlock(JsonValue(*group, "label", ""), DIM()));
    }
    return;
  }

  if (record.poll) {
    const char* style = ResultStyle(record.status);
    WriteTerminalRecord(std::string(style) + AsciiGlyphs("• ") +
                        TerminalSafe(record.summary) + RST() + "\n");
    return;
  }

  if (!record.change.empty()) {
    std::istringstream input(record.change);
    std::string line;
    if (std::getline(input, line)) {
      std::string output = std::string(DIM()) + AsciiGlyphs("•") + RST() + " " +
                           BOLD() + TerminalSafe(line) + RST() + "\n";
      while (std::getline(input, line)) {
        const char* style = DiffLineStyle(line);
        if (!*style) style = DIM();
        if (!line.empty() && line[0] == '@') line = "@@ " + line.substr(1);
        output +=
            std::string(style) + "    " + TerminalSafe(line) + RST() + "\n";
      }
      WriteTerminalRecord(output);
    }
    if (record.summary.empty() && record.detail.empty()) return;
  }

  const char* style = ResultStyle(record.status);
  std::string prefix = AsciiGlyphs("  ← ") + TerminalSafe(record.title);
  if (record.multiline && !record.detail.empty()) {
    WriteTerminalRecord(std::string(style) + prefix + RST() + "\n" +
                        TerminalSafe(record.detail) + "\n");
    return;
  }
  WriteTerminalRecord(std::string(style) + prefix + ": " +
                      TerminalSafe(record.summary) + RST() + "\n");
}

}  // namespace uagent
