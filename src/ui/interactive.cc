// Copyright 2026 Timon Gentzsch

#include "include/ui/interactive.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/cli.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

namespace {

constexpr std::string_view kFrameMagic{"\x1eUAGENT\x1f", 8};
constexpr size_t kFrameHeaderBytes = kFrameMagic.size() + 1 + sizeof(uint64_t);
constexpr uint8_t kRecordFrame = 1;
constexpr uint8_t kTailFrame = 2;

bool ValidFrameKind(uint8_t kind) {
  return kind == kRecordFrame || kind == kTailFrame;
}

uint64_t FrameLength(std::string_view header) {
  uint64_t length = 0;
  size_t begin = kFrameMagic.size() + 1;
  for (size_t index = begin; index < kFrameHeaderBytes; ++index) {
    length = (length << 8) | static_cast<unsigned char>(header[index]);
  }
  return length;
}

size_t PartialMagicSuffix(std::string_view text) {
  size_t limit = std::min(text.size(), kFrameMagic.size() - 1);
  for (size_t length = limit; length > 0; --length) {
    if (text.substr(text.size() - length) == kFrameMagic.substr(0, length)) {
      return length;
    }
  }
  return 0;
}

void WriteFrame(uint8_t kind, std::string_view payload) noexcept {
  if (!PersistentComposer()) {
    if (!payload.empty()) fwrite(payload.data(), 1, payload.size(), stdout);
    return;
  }

  std::array<unsigned char, kFrameHeaderBytes> header{};
  std::copy(kFrameMagic.begin(), kFrameMagic.end(), header.begin());
  header[kFrameMagic.size()] = kind;
  uint64_t length = static_cast<uint64_t>(payload.size());
  for (size_t index = 0; index < sizeof length; ++index) {
    header[kFrameHeaderBytes - index - 1] =
        static_cast<unsigned char>(length & 0xff);
    length >>= 8;
  }

  flockfile(stdout);
  fwrite(header.data(), 1, header.size(), stdout);
  if (!payload.empty()) fwrite(payload.data(), 1, payload.size(), stdout);
  funlockfile(stdout);
}

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
  kInsertNewline,
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
    // Draft newline: Shift+Enter as terminals spell it, plus Alt+Enter.
    {"\x1b\r", SequenceAction::kInsertNewline},
    {"\x1b\n", SequenceAction::kInsertNewline},
    {"\x1b[13;2u", SequenceAction::kInsertNewline},
};

// One completion candidate and the span of the draft it replaces. Two sources
// share this: slash commands, which replace the whole line, and `@paths`,
// which replace only the token under the cursor.
struct Suggestion {
  std::string name;
  std::string description;
  bool wants_argument = false;
};

struct Suggestions {
  size_t begin = 0;
  size_t end = 0;
  std::vector<Suggestion> matches;
};

// What a partially typed "/word" could still become; aliases stay hidden.
Suggestions SlashMatches(const std::string& buffer) {
  Suggestions found{0, buffer.size(), {}};
  if (buffer.empty() || buffer[0] != '/' ||
      buffer.find(' ') != std::string::npos) {
    return found;
  }
  for (const SlashCommandSpec& command : SlashCommandRegistry()) {
    if (!*command.description) continue;
    if (std::string_view(command.name).starts_with(buffer)) {
      found.matches.push_back(
          {command.name, command.description, *command.argument != 0});
    }
  }
  return found;
}

// A path is completed one segment at a time, from the directory the token
// already names -- the way a shell does it. Listing one directory per keypress
// needs no index, no cache to invalidate and no subprocess, and it cannot
// offer a build tree the repository ignores unless the typist walked into one.
Suggestions PathMatches(const std::string& buffer, size_t cursor) {
  Suggestions found{cursor, cursor, {}};
  size_t at = buffer.rfind('@', cursor == 0 ? 0 : cursor - 1);
  if (at == std::string::npos || at >= cursor) return found;
  // Only at a word boundary: an email address or a decorator is not a path.
  if (at > 0 && std::isspace(static_cast<unsigned char>(buffer[at - 1])) == 0) {
    return found;
  }
  std::string typed = buffer.substr(at + 1, cursor - at - 1);
  if (typed.find_first_of(" \t\n") != std::string::npos) return found;
  found.begin = at;

  size_t slash = typed.rfind('/');
  std::string parent = slash == std::string::npos ? "" : typed.substr(0, slash + 1);
  std::string prefix = slash == std::string::npos ? typed : typed.substr(slash + 1);

  namespace fs = std::filesystem;
  std::error_code error;
  // A cap, not a page: the whole set is what the shared prefix is computed
  // from, while only the first few are ever drawn.
  constexpr size_t kCandidateCap = 256;
  for (fs::directory_iterator it(parent.empty() ? "." : parent, error), end;
       it != end && !error && found.matches.size() < kCandidateCap;
       it.increment(error)) {
    std::string name = it->path().filename().string();
    if (!std::string_view(name).starts_with(prefix)) continue;
    // Hidden entries stay hidden until the typist asks for one by name.
    if (name.starts_with(".") && !prefix.starts_with(".")) continue;
    std::error_code kind_error;
    if (fs::is_directory(it->status(kind_error)) && !kind_error) name += "/";
    found.matches.push_back({"@" + parent + name, "", false});
  }
  std::sort(found.matches.begin(), found.matches.end(),
            [](const Suggestion& a, const Suggestion& b) {
              return a.name < b.name;
            });
  return found;
}

