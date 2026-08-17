// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"

namespace uagent {

std::string CompactReasoningPreview(const std::string& text, size_t cap) {
  size_t end = text.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) return "";
  size_t newline = text.find_last_of("\r\n", end);
  size_t begin = newline == std::string::npos ? 0 : newline + 1;
  std::string safe = TerminalSafe(text.substr(begin, end - begin + 1));
  std::string plain;
  plain.reserve(std::min(safe.size(), cap * 2));
  bool separator = false;
  for (size_t index = 0; index < safe.size(); ++index) {
    unsigned char c = static_cast<unsigned char>(safe[index]);
    bool left_word =
        index > 0 && isalnum(static_cast<unsigned char>(safe[index - 1]));
    bool right_word = index + 1 < safe.size() &&
                      isalnum(static_cast<unsigned char>(safe[index + 1]));
    bool decoration = c == '`' ||
                      ((c == '*' || c == '_') && !(left_word && right_word)) ||
                      (c == '#' && !left_word);
    bool whitespace = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (decoration || whitespace) {
      separator = !plain.empty();
      continue;
    }
    if (separator && plain.back() != ' ') plain.push_back(' ');
    separator = false;
    plain.push_back(static_cast<char>(c));
  }
  plain = Trim(plain);
  while (plain.starts_with("- ") || plain.starts_with("> ")) {
    plain = Trim(plain.substr(2));
  }
  if (plain.size() <= cap) return plain;
  size_t start = Utf8BoundaryAfter(plain, plain.size() - cap);
  size_t word = plain.find(' ', start);
  if (word != std::string::npos && word + 1 < plain.size()) start = word + 1;
  return Trim(plain.substr(start));
}

struct TerminalPresenter::State {
  explicit State(const Event& event)
      : render(event.render),
        full_reasoning(event.verbose),
        spinner(std::make_unique<TerminalSpinner>(
            event.render, SpinnerLabel(std::string(event.text)),
            event.anchor)) {}

  void BeginOutput() {
    if (spinner) spinner->Stop();
  }

  void Text(std::string_view value) {
    if (!render) return;
    BeginOutput();
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
    if (!value.empty()) {
      line_open = value.back() != '\n' && value.back() != '\r';
    }
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
    if (!full_reasoning) {
      reasoning_tail += value;
      constexpr size_t kTailBytes = 512;
      if (reasoning_tail.size() > kTailBytes) {
        size_t start = Utf8BoundaryAfter(reasoning_tail,
                                         reasoning_tail.size() - kTailBytes);
        reasoning_tail.erase(0, start);
      }
      std::string preview = CompactReasoningPreview(reasoning_tail);
      if (spinner && !preview.empty()) {
        spinner->SetLabel(SpinnerLabel("thinking · " + preview));
      }
      return;
    }

    BeginOutput();
    std::string style = std::string(RST()) + MUTED() + ITAL();
    if (!in_reasoning) {
      if (content_started && line_open) markdown.FeedPlain("\n");
      markdown.Control(RST());
      markdown.Control(DIM());
      markdown.FeedPlain("· thinking\n");
      line_open = false;
      in_reasoning = true;
    }
    markdown.Control(style.c_str());
    FeedReasoning(value, style);
    if (!value.empty()) {
      line_open = value.back() != '\n' && value.back() != '\r';
    }
  }

  void Finish() {
    BeginOutput();
    if (in_reasoning) {
      markdown.Control(RST());
      if (line_open) markdown.FeedPlain("\n");
      line_open = false;
    }
    markdown.Flush();
  }

  bool render = false;
  bool full_reasoning = false;
  bool in_reasoning = false;
  bool content_started = false;
  bool line_open = false;
  std::string reasoning_tail;
  std::unique_ptr<TerminalSpinner> spinner;
  MdStream markdown;
};

TerminalPresenter::TerminalPresenter() = default;
TerminalPresenter::~TerminalPresenter() { Finish(); }

void TerminalPresenter::Consume(const Event& event) noexcept {
  switch (event.id) {
    case EventId::kResponseStarted:
      Finish();
      state_ = std::make_unique<State>(event);
      break;
    case EventId::kReasoningDelta:
      if (state_) state_->Reasoning(event.text);
      break;
    case EventId::kAnswerDelta:
      if (state_) state_->Text(event.text);
      break;
    case EventId::kResponseFinished:
      Finish();
      break;
    default:
      if (event.render && event.presentation) {
        PrintPresentation(*event.presentation);
      }
      break;
  }
}

void TerminalPresenter::Finish() noexcept {
  if (!state_) return;
  state_->Finish();
  state_.reset();
}

void PrintPresentation(const PresentationRecord& record) noexcept {
  if (record.kind == PresentationKind::kToolCall) {
    std::string prefix = "→ " + TerminalSafe(record.title);
    if (record.multiline && !record.detail.empty()) {
      prefix += '\n' + TerminalSafe(record.detail);
    } else if (!record.summary.empty()) {
      std::string shown =
          record.verbatim
              ? TerminalSafe(record.summary)
              : TerminalSummary(record.summary, DisplayWidth(prefix) + 3);
      prefix += '(' + shown + ')';
    }
    printf("%s%s%s\n", CYAN(), prefix.c_str(), RST());
    return;
  }
  if (record.kind != PresentationKind::kToolResult) return;

  if (record.change_display && !record.detail.empty()) {
    std::istringstream input(record.detail);
    std::string line;
    if (!std::getline(input, line)) return;
    printf("%s•%s %s%s%s\n", DIM(), RST(), BOLD(), TerminalSafe(line).c_str(),
           RST());
    while (std::getline(input, line)) {
      const char* style = DIM();
      if (!line.empty() && line[0] == '+') style = GREEN();
      if (!line.empty() && line[0] == '-') style = RED();
      if (!line.empty() && line[0] == '@') line = "@@ " + line.substr(1);
      printf("%s    %s%s\n", style, TerminalSafe(line).c_str(), RST());
    }
    return;
  }

  const char* style = DIM();
  if (record.status == PresentationStatus::kFailed) {
    style = RED();
  } else if (record.status == PresentationStatus::kCancelled) {
    style = YEL();
  }
  std::string prefix = "  ← " + TerminalSafe(record.title);
  if (record.multiline && !record.detail.empty()) {
    printf("%s%s%s\n%s\n", style, prefix.c_str(), RST(),
           TerminalSafe(record.detail).c_str());
    return;
  }
  printf("%s%s: %s%s\n", style, prefix.c_str(),
         TerminalSafe(record.summary).c_str(), RST());
}

}  // namespace uagent
