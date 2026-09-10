// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_INTERACTIVE_H_
#define UAGENT_INCLUDE_UI_INTERACTIVE_H_
// Persistent native-scrollback composer and the small bridges that let a
// foreground worker publish output or request synchronous input.

#include <termios.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/cli.h"
#include "include/core/fd.h"
#include "include/ui/input_decoder.h"

namespace uagent {

struct InteractiveOutputUpdate {
  std::string committed;
  std::string tail;
  bool changed = false;
  size_t adopted_prefix_bytes = 0;  // committed prefix already shown as tail
};

// Decodes immutable records and append-only stream fragments from the stdout
// pipe. Unframed output keeps the historical newline-delimited behavior.
class InteractiveTranscript {
 public:
  InteractiveOutputUpdate Feed(std::string_view bytes, bool finish = false);
  void AdoptTail() { tail_.clear(); }

 private:
  void AppendTail(std::string_view text, InteractiveOutputUpdate& update);
  void CommitTail(InteractiveOutputUpdate& update);
  void ApplyFrame(uint8_t kind, std::string_view payload,
                  InteractiveOutputUpdate& update);

  std::string wire_;
  std::string tail_;
};

// Terminal presentation writes complete records atomically. Markdown appends to
// the replaceable tail; a newline, following record, or loop finish commits it.
void WriteTerminalRecord(std::string_view text) noexcept;
void WriteTerminalTail(std::string_view text) noexcept;

// Owns the stdout redirection, so worker output reaches the terminal through
// the composer rather than overwriting the block it has drawn.
class InteractiveOutput {
 public:
  ~InteractiveOutput();

  bool Start();
  void Stop();
  InteractiveOutputUpdate Read(bool finish = false);
  void AdoptTail() { transcript_.AdoptTail(); }
  void Write(const std::string& text) const;

  int ReadFd() const { return read_.Get(); }
  // The real terminal, for a child that has to own the screen: stdout is the
  // pipe here, so handing a subprocess the usual descriptors would send its
  // rendering into the transcript instead of to the user.
  int TerminalFd() const { return saved_.Get(); }

 private:
  Fd saved_;  // the real terminal, kept aside while stdout is the pipe
  Fd read_;
  InteractiveTranscript transcript_;
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

  bool EditTextExternally(std::string& text);

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
  // Hand the draft to $VISUAL/$EDITOR and take back what it saved. False when
  // no editor is configured or the round trip failed, leaving the draft as it
  // was.
  bool EditExternally();

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
  // Ctrl+X seen, waiting to learn whether it opens the editor.
  bool editor_prefix_ = false;
};

// Hands a synchronous input request from a worker thread to the composer
// thread and blocks until it is answered.
class InputBroker {
 public:
  InputBroker();
  ~InputBroker();

  std::string Read(const std::string& prompt, bool* eof, bool keep_history,
                   const std::string& initial, bool editor = false);
  void DrainWake() const;
  bool Take(std::string& prompt, std::string& initial, bool& keep_history,
            bool* editor = nullptr);
  void Answer(std::string answer, bool eof);
  void Notify() const;
  void Shutdown();

  int ReadFd() const { return wake_read_.Get(); }
  int NotifyFd() const { return wake_write_.Get(); }

 private:
  Fd wake_read_, wake_write_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::string prompt_;
  std::string initial_;
  std::string answer_;
  bool keep_history_ = false;
  bool editor_ = false;
  bool pending_ = false;
  bool answered_ = false;
  bool eof_ = false;
  bool shutdown_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_INTERACTIVE_H_
