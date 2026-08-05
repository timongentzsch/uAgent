// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_INTERACTIVE_H_
#define UAGENT_INCLUDE_UI_INTERACTIVE_H_
// Persistent native-scrollback composer and the small bridges that let a
// foreground worker publish output or request synchronous input.

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "include/cli.h"
#include "include/core/strings.h"
#include "include/core/term.h"

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
    const char* data = text.data();
    size_t left = text.size();
    while (left > 0) {
      ssize_t count = write(saved_, data, left);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return;
      data += count;
      left -= static_cast<size_t>(count);
    }
  }

 private:
  int saved_ = -1;
  int read_ = -1;
};

class RawComposer {
 public:
  explicit RawComposer(const InteractiveOutput& output) : output_(output) {}
  ~RawComposer() { Stop(); }

  // Number of terminal rows the current buffer occupies when displayed (1 when
  // empty/onone-line, >1 when the input wraps past the prompt row).
  size_t Height() const {
    std::string shown = TerminalSafe(buffer_);
    size_t full = DisplayWidth(shown);
    if (full == 0) return 1;
    size_t available = static_cast<size_t>(std::max(
        int64_t{1},
        TerminalColumns() - static_cast<int64_t>(DisplayWidth(prompt_)) - 1));
    return 1 + (full - 1) / available;
  }

