// Copyright 2026 Timon Gentzsch

#include "include/ui/markdown.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/ui/interactive.h"
#include "src/ui/markdown_internal.h"

namespace uagent {

bool MarkdownEscapable(char value) {
  constexpr std::string_view kEscapable = R"(\`*_${}[]()#+-.!|>$)";
  return kEscapable.find(value) != std::string_view::npos;
}

MdStream::MdStream() { on = g_tty; }

void MdStream::Feed(std::string_view s) {
  std::string safe = TerminalSafe(s);
  if (!on) {
    Put(safe);
    FlushOut();
    return;
  }
  for (char c : safe) Step(c);
  FlushOut();
}

void MdStream::FeedPlain(std::string_view s) {
  Put(TerminalSafe(s));
  FlushOut();
}

void MdStream::Control(const char* s) {
  Put(s);
  // A control sequence is the caller's way of ordering its own writes around
  // this stream, so it may not sit in the buffer waiting for text.
  FlushOut();
}

void MdStream::Flush() {  // stream end: resolve everything still held
  if (!on) {
    FlushOut();
    return;
  }
  while (math) ReplayMath();
  if (star) Pv(star == 2 ? "**" : "*");
  if (dollar) Pc('$');
  if (slash) Pc('\\');
  if (intable || tablemode) {
    if (!row.empty()) PushTableRow(row);
    row.clear();
    intable = tablemode = false;
  }
  FlushTable();
  EmitPre();
  if (bold || ital || code || math || heading || fence || fencehead) {
    Put(RST());
  }
  blank_held = 0;  // trailing blank lines never reach the terminal
  bold = ital = code = heading = fence = fencehead = false;
  math = 0;
  math_text.clear();
  star = 0;
  dollar = math_dollar = slash = false;
  linestart = true;
  FlushOut();
}

void MdStream::ReleaseBlanks() {
  if (!blank_held) return;
  blank_held = 0;
  Put('\n');
}

void MdStream::Pv(const std::string& s) {
  if (s.empty()) return;  // an empty write is not output, and must not release
  ReleaseBlanks();
  Put(s);
}

void MdStream::Pc(char c) {
  ReleaseBlanks();
  Put(c);
}

// Emission is collected here and written as one tail frame per chunk instead
// of once per character, which is where a third of the render cost sat: stdio
// takes its lock — needed, because TerminalSpinner writes stdout from its own
// thread when no composer is mounted (core/term.h) — on every call. Every
// public entry point drains the buffer before returning, so nothing an
// outside writer can observe has moved.
void MdStream::Put(char value) {
  outbuf += value;
  TrackRendered(std::string_view(&value, 1));
}

void MdStream::Put(std::string_view text) {
  outbuf.append(text);
  TrackRendered(text);
}

// Colour changes cost no columns, and neither does the closing newline. An
// escape is always written whole: the model's text has none left.
void MdStream::TrackRendered(std::string_view text) {
  // Whole runs of plain text are the common case, so they are measured in
  // one append rather than a byte at a time.
  if (text.find_first_of("\x1b\n\r") == std::string_view::npos) {
    if (cur_rendered.size() < kMdLineBytes) cur_rendered.append(text);
    return;
  }
  for (size_t at = 0; at < text.size(); ++at) {
    if (const size_t span = CsiSpan(text.substr(at))) {
      at += span - 1;
    } else if (text[at] != '\n' && text[at] != '\r' &&
               cur_rendered.size() < kMdLineBytes) {
      cur_rendered += text[at];
    }
  }
}

void MdStream::ForgetRenderedLine() { cur_rendered.clear(); }

void MdStream::FlushOut() {
  if (outbuf.empty()) return;
  WriteTerminalTail(outbuf);
  outbuf.clear();
}

void MdStream::EmitPre() {
  Pv(pre);
  pre.clear();
}

// The view is only valid until the next change to pre; every caller reads it
// before appending to or clearing pre.
std::string_view MdStream::Marker() const {  // pre minus its indentation
  size_t sp = pre.find_first_not_of(" \t");
  return sp == std::string::npos ? std::string_view()
                                 : std::string_view(pre).substr(sp);
}

void MdStream::Step(char c) {
  if (!intable && !tablemode && !fence && !fencehead && c != '\n' &&
      cur_raw.size() < kMdLineBytes) {
    cur_raw += c;
  }
  if (intable) {  // line started with '|': one table row
    if (c == '\n') {
      PushTableRow(row);
      row.clear();
      cur_raw.clear();
      ForgetRenderedLine();
      intable = false;
      linestart = true;
    } else {
      row += c;
    }
    return;
  }
  if (tablemode) {  // headerless-pipe table body: rows until a line without
                    // '|'
    if (c != '\n') {
      row += c;
      return;
    }
    if (SplitCells(row).size() > 1) {
      PushTableRow(row);
      row.clear();
      return;
    }
    std::string tail = row;
    row.clear();
    tablemode = false;
    FlushTable();
    linestart = true;
    for (char t : tail) Step(t);
    Step('\n');
    return;
  }
  if (fencehead) {  // rest of the ``` marker line, shown dim
    if (c == '\n') {
      Put(RST());
      Put('\n');
      if (fence) Put(CodeBlk());  // color the block body
      fencehead = false;
      linestart = true;
      ForgetPreviousLine();
    } else {
      Put(c);
    }
    return;
  }
  if (math) {
    MathChar(c);
    return;
  }
  if (linestart) {
    Classify(c);
    return;
  }
  if (fence) {
    Put(c);
    if (c == '\n') {
      linestart = true;
      ForgetPreviousLine();
      // The persistent composer inserts a reset-styled status row after a
      // complete line. Queue the fence color at the start of the next line so
      // streamed code blocks do not lose their styling after line one.
      Put(CodeBlk());
    }
    return;
  }
  InlineChar(c);
}

void MdStream::Classify(char c) {
  if (fence) {
    FenceClassify(c);
    return;
  }
  std::string_view mk = Marker();
  if (mk.empty()) {  // only indentation so far
    if (c == ' ' || c == '\t') {
      pre += c;
      return;
    }
    if (!table.empty() && c != '|') FlushTable();  // table ended on this line
    if (c == '#' || c == '`' || c == '~' || c == '-' || c == '*') {
      pre += c;
      return;
    }
    if (c == '|') {
      row = pre + c;
      pre.clear();
      cur_raw.clear();
      ForgetRenderedLine();
      intable = true;
      return;
    }
    EmitPre();
    if (c == '\n') {
      EndLine();
      return;
    }  // blank line
    linestart = false;
    InlineChar(c);
    return;
  }
  char m = mk[0];
  if (m == '#') {
    if (c == '#' && mk.size() < 6) {
      pre += c;
      return;
    }
    if (c == ' ') {  // "# ..." -> bold, hashes hidden
      Pv(pre.substr(0, pre.size() - mk.size()));  // indentation
      Put(BOLD());
      heading = true;
      pre.clear();
      linestart = false;
      return;
    }
    EmitPre();  // "#hashtag" etc: plain text
    linestart = false;
    InlineChar(c);
    return;
  }
  if (m == '`' || m == '~') {
    if (c == m && mk.find_first_not_of(m) == std::string_view::npos) {
      pre += c;  // the run goes on: its length is what closes the fence
      return;
    }
    if (IsMarkdownFence(mk)) {  // fence opens; `c` starts its info line
      Put(DIM());
      Pv(pre);
      pre.clear();
      fence = true;
      fence_char = m;
      fence_len = mk.size();
      fencehead = true;
      linestart = false;
      Step(c);
      return;
    }
    if (m == '~') {  // a tilde or two is text
      EmitPre();
      linestart = false;
      InlineChar(c);
      return;
    }
    // 1-2 backticks then something else: inline code span(s)
    std::string held = pre;
    pre.clear();
    linestart = false;
    for (char h : held) {
      if (h == '`') {
        InlineChar('`');
      } else {
        Pc(h);
      }
    }
    InlineChar(c);
    return;
  }
  // m is '-' or '*' (held marker): bullet if a space follows the single char
  if (mk.size() == 1 && c == ' ') {
    Pv(pre.substr(0, pre.size() - 1));
    Pv(AsciiGlyphs("• "));
    pre.clear();
    linestart = false;
    return;
  }
  if (m == '-') {
    // could still be a |-separator row ("---|---", "--- | ---")
    bool seppish = mk.find_first_not_of("-:| ") == std::string::npos;
    if (seppish &&
        (c == '-' || c == ':' || c == '|' || (c == ' ' && mk.size() > 1))) {
      pre += c;
      return;
    }
    if (c == '\n') {
      if (seppish && mk.find('|') != std::string::npos &&
          SplitCells(prev_raw).size() > 1) {
        RetroTable();
        return;
      }
      EmitPre();
      EndLine();
      return;
    }
    EmitPre();
    linestart = false;
    InlineChar(c);
    return;
  }
  pre.pop_back();  // '*' was an emphasis marker, not a bullet
  EmitPre();
  linestart = false;
  star = 1;
  InlineChar(c);
}

// Inside a fence only a run of its marker, at least as long as the one that
// opened it, is structural. The marker line is written here directly rather
// than through Pc/Pv, so held blanks are released by hand.
void MdStream::FenceClassify(char c) {
  ReleaseBlanks();
  std::string_view mk = Marker();
  const size_t run = std::min(mk.find_first_not_of(fence_char), mk.size());
  if (c == fence_char && run == mk.size()) {
    pre += c;
    return;
  }
  if (run >= fence_len) {  // held: it only closes if the line ends here
    if (c == ' ') {
      pre += c;
      return;
    }
    if (c == '\n') {
      Put(DIM());
      Put(pre);
      pre.clear();
      Put(RST());
      Put('\n');
      fence = false;
      return;  // linestart stays true
    }
    Put(pre);
    pre.clear();  // "```something": content
    Put(c);
    linestart = false;
    return;
  }
  if (c == ' ' && mk.empty()) {
    pre += c;
    return;
  }
  Put(pre);
  pre.clear();
  if (c == '\n') {
    Put('\n');
    prev_raw.clear();
    Put(CodeBlk());
    return;
  }
  Put(c);
  linestart = false;
}

void MdStream::InlineChar(char c) {
  if (dollar) {
    dollar = false;
    if (c == '$') {
      math = 2;
      Put(MATH());
      math_text.clear();
      return;
    }
    if (c != '\n' && c != ' ' && c != '\t') {
      math = 1;
      Put(MATH());
      math_text.clear();
      MathChar(c);
      return;
    }
    Pc('$');
  }
  if (slash) {
    slash = false;
    if (!code && (c == '(' || c == '[')) {
      math = c == '(' ? 3 : 4;
      Put(MATH());
      math_text.clear();
      return;
    }
    if (!code && MarkdownEscapable(c)) {
      Pc(c);
      return;
    }
    Pc('\\');
  }
  if (star) {
    int n = star;
    star = 0;
    if (n == 1 && c == '*') {  // "**"
      if (bold) {
        bold = false;
        Put(BoldOff());
      } else {
        star = 2;  // opening bold only if a non-space follows
      }
      return;
    }
    if (n == 2) {
      if (c == ' ' || c == '\n') {
        Pv("**");  // "a ** b": literal
      } else {
        bold = true;
        Put(BOLD());
      }
    } else if (ital) {
      ital = false;
      Put(ItalOff());
    } else if (c != ' ' && c != '\n') {
      ital = true;
      Put(ITAL());
    } else {
      Pv("*");  // "2 * 3": literal
    }
  }
  if (c == '$' && !code) {
    dollar = true;
    return;
  }
  if (c == '\\' && !code) {
    slash = true;
    return;
  }
  if (c == '*' && !code) {
    star = 1;
    return;
  }
  if (c == '`') {  // kept: in the default foreground it marks the span
    code = !code;
    if (code) Put(CODE());
    Pc(c);
    if (!code) Put(FgDfl());
    return;
  }
  if (c == '\n') {
    EndLine();
    return;
  }
  Pc(c);
}

void MdStream::ForgetPreviousLine() {
  prev_raw.clear();
  prev_rows = 1;
}

void MdStream::EndLine() {  // inline styles never span lines
  if (bold || ital || code || heading) {
    Put(RST());
    bold = ital = code = heading = false;
  }
  // Nothing visible reached this line, so hold its newline rather than print
  // it: ReleaseBlanks emits one if text follows, and Flush drops the rest.
  // One width pass over the raw line replaces per-character bookkeeping that
  // counted every leading UTF-8 byte as a column, so CJK, emoji and combining
  // marks used to give prev_rows (and thus RetroTable) the wrong row count.
  size_t vis_line = DisplayWidth(cur_raw);
  if (vis_line == 0) {
    ++blank_held;
  } else {
    Put('\n');
  }
  size_t columns = TerminalWidth();
  // Rows come from the rendered line, which is what RetroTable has to erase.
  size_t rendered = DisplayWidth(cur_rendered);
  prev_rows = rendered ? (rendered - 1) / columns + 1 : 1;
  prev_raw = std::move(cur_raw);
  cur_raw.clear();
  ForgetRenderedLine();
  linestart = true;
}

void MdPrint(const std::string& text) {
  MdStream stream;
  stream.Feed(text);
  stream.Flush();
}

}  // namespace uagent
