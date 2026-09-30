// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/cli.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/editor.h"
#include "include/ui/interactive.h"
#include "include/ui/live_region.h"
#include "src/ui/completion.h"

namespace uagent {

namespace {

// Map the bytes the composer has to show inline and, when asked, report where
// `offset` lands in the mapped text. The mapping is per byte, so mapping the
// prefix and the remainder separately equals mapping the whole string; that
// spares the caller a second full mapping pass just to locate the caret.
std::string DisplayText(std::string_view text, size_t offset = 0,
                        size_t* mapped_offset = nullptr) {
  auto map = [](std::string_view part) {
    std::string mapped(part);
    ReplaceAll(mapped, "\n", AsciiGlyphs("↵"));
    ReplaceAll(mapped, "\t", AsciiGlyphs("⇥"));
    return TerminalSafe(mapped);
  };
  if (mapped_offset == nullptr) return map(text);
  size_t split = std::min(offset, text.size());
  std::string safe = map(text.substr(0, split));
  *mapped_offset = safe.size();
  safe += map(text.substr(split));
  return safe;
}

enum class SequenceAction {
  kHistoryPrevious,
  kHistoryNext,
  kLeft,
  kRight,
  kHome,
  kEnd,
  kBackspace,
  kDeleteForward,
  kKillToEnd,
  kKillToStart,
  kPreviousWord,
  kNextWord,
  kDeletePreviousWord,
  kInsertNewline,
  kComplete,
};

struct SequenceBinding {
  std::string_view sequence;
  SequenceAction action;
};

// Readline's control keys first, then what terminals send for the named keys.
// Ctrl+B, Ctrl+D on an empty draft and Ctrl+X are events, handled in Read.
constexpr SequenceBinding kSequenceBindings[] = {
    {"\x01", SequenceAction::kHome},
    {"\x04", SequenceAction::kDeleteForward},
    {"\x05", SequenceAction::kEnd},
    {"\x06", SequenceAction::kRight},
    {"\x08", SequenceAction::kBackspace},
    {"\x7f", SequenceAction::kBackspace},
    {"\t", SequenceAction::kComplete},
    {"\x0b", SequenceAction::kKillToEnd},
    {"\x0e", SequenceAction::kHistoryNext},
    {"\x10", SequenceAction::kHistoryPrevious},
    {"\x15", SequenceAction::kKillToStart},
    {"\x17", SequenceAction::kDeletePreviousWord},
    {"\x1b[A", SequenceAction::kHistoryPrevious},
    {"\x1bOA", SequenceAction::kHistoryPrevious},
    {"\x1b[B", SequenceAction::kHistoryNext},
    {"\x1bOB", SequenceAction::kHistoryNext},
    {"\x1b[C", SequenceAction::kRight},
    {"\x1bOC", SequenceAction::kRight},
    {"\x1b[D", SequenceAction::kLeft},
    {"\x1bOD", SequenceAction::kLeft},
    {"\x1b[H", SequenceAction::kHome},
    {"\x1b[1~", SequenceAction::kHome},
    {"\x1bOH", SequenceAction::kHome},
    {"\x1b[F", SequenceAction::kEnd},
    {"\x1b[4~", SequenceAction::kEnd},
    {"\x1bOF", SequenceAction::kEnd},
    {"\x1b[3~", SequenceAction::kDeleteForward},
    {"\033b", SequenceAction::kPreviousWord},
    {"\x1b[1;3D", SequenceAction::kPreviousWord},
    {"\x1b[1;5D", SequenceAction::kPreviousWord},
    {"\033f", SequenceAction::kNextWord},
    {"\x1b[1;3C", SequenceAction::kNextWord},
    {"\x1b[1;5C", SequenceAction::kNextWord},
    {"\x1b\x7f", SequenceAction::kDeletePreviousWord},
    {"\x1b\x08", SequenceAction::kDeletePreviousWord},
    // Draft newline: Shift+Enter as terminals spell it, plus Alt+Enter.
    {"\x1b\r", SequenceAction::kInsertNewline},
    {"\x1b\n", SequenceAction::kInsertNewline},
    {"\x1b[13;2u", SequenceAction::kInsertNewline},
};

// One character back/forward from a boundary, over the shared scanners.
size_t PreviousUtf8(const std::string& text, size_t at) {
  return at == 0 ? 0 : Utf8BoundaryBefore(text, at - 1);
}

size_t NextUtf8(const std::string& text, size_t at) {
  return at >= text.size() ? text.size() : Utf8BoundaryAfter(text, at + 1);
}

bool WordSpace(unsigned char ch) { return std::isspace(ch) != 0; }

}  // namespace

RawComposer::RawComposer(const InteractiveOutput& output, LiveRegion& region)
    : output_(output), region_(region), prompt_(InputPrompt()) {}

RawComposer::~RawComposer() { Stop(); }

bool RawComposer::Start() {
  if (active_ || tcgetattr(STDIN_FILENO, &saved_) != 0) return false;
  termios raw = saved_;
  raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
  raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL | INLCR | IGNCR | IXON | ISTRIP);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
  active_ = true;
  // A fatal signal or Ctrl+Z from here on has to hand the terminal back
  // cooked, and only the handler can do that.
  ArmTerminalModes(saved_, raw);
  output_.Write("\033[?2004h");
  return true;
}