  bool Start() {
    if (active_ || tcgetattr(STDIN_FILENO, &saved_) != 0) return false;
    termios raw = saved_;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
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

  void Set(std::string prompt, std::string initial = {}) {
    prompt_ = std::move(prompt);
    buffer_ = std::move(initial);
    cursor_ = buffer_.size();
    Render();
  }

  const std::string& Buffer() const { return buffer_; }
  bool HasPending() const { return !pending_.empty(); }

  InteractiveInputEvent Read() {
    unsigned char bytes[4096];
    for (;;) {
      ssize_t count = read(STDIN_FILENO, bytes, sizeof bytes);
      if (count > 0) {
        pending_.insert(pending_.end(), bytes, bytes + count);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      break;
    }
    while (!pending_.empty()) {
      unsigned char ch = pending_.front();
      pending_.pop_front();
      if (paste_) {
        if (ch == 0x1b && Consume("[201~")) {
          paste_ = false;
        } else if (ch == '\r') {
          Insert("\n");
        } else if (ch != '\0') {
          Insert(std::string(1, static_cast<char>(ch)));
        }
        continue;
      }
      if (ch == 0x1b) {
        if (Consume("[200~")) {
          paste_ = true;
          continue;
        }
        if (Consume("[A")) {
          History(-1);
          continue;
        }
        if (Consume("[B")) {
          History(1);
          continue;
        }
        if (Consume("[C")) {
          cursor_ = NextUtf8(buffer_, cursor_);
          continue;
        }
        if (Consume("[D")) {
          cursor_ = PreviousUtf8(buffer_, cursor_);
          continue;
        }
        return {InteractiveInputKind::kEscape, buffer_};
      }
      if (ch == '\r' || ch == '\n') {
        std::string line = std::move(buffer_);
        buffer_.clear();
        cursor_ = 0;
        if (!Trim(line).empty()) {
          history_.push_back(line);
          if (history_.size() > 200) history_.pop_front();
        }
        history_index_ = history_.size();
        output_.Write("\r\033[2K" + prompt_ + TerminalSafe(line) + "\n");
        return {InteractiveInputKind::kLine, std::move(line)};
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
        Insert(std::string(1, static_cast<char>(ch)));
      }
    }
    Render();
    return {};
  }

  void Render() const {
    std::string shown = buffer_;
    ReplaceAll(shown, "\n", "↵");
    std::string before = buffer_.substr(0, cursor_);
    ReplaceAll(before, "\n", "↵");
    std::string safe = TerminalSafe(shown);
    size_t available = static_cast<size_t>(std::max(
        int64_t{1},
        TerminalColumns() - static_cast<int64_t>(DisplayWidth(prompt_)) - 1));

    // Long input wraps across visual rows instead of truncating with ….
    size_t full_width = DisplayWidth(safe);
    size_t rows = full_width == 0 ? 1 : 1 + (full_width - 1) / available;

    // Erase the previously rendered input block (starting at the row above the
    // current cursor line and going up), then write each wrapped row. The
    // prompt prefix shares the first row with the text.
    output_.Write("\r\033[2K");
    for (size_t row = 0; row < rows; ++row) {
      if (row > 0) output_.Write("\033[1A\033[2K");
      size_t start = row * available;
      size_t take = std::min(available, full_width - start);
      if (row == 0) {
        output_.Write(prompt_);
        output_.Write("\r\033[" + std::to_string(DisplayWidth(prompt_)) + "C");
      }
      output_.Write(safe.substr(start, take));
    }
    // The cursor is now at the end of the last row. Walk it up and left to the
    // correct scrollback position without clearing the freshly written block.
    size_t before_width = DisplayWidth(before);
    size_t cursor_row = rows > 1 ? before_width / available : 0;
    size_t row_text = std::min(available, full_width - cursor_row * available);
    size_t cursor_col = rows > 1 ? before_width % available : before_width;
    size_t lines_up = rows - 1 - cursor_row;
    if (lines_up) output_.Write("\033[" + std::to_string(lines_up) + "A");
    size_t left = row_text - std::min(cursor_col, row_text);
    if (left) output_.Write("\033[" + std::to_string(left) + "D");
  }

 private:
  static size_t PreviousUtf8(const std::string& text, size_t at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) {
      --at;
    }
    return at;
  }

  static size_t NextUtf8(const std::string& text, size_t at) {
    if (at >= text.size()) return text.size();
    ++at;
    while (at < text.size() &&
           (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) {
      ++at;
    }
    return at;
  }

  void Insert(const std::string& text) {
    buffer_.insert(cursor_, text);
    cursor_ += text.size();
  }

  void Backspace() {
    size_t previous = PreviousUtf8(buffer_, cursor_);
    buffer_.erase(previous, cursor_ - previous);
    cursor_ = previous;
  }

  bool Consume(const char* suffix) {
    size_t size = strlen(suffix);
    if (pending_.size() < size) return false;
    for (size_t i = 0; i < size; ++i) {
      if (pending_[i] != static_cast<unsigned char>(suffix[i])) return false;
    }
    for (size_t i = 0; i < size; ++i) pending_.pop_front();
    return true;
  }

  void History(int direction) {
    if (history_.empty()) return;
    if (direction < 0 && history_index_ > 0) --history_index_;
    if (direction > 0 && history_index_ < history_.size()) ++history_index_;
    buffer_ = history_index_ < history_.size() ? history_[history_index_] : "";
    cursor_ = buffer_.size();
  }

  const InteractiveOutput& output_;
  termios saved_{};
  bool active_ = false;
  bool paste_ = false;
  std::string prompt_ = InputPrompt();
  std::string buffer_;
  size_t cursor_ = 0;
  std::deque<unsigned char> pending_;
  std::deque<std::string> history_;
  size_t history_index_ = 0;
};

class InputBroker {
 public:
  InputBroker() {
    if (pipe(wake_) != 0) wake_[0] = wake_[1] = -1;
    if (wake_[0] >= 0) {
      fcntl(wake_[0], F_SETFL, fcntl(wake_[0], F_GETFL) | O_NONBLOCK);
      fcntl(wake_[1], F_SETFL, fcntl(wake_[1], F_GETFL) | O_NONBLOCK);
    }
  }

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

  void DrainWake() const {
    char bytes[32];
    while (wake_[0] >= 0 && read(wake_[0], bytes, sizeof bytes) > 0) {
    }
  }

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

  void Notify() const {
    if (wake_[1] < 0) return;
    const char byte = 'x';
    ssize_t ignored = write(wake_[1], &byte, 1);
    (void)ignored;
  }

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