Suggestions CompletionMatches(const std::string& buffer, size_t cursor) {
  Suggestions slash = SlashMatches(buffer);
  if (!slash.matches.empty()) return slash;
  return PathMatches(buffer, cursor);
}

// One character back/forward from a boundary, over the shared scanners.
size_t PreviousUtf8(const std::string& text, size_t at) {
  return at == 0 ? 0 : Utf8BoundaryBefore(text, at - 1);
}

size_t NextUtf8(const std::string& text, size_t at) {
  return at >= text.size() ? text.size() : Utf8BoundaryAfter(text, at + 1);
}

bool WordSpace(unsigned char ch) { return std::isspace(ch) != 0; }

}  // namespace

void WriteTerminalRecord(std::string_view text) noexcept {
  WriteFrame(kRecordFrame, text);
  fflush(stdout);
}

void WriteTerminalTail(std::string_view text) noexcept {
  if (!text.empty()) WriteFrame(kTailFrame, text);
}

void InteractiveTranscript::AppendTail(std::string_view text,
                                       InteractiveOutputUpdate& update) {
  if (text.empty()) return;
  tail_.append(text);
  size_t split = tail_.rfind('\n');
  if (split != std::string::npos) {
    ++split;
    update.committed.append(tail_, 0, split);
    tail_.erase(0, split);
  }
  update.changed = true;
}

void InteractiveTranscript::CommitTail(InteractiveOutputUpdate& update) {
  if (tail_.empty()) return;
  update.committed += tail_;
  update.committed += '\n';
  tail_.clear();
  update.changed = true;
}

void InteractiveTranscript::ApplyFrame(uint8_t kind, std::string_view payload,
                                       InteractiveOutputUpdate& update) {
  if (kind == kTailFrame) {
    AppendTail(payload, update);
    return;
  }
  CommitTail(update);
  if (payload.empty()) return;
  update.committed += payload;
  if (payload.back() != '\n') update.committed += '\n';
  update.changed = true;
}

InteractiveOutputUpdate InteractiveTranscript::Feed(std::string_view bytes,
                                                    bool finish) {
  size_t visible_tail_bytes = tail_.size();
  wire_.append(bytes);
  InteractiveOutputUpdate update;
  size_t offset = 0;
  while (offset < wire_.size()) {
    size_t marker = wire_.find(kFrameMagic.data(), offset, kFrameMagic.size());
    if (marker == std::string::npos) {
      std::string_view remainder(wire_.data() + offset, wire_.size() - offset);
      size_t keep = PartialMagicSuffix(remainder);
      AppendTail(remainder.substr(0, remainder.size() - keep), update);
      offset = wire_.size() - keep;
      break;
    }
    if (marker > offset) {
      AppendTail(std::string_view(wire_).substr(offset, marker - offset),
                 update);
      offset = marker;
    }
    if (wire_.size() - offset < kFrameHeaderBytes) break;

    std::string_view header(wire_.data() + offset, kFrameHeaderBytes);
    uint8_t kind = static_cast<uint8_t>(header[kFrameMagic.size()]);
    uint64_t encoded_length = FrameLength(header);
    if (!ValidFrameKind(kind) ||
        encoded_length > std::numeric_limits<size_t>::max()) {
      AppendTail(std::string_view(wire_).substr(offset, 1), update);
      ++offset;
      continue;
    }
    size_t length = static_cast<size_t>(encoded_length);
    size_t available = wire_.size() - offset - kFrameHeaderBytes;
    if (length > available) break;
    ApplyFrame(
        kind,
        std::string_view(wire_).substr(offset + kFrameHeaderBytes, length),
        update);
    offset += kFrameHeaderBytes + length;
  }
  wire_.erase(0, offset);

  if (finish && !wire_.empty()) {
    bool starts_frame = wire_.starts_with(kFrameMagic);
    if (starts_frame && wire_.size() >= kFrameHeaderBytes) {
      std::string_view header(wire_.data(), kFrameHeaderBytes);
      uint8_t kind = static_cast<uint8_t>(header[kFrameMagic.size()]);
      if (ValidFrameKind(kind)) {
        ApplyFrame(kind, std::string_view(wire_).substr(kFrameHeaderBytes),
                   update);
      }
    } else if (!starts_frame && !kFrameMagic.starts_with(wire_)) {
      AppendTail(wire_, update);
    }
    wire_.clear();
  }
  if (finish) CommitTail(update);
  update.tail = tail_;
  if (visible_tail_bytes > 0 && update.committed.size() > visible_tail_bytes &&
      update.committed[visible_tail_bytes] == '\n') {
    update.adopted_prefix_bytes = visible_tail_bytes + 1;
  }
  return update;
}