void RawComposer::Stop() {
  if (!active_) return;
  DisarmTerminalModes();
  output_.Write("\033[?2004l");
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  active_ = false;
}

void RawComposer::Mount(std::string prompt, std::string initial,
                        bool keep_history) {
  prompt_ = std::move(prompt);
  buffer_ = Utf8Prefix(std::move(initial), kInputBufferBytes);
  cursor_ = buffer_.size();
  keep_history_ = keep_history;
  ResetDraftState();
}

void RawComposer::ResetDraftState() {
  history_index_ = history_.size();
  history_draft_.clear();
  input_limit_bell_ = false;
  selected_ = 0;
}

// Readline's spelling, so the gesture is already in the fingers of anyone who
// edits a long shell command the same way. The composer holds 64 KiB and
// renders newlines as a glyph, which is writable but not somewhere to compose
// a long prompt.
bool RawComposer::EditTextExternally(std::string& text) {
  region_.Clear();
  Stop();
  const bool edited =
      EditExternalText(text, output_.TerminalFd(), kAdaptiveSystemBytes);
  Start();
  return edited;
}

bool RawComposer::EditExternally() {
  auto text = buffer_;
  const bool edited = EditTextExternally(text);
  if (edited) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }
    buffer_ = Utf8Prefix(std::move(text), kInputBufferBytes);
    cursor_ = buffer_.size();
  }
  return edited;
}

void RawComposer::Clear() {
  buffer_.clear();
  cursor_ = 0;
  ResetDraftState();
}

