// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_INTERACTIVE_H_
#define UAGENT_INCLUDE_UI_INTERACTIVE_H_
// Persistent native-scrollback composer and the small bridges that let a
// foreground worker publish output or request synchronous input.

#include <termios.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "include/cli.h"
#include "include/ui/input_decoder.h"

namespace uagent {

// Owns the stdout redirection, so worker output reaches the terminal through
// the composer rather than overwriting the block it has drawn.
class InteractiveOutput {
 public:
  ~InteractiveOutput();

  bool Start();
  void Stop();
  std::string Read() const;
  void Write(const std::string& text) const;

  int Fd() const { return read_; }

 private:
  int saved_ = -1;
  int read_ = -1;
};

class RawComposer {
 public:
  explicit RawComposer(const InteractiveOutput& output);
  ~RawComposer();

  bool Start();
  void Stop();

  // The application calls Mount only after leaving the cursor at the cleared
  // composer block top, directly below its status row.
  void Mount(std::string prompt, std::string initial = {},
             bool keep_history = true);

  // Repaint the current buffer after the application cleared and rebuilt the
  // status row, again leaving the cursor at the composer block top.
  void Remount();

  // Forget the old footprint after the application erased the whole mounted
  // status/composer region.
  void Detach();

  // Clear the current input line and repaint an empty prompt.
  void Clear();

  InteractiveInputEvent Read();

  // These describe the block actually on screen, not a newly computed layout.
  // That distinction matters after a resize and while an edit changes wrapping.
  bool Drawn() const { return drawn_rows_ > 0; }
  size_t CaretRow() const { return caret_row_; }
  size_t CaretColumn() const { return caret_column_; }
  size_t LastSubmittedRows() const { return last_submitted_rows_; }
  const std::string& Prompt() const { return prompt_; }
  const std::string& Buffer() const { return buffer_; }
  bool HasPending() const { return decoder_.HasReady(); }
  std::optional<std::chrono::steady_clock::time_point> WakeDeadline() const {
    return decoder_.WakeDeadline();
  }

 private:
  struct Layout {
    std::vector<std::string> rows;
    size_t caret_row = 0;
    size_t caret_col = 0;
  };

  size_t AvailableColumns() const;
  Layout ComputeLayout() const;
  void MoveToTop();
  void EraseDrawnRows();
  void Render();
  void RenderFromTop();
  bool Insert(const std::string& text);
  void Backspace();
  void PreviousWord();
  void NextWord();
  void DeletePreviousWord();
  void ApplySequence(const std::string& sequence);
  void History(int direction);

  const InteractiveOutput& output_;
  termios saved_{};
  bool active_ = false;
  size_t drawn_rows_ = 0;
  size_t caret_row_ = 0;
  size_t caret_column_ = 0;
  size_t last_submitted_rows_ = 1;
  std::string prompt_;
  std::string buffer_;
  size_t cursor_ = 0;
  TerminalInputDecoder decoder_;
  std::deque<std::string> history_;
  size_t history_index_ = 0;
  std::string history_draft_;
  bool keep_history_ = true;
  bool input_limit_bell_ = false;
};

// Hands a synchronous input request from a worker thread to the composer
// thread and blocks until it is answered.
class InputBroker {
 public:
  InputBroker();
  ~InputBroker();

  std::string Read(const std::string& prompt, bool* eof, bool keep_history,
                   const std::string& initial);
  void DrainWake() const;
  bool Take(std::string& prompt, std::string& initial, bool& keep_history);
  void Answer(std::string answer, bool eof);
  void Notify() const;
  void Shutdown();

  int Fd() const { return wake_[0]; }
  int NotifyFd() const { return wake_[1]; }

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
