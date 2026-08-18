// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_INTERACTIVE_H_
#define UAGENT_INCLUDE_UI_INTERACTIVE_H_
// Persistent native-scrollback composer and the small bridges that let a
// foreground worker publish output or request synchronous input.

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/cli.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/input_decoder.h"

namespace uagent {

class InteractiveOutput {
 public:
  ~InteractiveOutput() { Stop(); }

  bool Start() {
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
    setvbuf(stdout, nullptr, _IONBF, 0);
    return true;
  }

  void Stop() {
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

  int Fd() const { return read_; }

  std::string Read() const {
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

  void Write(const std::string& text) const {
    (void)WriteAll(saved_, text.data(), text.size());
  }

 private:
  int saved_ = -1;
  int read_ = -1;
};

class RawComposer {
 public:
  explicit RawComposer(const InteractiveOutput& output) : output_(output) {}
  ~RawComposer() { Stop(); }

  // These describe the block actually on screen, not a newly computed layout.
  // That distinction matters after a resize and while an edit changes wrapping.
  bool Drawn() const { return drawn_rows_ > 0; }
  size_t CaretRow() const { return caret_row_; }
  size_t CaretColumn() const { return caret_column_; }
  size_t LastSubmittedRows() const { return last_submitted_rows_; }
  const std::string& Prompt() const { return prompt_; }

  bool Start() {
    if (active_ || tcgetattr(STDIN_FILENO, &saved_) != 0) return false;
    termios raw = saved_;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_iflag &=
        ~static_cast<tcflag_t>(ICRNL | INLCR | IGNCR | IXON | ISTRIP);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    active_ = true;
    output_.Write("\033[?2004h");
    return true;
  }

  void Stop() {
    if (!active_) return;
    output_.Write("\033[?2004l");
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    active_ = false;
  }

  // The application calls Mount only after leaving the cursor at the cleared
  // composer block top, directly below its status row.
  void Mount(std::string prompt, std::string initial = {},
             bool keep_history = true) {
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

  // Repaint the current buffer after the application cleared and rebuilt the
  // status row, again leaving the cursor at the composer block top.
  void Remount() {
    Detach();
    RenderFromTop();
  }

  // Forget the old footprint after the application erased the whole mounted
  // status/composer region.
  void Detach() {
    drawn_rows_ = 0;
    caret_row_ = 0;
    caret_column_ = 0;
  }

  // Clear the current input line and repaint an empty prompt.
  void Clear() {
    buffer_.clear();
    cursor_ = 0;
    history_index_ = history_.size();
    history_draft_.clear();
    input_limit_bell_ = false;
    Render();
  }

  const std::string& Buffer() const { return buffer_; }
  bool HasPending() const { return decoder_.HasReady(); }
  std::optional<std::chrono::steady_clock::time_point> WakeDeadline() const {
    return decoder_.WakeDeadline();
  }

  InteractiveInputEvent Read() {
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
        output_.Write("\r" + prompt_ + DisplayText(line) + "\n");
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

 private:
  struct Layout {
    std::vector<std::string> rows;
    size_t caret_row = 0;
    size_t caret_col = 0;
  };

  size_t AvailableColumns() const {
    return TerminalWidth(static_cast<int64_t>(DisplayWidth(prompt_)) + 1);
  }

  static std::string DisplayText(std::string text) {
    ReplaceAll(text, "\n", "↵");
    ReplaceAll(text, "\t", "⇥");
    return TerminalSafe(text);
  }

  Layout ComputeLayout() const {
    std::string safe = DisplayText(buffer_);
    std::vector<std::string> rows = WrapLines(safe, AvailableColumns());
    if (rows.empty()) rows = {""};

    size_t before_width = DisplayWidth(DisplayText(buffer_.substr(0, cursor_)));
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

  void MoveToTop() {
    output_.Write("\r");
    if (caret_row_ > 0) {
      output_.Write("\033[" + std::to_string(caret_row_) + "A");
    }
  }

  void EraseDrawnRows() {
    for (size_t row = 0; row < drawn_rows_; ++row) {
      output_.Write("\r\033[2K");
      if (row + 1 < drawn_rows_) output_.Write("\033[1B");
    }
    if (drawn_rows_ > 1) {
      output_.Write("\033[" + std::to_string(drawn_rows_ - 1) + "A");
    }
    output_.Write("\r");
  }

  void Render() {
    MoveToTop();
    RenderFromTop();
  }

  void RenderFromTop() {
    Layout layout = ComputeLayout();
    size_t count = layout.rows.size();
    size_t previous_rows = drawn_rows_;
    EraseDrawnRows();

    // Draw the new block top-to-bottom. Growing taller emits real newlines so
    // extra rows are created rather than overwriting the status line above.
    for (size_t i = 0; i < count; ++i) {
      if (i > 0) {
        if (i >= previous_rows) {
          output_.Write("\n\r");
        } else {
          output_.Write("\033[1B\r");
        }
      }
      output_.Write("\033[2K");
      if (i == 0) output_.Write(prompt_);
      output_.Write(layout.rows[i]);
    }

    // Place the caret from column zero so prompt width and continuation rows
    // are handled explicitly rather than inferred from the old cursor column.
    output_.Write("\r");
    size_t rows_up = count - 1 - layout.caret_row;
    if (rows_up > 0) {
      output_.Write("\033[" + std::to_string(rows_up) + "A");
    }
    size_t caret_column = layout.caret_col;
    if (layout.caret_row == 0) caret_column += DisplayWidth(prompt_);
    if (caret_column > 0) {
      output_.Write("\033[" + std::to_string(caret_column) + "C");
    }

    drawn_rows_ = count;
    caret_row_ = layout.caret_row;
    caret_column_ = caret_column;
  }
  // One character back/forward from a boundary, over the shared scanners.
  static size_t PreviousUtf8(const std::string& text, size_t at) {
    return at == 0 ? 0 : Utf8BoundaryBefore(text, at - 1);
  }

  static size_t NextUtf8(const std::string& text, size_t at) {
    return at >= text.size() ? text.size() : Utf8BoundaryAfter(text, at + 1);
  }

  bool Insert(const std::string& text) {
    if (text.size() >
        kInputBufferBytes - std::min(buffer_.size(), kInputBufferBytes)) {
      return false;
    }
    buffer_.insert(cursor_, text);
    cursor_ += text.size();
    input_limit_bell_ = false;
    return true;
  }

  void Backspace() {
    size_t previous = PreviousUtf8(buffer_, cursor_);
    buffer_.erase(previous, cursor_ - previous);
    cursor_ = previous;
    input_limit_bell_ = false;
  }

  static bool WordSpace(unsigned char ch) { return std::isspace(ch) != 0; }

  void PreviousWord() {
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

  void NextWord() {
    while (cursor_ < buffer_.size() &&
           !WordSpace(static_cast<unsigned char>(buffer_[cursor_]))) {
      cursor_ = NextUtf8(buffer_, cursor_);
    }
    while (cursor_ < buffer_.size() &&
           WordSpace(static_cast<unsigned char>(buffer_[cursor_]))) {
      cursor_ = NextUtf8(buffer_, cursor_);
    }
  }

  void DeletePreviousWord() {
    size_t end = cursor_;
    PreviousWord();
    buffer_.erase(cursor_, end - cursor_);
    input_limit_bell_ = false;
  }

  void ApplySequence(const std::string& sequence) {
    if (sequence == "\x1b[A" || sequence == "\x1bOA") History(-1);
    if (sequence == "\x1b[B" || sequence == "\x1bOB") History(1);
    if (sequence == "\x1b[C" || sequence == "\x1bOC") {
      cursor_ = NextUtf8(buffer_, cursor_);
    }
    if (sequence == "\x1b[D" || sequence == "\x1bOD") {
      cursor_ = PreviousUtf8(buffer_, cursor_);
    }
    if (sequence == "\x1b[H" || sequence == "\x1b[1~" || sequence == "\x1bOH") {
      cursor_ = 0;
    }
    if (sequence == "\x1b[F" || sequence == "\x1b[4~" || sequence == "\x1bOF") {
      cursor_ = buffer_.size();
    }
    if (sequence == "\x1b[3~" && cursor_ < buffer_.size()) {
      size_t next = NextUtf8(buffer_, cursor_);
      buffer_.erase(cursor_, next - cursor_);
    }
    if (sequence ==
            "\x1b"
            "b" ||
        sequence == "\x1b[1;3D" || sequence == "\x1b[1;5D") {
      PreviousWord();
    }
    if (sequence ==
            "\x1b"
            "f" ||
        sequence == "\x1b[1;3C" || sequence == "\x1b[1;5C") {
      NextWord();
    }
    if (sequence == "\x1b\x7f" || sequence == "\x1b\x08") {
      DeletePreviousWord();
    }
  }

  void History(int direction) {
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

  const InteractiveOutput& output_;
  termios saved_{};
  bool active_ = false;
  size_t drawn_rows_ = 0;
  size_t caret_row_ = 0;
  size_t caret_column_ = 0;
  size_t last_submitted_rows_ = 1;
  std::string prompt_ = InputPrompt();
  std::string buffer_;
  size_t cursor_ = 0;
  TerminalInputDecoder decoder_;
  std::deque<std::string> history_;
  size_t history_index_ = 0;
  std::string history_draft_;
  bool keep_history_ = true;
  bool input_limit_bell_ = false;
};

class InputBroker {
 public:
  InputBroker() { (void)OpenNonblockingPipe(wake_); }

  ~InputBroker() {
    Shutdown();
    if (wake_[0] >= 0) close(wake_[0]);
    if (wake_[1] >= 0) close(wake_[1]);
  }

  std::string Read(const std::string& prompt, bool* eof, bool keep_history,
                   const std::string& initial) {
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

  int Fd() const { return wake_[0]; }
  int NotifyFd() const { return wake_[1]; }

  void DrainWake() const { DrainDescriptor(wake_[0]); }

  bool Take(std::string& prompt, std::string& initial, bool& keep_history) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_) return false;
    pending_ = false;
    prompt = prompt_;
    initial = initial_;
    keep_history = keep_history_;
    return true;
  }

  void Answer(std::string answer, bool eof) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      answer_ = std::move(answer);
      eof_ = eof;
      answered_ = true;
    }
    changed_.notify_one();
  }

  void Notify() const { WakeDescriptor(wake_[1]); }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    changed_.notify_all();
  }

 private:
  int wake_[2] = {-1, -1};
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::string prompt_;
  std::string initial_;
  std::string answer_;
  bool keep_history_ = false;
  bool pending_ = false;
  bool answered_ = false;
  bool eof_ = false;
  bool shutdown_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_INTERACTIVE_H_