InteractiveInputEvent RawComposer::Read() {
  unsigned char bytes[4096];
  for (;;) {
    ssize_t count = read(STDIN_FILENO, bytes, sizeof bytes);
    if (count > 0) {
      decoder_.Feed(bytes, static_cast<size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  bool pasted_in_batch = false;
  while (std::optional<TerminalInputToken> token = decoder_.Next()) {
    note_.clear();
    if (token->kind == TerminalInputTokenKind::kEscape) {
      return {InteractiveInputKind::kEscape, buffer_};
    }
    if (token->kind == TerminalInputTokenKind::kSequence) {
      ApplySequence(token->text);
      continue;
    }
    if (token->kind == TerminalInputTokenKind::kPaste) {
      pasted_in_batch = true;
      if (token->overflow || !Insert(token->text)) {
        output_.Write("\a");
        note_ = "paste not inserted: the draft holds " +
                std::to_string(kInputBufferBytes / 1024) + " KiB";
      }
      continue;
    }
    unsigned char ch = static_cast<unsigned char>(token->text[0]);
    // A terminal may append Enter to a bracketed paste in the same input
    // batch. Preserve it as pasted text instead of submitting unexpectedly.
    if ((ch == '\r' || ch == '\n') && pasted_in_batch) {
      if (!Insert("\n")) output_.Write("\a");
      continue;
    }
    // Both spellings submit: a piped script sends the bare newline. With the
    // menu open, Enter takes the highlighted command unless the draft already
    // names one exactly.
    if ((ch == '\r' || ch == '\n') && !ParseSlashCommand(buffer_).spec &&
        CompletionMatches(buffer_, cursor_).menu) {
      Complete();
      continue;
    }
    if (ch == '\r' || ch == '\n') {
      std::string line = std::move(buffer_);
      buffer_.clear();
      cursor_ = 0;
      if (keep_history_ && ShouldRememberInput(line)) {
        history_.push_back(line);
        if (history_.size() > kInputHistoryEntries) history_.pop_front();
      }
      ResetDraftState();
      return {InteractiveInputKind::kLine, std::move(line)};
    }
    if (ch == 0x02) {
      return {InteractiveInputKind::kBackground, buffer_};
    }
    if (ch == 0x04 && buffer_.empty()) return {InteractiveInputKind::kEof, {}};
    if (editor_prefix_) {
      editor_prefix_ = false;
      if (ch == 0x05) {  // Ctrl+X Ctrl+E, readline's spelling
        if (!EditExternally()) output_.Write("\a");
        continue;
      }
    }
    if (ch == 0x18) {  // Ctrl+X, a prefix on its own
      editor_prefix_ = true;
      continue;
    }
    if (ApplySequence(token->text)) continue;
    if (ch >= 0x20 && !Insert(std::string(1, static_cast<char>(ch))) &&
        !input_limit_bell_) {
      output_.Write("\a");
      input_limit_bell_ = true;
    }
  }
  return {};
}

size_t RawComposer::AvailableColumns() const {
  return TerminalWidth(static_cast<int64_t>(DisplayWidth(prompt_)) + 1);
}

RawComposer::Layout RawComposer::View() const {
  size_t mapped_cursor = 0;
  std::string safe = DisplayText(buffer_, cursor_, &mapped_cursor);
  std::vector<std::string> rows = WrapLines(safe, AvailableColumns());
  if (rows.empty()) rows = {""};

  size_t before_width = DisplayWidth(safe.substr(0, mapped_cursor));
  size_t row = 0;
  while (row + 1 < rows.size()) {
    size_t row_width = DisplayWidth(rows[row]);
    if (before_width <= row_width) break;
    before_width -= row_width;
    ++row;
  }
  size_t caret_col = std::min(before_width, DisplayWidth(rows[row]));
  if (row == 0) caret_col += DisplayWidth(prompt_);
  rows[0] = prompt_ + rows[0];
  // Below the draft, inside the erased block. Plain text: these rows are
  // measured, and an SGR escape is not width.
  Suggestions found = CompletionMatches(buffer_, cursor_);
  constexpr size_t kShownMatches = 5;
  // The window scrolls to keep the highlighted row in view.
  const size_t selected =
      std::min(selected_, std::max<size_t>(found.matches.size(), 1) - 1);
  const size_t first =
      selected < kShownMatches ? 0 : selected + 1 - kShownMatches;
  for (size_t index = first;
       index < found.matches.size() && index < first + kShownMatches; ++index) {
    const Suggestion& match = found.matches[index];
    std::string suggestion =
        (found.menu && index == selected ? "> " : "  ") + match.name;
    if (!match.description.empty()) suggestion += "  " + match.description;
    rows.push_back(DisplayTrunc(TerminalSafe(suggestion), AvailableColumns()));
  }
  if (!note_.empty()) {
    rows.push_back(DisplayTrunc("  " + note_, AvailableColumns()));
  }
  return {std::move(rows), row, caret_col};
}

bool RawComposer::Insert(const std::string& text) {
  if (text.size() >
      kInputBufferBytes - std::min(buffer_.size(), kInputBufferBytes)) {
    return false;
  }
  buffer_.insert(cursor_, text);
  cursor_ += text.size();
  input_limit_bell_ = false;
  selected_ = 0;
  return true;
}

void RawComposer::Backspace() {
  size_t previous = PreviousUtf8(buffer_, cursor_);
  buffer_.erase(previous, cursor_ - previous);
  cursor_ = previous;
  input_limit_bell_ = false;
}

void RawComposer::PreviousWord() {
  while (cursor_ > 0) {
    size_t previous = PreviousUtf8(buffer_, cursor_);
    if (!WordSpace(static_cast<unsigned char>(buffer_[previous]))) break;
    cursor_ = previous;
  }
  while (cursor_ > 0) {
    size_t previous = PreviousUtf8(buffer_, cursor_);
    if (WordSpace(static_cast<unsigned char>(buffer_[previous]))) break;
    cursor_ = previous;
  }
}

void RawComposer::NextWord() {
  while (cursor_ < buffer_.size() &&
         !WordSpace(static_cast<unsigned char>(buffer_[cursor_]))) {
    cursor_ = NextUtf8(buffer_, cursor_);
  }
  while (cursor_ < buffer_.size() &&
         WordSpace(static_cast<unsigned char>(buffer_[cursor_]))) {
    cursor_ = NextUtf8(buffer_, cursor_);
  }
}

void RawComposer::DeletePreviousWord() {
  size_t end = cursor_;
  PreviousWord();
  buffer_.erase(cursor_, end - cursor_);
  input_limit_bell_ = false;
}

// Completes a command or a path, else inserts a plain tab.
void RawComposer::Complete() {
  Suggestions found = CompletionMatches(buffer_, cursor_);
  if (found.matches.empty()) {
    Insert("\t");
    return;
  }
  if (found.menu) {
    const Suggestion& pick =
        found.matches[std::min(selected_, found.matches.size() - 1)];
    buffer_ = pick.name + (pick.wants_argument ? " " : "");
    cursor_ = buffer_.size();
    selected_ = 0;
    return;
  }
  // The longest prefix every candidate agrees on: one Tab commits what is
  // certain, and the rows below the draft show what is still open.
  std::string name = found.matches.front().name;
  for (const Suggestion& match : found.matches) {
    name.resize(
        static_cast<size_t>(std::mismatch(name.begin(), name.end(),
                                          match.name.begin(), match.name.end())
                                .first -
                            name.begin()));
  }
  if (found.matches.size() == 1 && found.matches.front().wants_argument) {
    name += " ";
  }
  buffer_.replace(found.begin, found.end - found.begin, name);
  cursor_ = found.begin + name.size();
}

bool RawComposer::Select(int direction) {
  // Browsing history keeps going through history even past a recalled
  // command.
  if (history_index_ != history_.size()) return false;
  const Suggestions found = CompletionMatches(buffer_, cursor_);
  const size_t count = found.matches.size();
  if (!found.menu || count == 0) return false;
  selected_ =
      (std::min(selected_, count - 1) + (direction < 0 ? count - 1 : 1)) %
      count;
  return true;
}

bool RawComposer::ApplySequence(const std::string& sequence) {
  for (const SequenceBinding& binding : kSequenceBindings) {
    if (binding.sequence != sequence) continue;
    if (binding.action != SequenceAction::kHistoryPrevious &&
        binding.action != SequenceAction::kHistoryNext) {
      selected_ = 0;
    }
    switch (binding.action) {
      case SequenceAction::kHistoryPrevious:
        if (!Select(-1)) History(-1);
        break;
      case SequenceAction::kHistoryNext:
        if (!Select(1)) History(1);
        break;
      case SequenceAction::kLeft:
        cursor_ = PreviousUtf8(buffer_, cursor_);
        break;
      case SequenceAction::kRight:
        cursor_ = NextUtf8(buffer_, cursor_);
        break;
      case SequenceAction::kHome:
        cursor_ = 0;
        break;
      case SequenceAction::kEnd:
        cursor_ = buffer_.size();
        break;
      case SequenceAction::kBackspace:
        Backspace();
        break;
      case SequenceAction::kKillToEnd:
        buffer_.erase(cursor_);
        break;
      case SequenceAction::kKillToStart:
        buffer_.erase(0, cursor_);
        cursor_ = 0;
        break;
      case SequenceAction::kComplete:
        Complete();
        break;
      case SequenceAction::kDeleteForward:
        if (cursor_ < buffer_.size()) {
          buffer_.erase(cursor_, NextUtf8(buffer_, cursor_) - cursor_);
        }
        break;
      case SequenceAction::kInsertNewline:
        if (!Insert("\n")) output_.Write("\a");
        break;
      case SequenceAction::kPreviousWord:
        PreviousWord();
        break;
      case SequenceAction::kNextWord:
        NextWord();
        break;
      case SequenceAction::kDeletePreviousWord:
        DeletePreviousWord();
        break;
    }
    return true;
  }
  return false;
}

void RawComposer::History(int direction) {
  if (history_.empty()) return;
  if (direction > 0 && history_index_ == history_.size()) return;
  if (direction < 0 && history_index_ == history_.size()) {
    history_draft_ = buffer_;
  }
  if (direction < 0 && history_index_ > 0) --history_index_;
  if (direction > 0 && history_index_ < history_.size()) ++history_index_;
  buffer_ = history_index_ < history_.size() ? history_[history_index_]
                                             : history_draft_;
  cursor_ = buffer_.size();
}

}  // namespace uagent
