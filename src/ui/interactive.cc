// Copyright 2026 Timon Gentzsch

#include "include/ui/interactive.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/cli.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
#include "include/core/term.h"

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
    ReplaceAll(mapped, "\n", "↵");
    ReplaceAll(mapped, "\t", "⇥");
    return TerminalSafe(mapped);
  };
  if (mapped_offset == nullptr) return map(text);
  size_t split = std::min(offset, text.size());
  std::string safe = map(text.substr(0, split));
  *mapped_offset = safe.size();
  safe += map(text.substr(split));
  return safe;
}

// Cursor movement and erasure append into one buffer so a redraw reaches the
// terminal as a single write instead of a partially applied frame.
void AppendMoveToTop(std::string& out, size_t caret_row) {
  out += "\r";
  if (caret_row > 0) out += "\033[" + std::to_string(caret_row) + "A";
}

void AppendEraseRows(std::string& out, size_t rows) {
  for (size_t row = 0; row < rows; ++row) {
    out += "\r\033[2K";
    if (row + 1 < rows) out += "\033[1B";
  }
  if (rows > 1) out += "\033[" + std::to_string(rows - 1) + "A";
  out += "\r";
}

enum class SequenceAction {
  kHistoryPrevious,
  kHistoryNext,
  kLeft,
  kRight,
  kHome,
  kEnd,
  kDeleteForward,
  kPreviousWord,
  kNextWord,
  kDeletePreviousWord,
};

struct SequenceBinding {
  std::string_view sequence;
  SequenceAction action;
};

constexpr SequenceBinding kSequenceBindings[] = {
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

InteractiveOutput::~InteractiveOutput() { Stop(); }

bool InteractiveOutput::Start() {
  int descriptors[2];
  if (pipe(descriptors) != 0) return false;
  saved_ = dup(STDOUT_FILENO);
  if (saved_ < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  fcntl(saved_, F_SETFD, FD_CLOEXEC);
  read_ = descriptors[0];
  fcntl(read_, F_SETFL, fcntl(read_, F_GETFL) | O_NONBLOCK);
  if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    close(saved_);
    read_ = saved_ = -1;
    return false;
  }
  close(descriptors[1]);
  // Buffered, deliberately: unbuffered turned every putchar of the streaming
  // markdown renderer into its own write(2) and left the renderer's byte/time
  // flush governor with nothing to govern. Buffering hands that governor the
  // real flush control. Line buffering rather than fully buffered, because
  // callers outside this layer print notices with plain printf and rely on
  // them reaching the pipe when the line ends; only the mid-line streaming
  // path (which flushes on its own budget) and Stop() need explicit flushes.
  static char buffer[64 * 1024];
  setvbuf(stdout, buffer, _IOLBF, sizeof buffer);
  return true;
}

void InteractiveOutput::Stop() {
  if (saved_ < 0) return;
  fflush(stdout);
  dup2(saved_, STDOUT_FILENO);
  close(saved_);
  saved_ = -1;
  if (read_ >= 0) {
    close(read_);
    read_ = -1;
  }
}

std::string InteractiveOutput::Read() const {
  std::string out;
  char buffer[8192];
  for (;;) {
    ssize_t count = read(read_, buffer, sizeof buffer);
    if (count > 0) {
      out.append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  return out;
}

void InteractiveOutput::Write(const std::string& text) const {
  (void)WriteAll(saved_, text.data(), text.size());
}

RawComposer::RawComposer(const InteractiveOutput& output)
    : output_(output), prompt_(InputPrompt()) {}

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
  output_.Write("\033[?2004h");
  return true;
}

void RawComposer::Stop() {
  if (!active_) return;
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
  history_index_ = history_.size();
  history_draft_.clear();
  input_limit_bell_ = false;
  Detach();
  RenderFromTop();
}

void RawComposer::Remount() {
  Detach();
  RenderFromTop();
}

void RawComposer::Detach() {
  drawn_rows_ = 0;
  caret_row_ = 0;
  caret_column_ = 0;
}

void RawComposer::Clear() {
  buffer_.clear();
  cursor_ = 0;
  history_index_ = history_.size();
  history_draft_.clear();
  input_limit_bell_ = false;
  Render();
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
  while (std::optional<TerminalInputToken> token = decoder_.Next()) {
    if (token->kind == TerminalInputTokenKind::kEscape) {
      return {InteractiveInputKind::kEscape, buffer_};
    }
    if (token->kind == TerminalInputTokenKind::kSequence) {
      ApplySequence(token->text);
      continue;
    }
    if (token->kind == TerminalInputTokenKind::kPaste) {
      if (token->overflow || !Insert(token->text)) {
        output_.Write("\a");
      }
      continue;
    }
    unsigned char ch = static_cast<unsigned char>(token->text[0]);
    if (ch == '\r' || ch == '\n') {
      std::string line = std::move(buffer_);
      buffer_.clear();
      cursor_ = 0;
      if (keep_history_ && ShouldRememberInput(line)) {
        history_.push_back(line);
        if (history_.size() > kInputHistoryEntries) history_.pop_front();
      }
      history_index_ = history_.size();
      history_draft_.clear();
      input_limit_bell_ = false;
      last_submitted_rows_ = drawn_rows_;
      MoveToTop();
      EraseDrawnRows();
      // Echo exactly what was drawn: DisplayText maps newlines to ↵, so a
      // pasted multi-line prompt occupies the rows the caller will move back
      // over. Echoing the raw text would print real newlines and desync it.
      output_.Write(UserEchoRow(prompt_, DisplayText(line)) + "\n");
      Detach();
      return {InteractiveInputKind::kLine, std::move(line)};
    }
    if (ch == 0x02) {
      return {InteractiveInputKind::kBackground, buffer_};
    }
    if (ch == 0x04) {
      if (buffer_.empty()) return {InteractiveInputKind::kEof, {}};
      size_t next = NextUtf8(buffer_, cursor_);
      buffer_.erase(cursor_, next - cursor_);
      continue;
    }
    if (ch == 0x7f || ch == 0x08) {
      Backspace();
    } else if (ch == 0x01) {
      cursor_ = 0;
    } else if (ch == 0x05) {
      cursor_ = buffer_.size();
    } else if (ch == 0x0b) {
      buffer_.erase(cursor_);
    } else if (ch == 0x15) {
      buffer_.erase(0, cursor_);
      cursor_ = 0;
    } else if (ch >= 0x20 || ch == '\t') {
      if (!Insert(std::string(1, static_cast<char>(ch))) &&
          !input_limit_bell_) {
        output_.Write("\a");
        input_limit_bell_ = true;
      }
    }
  }
  Render();
  return {};
}

size_t RawComposer::AvailableColumns() const {
  return TerminalWidth(static_cast<int64_t>(DisplayWidth(prompt_)) + 1);
}

RawComposer::Layout RawComposer::ComputeLayout() const {
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
  return {std::move(rows), row, caret_col};
}

void RawComposer::MoveToTop() {
  std::string out;
  AppendMoveToTop(out, caret_row_);
  output_.Write(out);
}

void RawComposer::EraseDrawnRows() {
  std::string out;
  AppendEraseRows(out, drawn_rows_);
  output_.Write(out);
}

void RawComposer::Render() {
  MoveToTop();
  RenderFromTop();
}

void RawComposer::RenderFromTop() {
  Layout layout = ComputeLayout();
  size_t count = layout.rows.size();
  size_t previous_rows = drawn_rows_;
  std::string out;
  AppendEraseRows(out, drawn_rows_);

  // Draw the new block top-to-bottom. Growing taller emits real newlines so
  // extra rows are created rather than overwriting the status line above.
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) out += i >= previous_rows ? "\n\r" : "\033[1B\r";
    out += "\033[2K";
    if (i == 0) out += prompt_;
    out += layout.rows[i];
  }

  // Place the caret from column zero so prompt width and continuation rows
  // are handled explicitly rather than inferred from the old cursor column.
  out += "\r";
  size_t rows_up = count - 1 - layout.caret_row;
  if (rows_up > 0) out += "\033[" + std::to_string(rows_up) + "A";
  size_t caret_column = layout.caret_col;
  if (layout.caret_row == 0) caret_column += DisplayWidth(prompt_);
  if (caret_column > 0) out += "\033[" + std::to_string(caret_column) + "C";

  // One write per redraw: the terminal never observes a half-erased frame.
  output_.Write(out);

  drawn_rows_ = count;
  caret_row_ = layout.caret_row;
  caret_column_ = caret_column;
}

