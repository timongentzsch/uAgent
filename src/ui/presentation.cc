// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
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

void PrintCitationSources(const json& annotations) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (sources.empty()) return;
  printf("\n%sSources:%s\n", DIM(), RST());
  for (const CitationEntry& source : sources) {
    printf("%s- <%s>%s\n", DIM(), TerminalSafe(source.url).c_str(), RST());
  }
}

const char* DiffLineStyle(std::string_view line) {
  if (line.starts_with('+')) return GREEN();
  if (line.starts_with('-')) return RED();
  return "";
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
  State(bool reasoning, bool spoke)
      : header_printed(spoke), full_reasoning(reasoning) {}

  void Text(std::string_view value) {
    // A reply shows nothing until it has text.
    if (!content_started &&
        value.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      return;
    }
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

  bool header_printed;
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
      spoke_ = false;
      spinner_ = std::make_unique<TerminalSpinner>(false);
      break;
    case EventId::kToolResult:
      if (spinner_) spinner_->Stop();
      break;  // The grouped result presentation follows result bookkeeping.
    case EventId::kResponseSources:
      Finish();
      if (JsonValue(event.data, "citations", false)) {
        PrintCitationSources(event.data["annotations"]);
      }
      break;
    case EventId::kTurnCompleted:
      // The turn's footer is a node of its own.
      Finish();
      break;
    case EventId::kResponseStarted:
      Finish();
      // A reply starts on a line of its own.
      if (std::exchange(line_open_, false)) fputs("\n", stdout);
      // Thinking streams where it is open, at every level of the tree.
      state_ = std::make_unique<State>(detail_->depth == kDepthAll, spoke_);
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
          phase != "idle" && phase != "finishing" && phase != "decision" &&
          phase != "responding" &&
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
      // Routine notices show where every level is open.
      if (const auto& notice = event.presentation;
          notice && (detail_->depth == kDepthAll || !notice->minor)) {
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
    if (const json* value = JsonObject(received.data, "presentation")) {
      event.presentation = PresentationFromJson(*value);
    }
    Consume(event);
    break;
  }
}

void TerminalPresenter::Finish() noexcept {
  if (spinner_) spinner_->Stop();
  if (!state_) return;
  state_->Finish();
  line_open_ = state_->line_open;
  spoke_ = state_->header_printed;
  state_.reset();
}

}  // namespace uagent