InteractiveOutput::~InteractiveOutput() { Stop(); }

bool InteractiveOutput::Start() {
  int descriptors[2];
  if (pipe(descriptors) != 0) return false;
  Fd read_end(descriptors[0]);
  Fd write_end(descriptors[1]);
  Fd saved(dup(STDOUT_FILENO));
  if (!saved) return false;
  fcntl(saved.Get(), F_SETFD, FD_CLOEXEC);
  fcntl(read_end.Get(), F_SETFL, fcntl(read_end.Get(), F_GETFL) | O_NONBLOCK);
  if (dup2(write_end.Get(), STDOUT_FILENO) < 0) return false;
  saved_ = std::move(saved);
  read_ = std::move(read_end);
  // From here stdout is the pipe, so signal handlers must not write there.
  SetSignalTerminalFd(saved_.Get());
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
  if (!saved_) return;
  fflush(stdout);
  SetSignalTerminalFd(STDOUT_FILENO);
  dup2(saved_.Get(), STDOUT_FILENO);
  saved_.Reset();
  read_.Reset();
}

InteractiveOutputUpdate InteractiveOutput::Read(bool finish) {
  if (finish) fflush(stdout);
  std::string bytes;
  char buffer[8192];
  for (;;) {
    ssize_t count = read(read_.Get(), buffer, sizeof buffer);
    if (count > 0) {
      bytes.append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  return transcript_.Feed(bytes, finish);
}

void InteractiveOutput::Write(const std::string& text) const {
  (void)WriteAll(saved_.Get(), text.data(), text.size());
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
  bool pasted_in_batch = false;
  while (std::optional<TerminalInputToken> token = decoder_.Next()) {
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
    // Both spellings submit: a piped script sends the bare newline.
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
    } else if (ch == '\t') {  // completes a command or a path, else a plain tab
      Suggestions found = CompletionMatches(buffer_, cursor_);
      if (found.matches.empty()) {
        Insert("\t");
      } else {
        // The longest prefix every candidate agrees on: one Tab commits what
        // is certain, and the rows below the draft show what is still open.
        std::string name = found.matches.front().name;
        for (const Suggestion& match : found.matches) {
          name.resize(static_cast<size_t>(
              std::mismatch(name.begin(), name.end(), match.name.begin(),
                            match.name.end())
                  .first -
              name.begin()));
        }
        if (found.matches.size() == 1 && found.matches.front().wants_argument) {
          name += " ";
        }
        buffer_.replace(found.begin, found.end - found.begin, name);
        cursor_ = found.begin + name.size();
      }
    } else if (ch >= 0x20) {
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
  // Below the draft, inside the erased block. Plain text: these rows are
  // measured, and an SGR escape is not width.
  Suggestions found = CompletionMatches(buffer_, cursor_);
  constexpr size_t kShownMatches = 5;
  for (size_t index = 0; index < found.matches.size() && index < kShownMatches;
       ++index) {
    const Suggestion& match = found.matches[index];
    std::string suggestion = "  " + match.name;
    if (!match.description.empty()) suggestion += "  " + match.description;
    rows.push_back(DisplayTrunc(suggestion, AvailableColumns()));
  }
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

InputBroker::InputBroker() {
  int wake[2] = {-1, -1};
  if (OpenNonblockingPipe(wake)) {
    wake_read_.Reset(wake[0]);
    wake_write_.Reset(wake[1]);
  }
}

InputBroker::~InputBroker() { Shutdown(); }

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

void InputBroker::DrainWake() const { DrainDescriptor(wake_read_.Get()); }

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

void InputBroker::Notify() const { WakeDescriptor(wake_write_.Get()); }

void InputBroker::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
  }
  changed_.notify_all();
}

}  // namespace uagent