bool RawComposer::Insert(const std::string& text) {
  if (text.size() >
      kInputBufferBytes - std::min(buffer_.size(), kInputBufferBytes)) {
    return false;
  }
  buffer_.insert(cursor_, text);
  cursor_ += text.size();
  input_limit_bell_ = false;
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

void RawComposer::ApplySequence(const std::string& sequence) {
  for (const SequenceBinding& binding : kSequenceBindings) {
    if (binding.sequence != sequence) continue;
    switch (binding.action) {
      case SequenceAction::kHistoryPrevious:
        History(-1);
        break;
      case SequenceAction::kHistoryNext:
        History(1);
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
      case SequenceAction::kDeleteForward:
        if (cursor_ < buffer_.size()) {
          buffer_.erase(cursor_, NextUtf8(buffer_, cursor_) - cursor_);
        }
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
    return;
  }
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

InputBroker::InputBroker() { (void)OpenNonblockingPipe(wake_); }

InputBroker::~InputBroker() {
  Shutdown();
  if (wake_[0] >= 0) close(wake_[0]);
  if (wake_[1] >= 0) close(wake_[1]);
}

std::string InputBroker::Read(const std::string& prompt, bool* eof,
                              bool keep_history, const std::string& initial) {
  std::unique_lock<std::mutex> lock(mutex_);
  prompt_ = prompt;
  initial_ = initial;
  keep_history_ = keep_history;
  pending_ = true;
  answered_ = false;
  Notify();
  changed_.wait(lock, [&] { return answered_ || shutdown_; });
  *eof = shutdown_ || eof_;
  return shutdown_ ? std::string() : answer_;
}

void InputBroker::DrainWake() const { DrainDescriptor(wake_[0]); }

bool InputBroker::Take(std::string& prompt, std::string& initial,
                       bool& keep_history) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_) return false;
  pending_ = false;
  prompt = prompt_;
  initial = initial_;
  keep_history = keep_history_;
  return true;
}

void InputBroker::Answer(std::string answer, bool eof) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    answer_ = std::move(answer);
    eof_ = eof;
    answered_ = true;
  }
  changed_.notify_one();
}

void InputBroker::Notify() const { WakeDescriptor(wake_[1]); }

void InputBroker::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
  }
  changed_.notify_all();
}

}  // namespace uagent
