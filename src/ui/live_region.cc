// Copyright 2026 Timon Gentzsch

#include "include/ui/live_region.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/interactive.h"

namespace uagent {
namespace {

// Every SGR `text` wrote, in order: replayed after a reset they restore the
// styling it left set, so a row drawn on its own keeps the bold or colour an
// earlier row opened, whatever the codes (38;5;n included).
std::string ActiveSgr(std::string_view text) {
  std::string replay;
  for (size_t at = text.find("\033["); at != std::string_view::npos;
       at = text.find("\033[", at + 1)) {
    const size_t end = text.find_first_not_of("0123456789;:", at + 2);
    if (end != std::string_view::npos && text[end] == 'm') {
      replay += text.substr(at, end + 1 - at);
    }
  }
  return replay;
}

std::string Up(size_t rows) {
  return rows ? "\033[" + std::to_string(rows) + "A" : std::string();
}

size_t ScreenRows() {
  winsize size{};
  return ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0
             ? size.ws_row
             : 24;
}

// A live row's own cursor control (a table's retroactive erase) would move the
// frame under it, so only its styling is drawn until it is committed.
std::string StylingOnly(std::string_view row) {
  std::string kept;
  for (size_t at = 0; at < row.size();) {
    if (row.substr(at, 2) == "\033[") {
      const size_t end = row.find_first_not_of("0123456789;?", at + 2);
      if (end != std::string_view::npos && row[end] != 'm') {
        at = end + 1;
        continue;
      }
    }
    kept += row[at++];
  }
  return kept;
}

// `text` in rows of at most `width` columns. Each opens with the styling the
// rows before it left set and closes it, so it can be drawn alone; `raw`
// receives each row's own bytes.
std::vector<std::string> Rows(std::string_view text, size_t width,
                              std::string sgr = "",
                              std::vector<std::string>* raw = nullptr) {
  std::vector<std::string> rows;
  for (size_t from = 0; from < text.size();) {
    const size_t end = std::min(text.find('\n', from), text.size());
    for (std::string& row :
         WrapLines(std::string(text.substr(from, end - from)), width)) {
      std::string drawn = sgr + StylingOnly(row);
      sgr = ActiveSgr(drawn);
      if (drawn.find('\033') != std::string::npos) drawn += RST();
      rows.push_back(std::move(drawn));
      if (raw) raw->push_back(std::move(row));
    }
    from = end + 1;
  }
  return rows;
}

}  // namespace

void LiveRegion::Commit(std::string text) {
  if (text.empty()) return;
  // Finishing the tail as drawn: its rows stay where they are, as scrollback.
  if (committed_.empty() && !drawn_.empty() && !tail_.empty() &&
      tail_ == drawn_tail_ && text.size() > tail_.size() &&
      text.compare(0, tail_.size(), tail_) == 0 && text[tail_.size()] == '\n') {
    adopted_ = drawn_tail_rows_;
    emitted_ = 0;
    text.erase(0, tail_.size() + 1);
    if (text.empty()) return;
  } else if (emitted_ > 0) {
    if (text.compare(0, emitted_, tail_, 0, emitted_) == 0) {
      text = ActiveSgr(std::string_view(text).substr(0, emitted_)) +
             text.substr(emitted_);
    }
    emitted_ = 0;
  }
  if (text.back() != '\n') text += '\n';
  committed_ += text;
}

void LiveRegion::SetTail(std::string text) {
  if (text.compare(0, emitted_, tail_, 0, emitted_) != 0) emitted_ = 0;
  tail_ = std::move(text);
}

void LiveRegion::SetStatus(std::string row) { status_ = std::move(row); }

void LiveRegion::SetComposer(std::vector<std::string> rows, size_t caret_row,
                             size_t caret_column) {
  composer_ = std::move(rows);
  composer_row_ = caret_row;
  composer_column_ = caret_column;
}

void LiveRegion::SetOverlay(std::string text) { overlay_ = std::move(text); }

std::string LiveRegion::ToTop(size_t width) const {
  size_t up = cursor_column_ / width;
  for (size_t row = 0; row < cursor_row_ && row < drawn_.size(); ++row) {
    up += std::max<size_t>(1, DisplayRows(drawn_[row], width));
  }
  return "\r" + Up(up);
}

void LiveRegion::Flush() {
  const size_t width = TerminalWidth();
  const bool resized = g_terminal_resized != 0;
  g_terminal_resized = 0;
  // The status row and the composer, or the picker standing in for it.
  std::vector<std::string> frame{status_};
  size_t caret_row = 1 + composer_row_, caret_column = composer_column_;
  if (overlay_.empty()) {
    frame.insert(frame.end(), composer_.begin(), composer_.end());
  } else {
    std::vector<std::string> rows = Rows(overlay_, width);
    caret_row = rows.size();
    caret_column = DisplayWidth(rows.back());
    frame.insert(frame.end(), rows.begin(), rows.end());
  }
  // As much tail as fits above them; rows scrolling off its top go to
  // scrollback once, so a paragraph taller than the screen is never
  // repainted where no erase can reach it.
  std::vector<std::string> raw;
  std::vector<std::string> tail =
      Rows(std::string_view(tail_).substr(emitted_), width,
           ActiveSgr(std::string_view(tail_).substr(0, emitted_)), &raw);
  const size_t room = std::max<size_t>(
      1, ScreenRows() - std::min(ScreenRows(), frame.size() + 1));
  const size_t spilled = tail.size() > room ? tail.size() - room : 0;
  for (size_t row = 0; row < spilled; ++row) {
    committed_ += tail[row] + "\n";
    emitted_ += raw[row].size();
  }
  tail.erase(tail.begin(), tail.begin() + static_cast<ptrdiff_t>(spilled));
  const size_t tail_rows = tail.size();
  caret_row += tail_rows;
  frame.insert(frame.begin(), tail.begin(), tail.end());

  std::string out;
  if (resized || !committed_.empty() || adopted_ > 0 || drawn_.empty()) {
    // Committed text scrolls the terminal, and a resize reflows it: the old
    // frame is erased and the new one drawn in full below.
    if (drawn_.empty()) {
      out += "\r";
    } else if (adopted_ > 0 && !resized) {
      out += "\r" + Up(cursor_row_ - adopted_) + "\033[J";
    } else {
      out += ToTop(width) + "\033[J";
    }
    adopted_ = 0;
    out += committed_;
    committed_.clear();
    drawn_.clear();
    cursor_row_ = 0;
  } else if (frame == drawn_ && caret_row == cursor_row_ &&
             caret_column == cursor_column_) {
    return;
  }
  size_t at = cursor_row_;
  auto move_to = [&](size_t row) {
    if (row < at) out += "\r" + Up(at - row);
    for (; at < row; ++at) out += "\r\n";
    at = row;
  };
  for (size_t row = 0; row < frame.size(); ++row) {
    if (row < drawn_.size() && frame[row] == drawn_[row]) continue;
    move_to(row);
    out += "\r\033[2K" + frame[row];
  }
  if (drawn_.size() > frame.size()) {
    move_to(frame.size() - 1);
    out += "\r\n\033[J";
    at = frame.size();
  }
  move_to(caret_row);
  out += "\r";
  if (caret_column) out += "\033[" + std::to_string(caret_column) + "C";
  // Synchronized output: a terminal that knows it shows the frame whole.
  output_.Write("\033[?2026h" + out + "\033[?2026l");
  drawn_tail_ = tail_;
  drawn_tail_rows_ = tail_rows;
  drawn_ = std::move(frame);
  cursor_row_ = caret_row;
  cursor_column_ = caret_column;
}

void LiveRegion::Clear() {
  adopted_ = 0;
  if (drawn_.empty()) return;
  output_.Write("\033[?2026h" + ToTop(TerminalWidth()) + "\033[J\033[?2026l");
  drawn_.clear();
  cursor_row_ = cursor_column_ = 0;
}

}  // namespace uagent
