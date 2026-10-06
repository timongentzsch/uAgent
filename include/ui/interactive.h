// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_INTERACTIVE_H_
#define UAGENT_INCLUDE_UI_INTERACTIVE_H_
// Persistent native-scrollback composer and the small bridges that let a
// foreground worker publish output or request synchronous input.

#include <termios.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/cli.h"
#include "include/core/fd.h"
#include "include/ui/input_decoder.h"

namespace uagent {

class LiveRegion;

struct InteractiveOutputUpdate {
  std::string committed;
  std::string tail;
  bool changed = false;
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

// Edits the draft; the LiveRegion draws it, from View.
class RawComposer {
 public:
  // `keys` outlives the composer: what was typed ahead is still there when
  // the terminal attaches again.
  RawComposer(const InteractiveOutput& output, LiveRegion& region,
              TerminalInputDecoder& keys);
  ~RawComposer();

  bool Start();
  void Stop();

  void Mount(std::string prompt, std::string initial = {},
             bool keep_history = true);

  // Clear the current input line.
  void Clear();

  InteractiveInputEvent Read();

  // Hands the terminal to $VISUAL/$EDITOR, the live region erased first.
  bool EditTextExternally(std::string& text);

  // The prompt and the wrapped draft, completions below, and where the caret
  // sits among those rows.
  struct Layout {
    std::vector<std::string> rows;
    size_t caret_row = 0;
    size_t caret_col = 0;
  };
  Layout View() const;

  const std::string& Buffer() const { return buffer_; }
  bool HasPending() const { return decoder_.HasReady(); }
  std::optional<std::chrono::steady_clock::time_point> WakeDeadline() const {
    return decoder_.WakeDeadline();
  }

 private:
  size_t AvailableColumns() const;
  bool Insert(const std::string& text);
  void Backspace();
  void PreviousWord();
  void NextWord();
  void DeletePreviousWord();
  void Complete();
  // Moves the menu highlight; false when no menu is open, so the arrow
  // browses history instead.
  bool Select(int direction);
  // False when no binding names `sequence`.
  bool ApplySequence(const std::string& sequence);
  void History(int direction);
  // A fresh draft: history browsing starts over from the newest entry.
  void ResetDraftState();
  // Hand the draft to $VISUAL/$EDITOR and take back what it saved. False when
  // no editor is configured or the round trip failed, leaving the draft as it
  // was.
  bool EditExternally();

  const InteractiveOutput& output_;
  LiveRegion& region_;
  termios saved_{};
  bool active_ = false;
  std::string prompt_;
  std::string buffer_;
  size_t cursor_ = 0;
  // The highlighted row of the slash command menu; any edit resets it.
  size_t selected_ = 0;
  TerminalInputDecoder& decoder_;
  std::deque<std::string> history_;
  size_t history_index_ = 0;
  std::string history_draft_;
  bool keep_history_ = true;
  bool input_limit_bell_ = false;
  // Why the last key did nothing, shown under the draft until the next one.
  std::string note_;
  // Ctrl+X seen, waiting to learn whether it opens the editor.
  bool editor_prefix_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_INTERACTIVE_H_
