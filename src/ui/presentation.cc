// Copyright 2026 Timon Gentzsch

#include "include/ui/presentation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/ui/interactive.h"

namespace uagent {

namespace {

struct PollAnchor {
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point seen;
};

// An activity can also vanish without a final poll, by completing in the
// background or being stopped, so anchors expire instead of relying on every
// such path to announce itself.
constexpr std::chrono::hours kPollAnchorTtl{1};

std::unordered_map<int64_t, PollAnchor>& PollAnchors() {
  static std::unordered_map<int64_t, PollAnchor> anchors;
  return anchors;
}

}  // namespace

std::chrono::steady_clock::duration PollElapsed(int64_t activity_id) {
  auto now = std::chrono::steady_clock::now();
  auto& anchors = PollAnchors();
  auto [it, inserted] = anchors.try_emplace(activity_id, PollAnchor{now, now});
  it->second.seen = now;
  auto elapsed = now - it->second.started;
  if (inserted) {
    std::erase_if(anchors, [now](const auto& entry) {
      return entry.second.seen + kPollAnchorTtl < now;
    });
  }
  return elapsed;
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

void ClearPollAnchor(int64_t activity_id) { PollAnchors().erase(activity_id); }

std::string StripDisplayMarkdown(const std::string& text) {
  std::string safe = TerminalSafe(text);
  std::string plain;
  plain.reserve(safe.size());
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
  return plain;
}

// Bounded single-line rolling buffer for the live reasoning ticker: collapse
// newlines to spaces so the status row never wraps, keep UTF-8 boundaries.
void AppendRolling(std::string& buffer, std::string_view value) {
  for (char c : value) {
    if (c == '\r' || c == '\n') c = ' ';
    buffer.push_back(c);
  }
  constexpr size_t kRollingBytes = 512;
  if (buffer.size() > kRollingBytes) {
    size_t start = Utf8BoundaryAfter(buffer, buffer.size() - kRollingBytes);
    buffer.erase(0, start);
  }
}

// What the row says while the provider runs a search of its own. Ephemeral by
// design: several searches a turn would otherwise each leave a permanent line
// in the scrollback for something the user did not ask to see individually.
constexpr const char* kSearchingActivity = "searching the web";

struct TerminalPresenter::State {
  explicit State(const Event& event)
      : render(event.render),
        full_reasoning(event.verbose),
        base_label(SpinnerLabel(std::string(event.text))),
        spinner(std::make_unique<TerminalSpinner>(event.render, base_label,
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
    if (!full_reasoning) {
      if (!spinner) return;
      AppendRolling(reasoning_tail, value);
      // A provider-side search owns the row for as long as it runs. The buffer
      // still grows underneath, so the ticker resumes at the live edge rather
      // than replaying what was thought during the wait.
      if (!active_searches.empty()) return;
      // Strip on the whole buffer, not per delta: decoration detection needs
      // the neighbouring characters, which a chunk boundary would split. The
      // renderer applies it, so that whole-buffer pass runs once per drawn
      // frame instead of once per streamed token — an order of magnitude
      // apart — while the ticker still shows the newest text.
      spinner->SetRolling("thinking · ", reasoning_tail, StripDisplayMarkdown);
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
    SetLineOpen(value);
  }

  // A tool the provider ran. It is response activity, not a tool row: routing
  // it through the tool presentation would claim this agent executed a search,
  // and would put an approval-shaped record in the scrollback for one it never
  // could have declined.
  void HostedTool(const json& data) {
    if (!render || !spinner) return;
    const std::string phase = JsonValue(data, "phase", "");
    const bool running = phase == "started" || phase == "searching";
    // Erasing an id that never started covers a completion with no matching
    // start, which otherwise strands the row on "searching the web".
    if (running) {
      active_searches.insert(JsonValue(data, "id", ""));
    } else {
      active_searches.erase(JsonValue(data, "id", ""));
    }
    if (!active_searches.empty()) {
      spinner->SetLabel(kSearchingActivity);
    } else if (!reasoning_tail.empty()) {
      spinner->SetRolling("thinking · ", reasoning_tail, StripDisplayMarkdown);
    } else {
      spinner->SetLabel(base_label);
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
  std::string base_label;
  // Concurrent searches share one label; the last to finish hands the row back.
  std::set<std::string> active_searches;
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
    case EventId::kHostedToolActivity:
      if (state_) state_->HostedTool(event.data);
      break;
    case EventId::kResponseFinished:
      Finish();
      break;
    default:
      if (event.render && event.presentation) {
        if (state_) state_->BeginOutput();
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
      WriteTerminalRecord(std::string(BOLD()) + BLUE() + "◆ skill " +
                          TerminalSafe(record.summary) + RST() + "\n");
      return;
    }
    if (record.poll) return;
    std::string body = "→ " + TerminalSafe(record.title);
    if (record.multiline && !record.detail.empty()) {
      body += '\n' + TerminalSafe(record.detail);
    } else if (!record.summary.empty()) {
      body += '(' + TerminalSafe(record.summary) + ')';
    }
    WriteTerminalRecord(StyledBlock(body, CYAN()));
    return;
  }
  if (record.kind != PresentationKind::kToolResult) return;

  if (record.poll) {
    const char* style = ResultStyle(record.status);
    WriteTerminalRecord(std::string(style) + "• " +
                        TerminalSafe(record.summary) + RST() + "\n");
    return;
  }

  if (!record.change.empty()) {
    std::istringstream input(record.change);
    std::string line;
    if (std::getline(input, line)) {
      std::string output = std::string(DIM()) + "•" + RST() + " " + BOLD() +
                           TerminalSafe(line) + RST() + "\n";
      while (std::getline(input, line)) {
        const char* style = DIM();
        if (!line.empty() && line[0] == '+') style = GREEN();
        if (!line.empty() && line[0] == '-') style = RED();
        if (!line.empty() && line[0] == '@') line = "@@ " + line.substr(1);
        output +=
            std::string(style) + "    " + TerminalSafe(line) + RST() + "\n";
      }
      WriteTerminalRecord(output);
    }
    // A change that also produced output (a script that was written and then
    // run) still owes the person that output, so only a bare receipt ends here.
    if (record.detail.empty() && record.summary.empty()) return;
  }

  const char* style = ResultStyle(record.status);
  std::string prefix = "  ← " + TerminalSafe(record.title);
  if (record.multiline && !record.detail.empty()) {
    WriteTerminalRecord(std::string(style) + prefix + RST() + "\n" +
                        TerminalSafe(record.detail) + "\n");
    return;
  }
  WriteTerminalRecord(std::string(style) + prefix + ": " +
                      TerminalSafe(record.summary) + RST() + "\n");
}

}  // namespace uagent
