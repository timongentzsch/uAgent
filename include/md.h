// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MD_H_
#define UAGENT_INCLUDE_MD_H_
// Streaming Markdown -> ANSI renderer for model output. Inline styles
// (**bold**, *italic*, `code`), headings, bullets and fenced code blocks
// render as tokens arrive — no full-message buffering, the stream stays
// live. Tables are the one construct held back until complete, because
// column alignment needs every row; headerless-pipe tables ("A | B" over
// "---|---") are caught retroactively by erasing the already-printed header
// line and re-rendering. TTY only (piped output stays byte-exact);
// UAGENT_MARKDOWN=0 disables.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

struct MdStream {
  bool on;

  // inline state
  bool bold = false, ital = false, code = false;
  int math = 0;  // 1=$…$ · 2=$$…$$ · 3=\(…\) · 4=\[…\]
  bool dollar = false, math_dollar = false, slash = false;
  int star =
      0;  // 1: lone '*' unresolved · 2: "**" seen, opening needs a non-space
  // line state
  bool linestart = true;
  std::string pre;  // held line prefix while it may still be structural
  bool heading = false;
  bool fence = false;      // inside a ``` block
  bool fencehead = false;  // on the ``` marker line itself (rendered dim)
  // table state
  bool intable = false;    // buffering the current line: it started with '|'
  bool tablemode = false;  // buffering lines of a headerless-pipe table
  std::string row;
  std::vector<std::string> table;
  // previous plain line, for retroactive table capture
  std::string cur_raw, prev_raw;
  size_t vis_line = 0, prev_vis = 0;  // visible columns printed on the line

  MdStream() { on = g_tty && EnvStr("UAGENT_MARKDOWN", "1") != "0"; }

  void Feed(const std::string& s) {
    std::string safe = TerminalSafe(s);
    if (!on) {
      fputs(safe.c_str(), stdout);
      fflush(stdout);
      return;
    }
    for (char c : safe) Step(c);
    fflush(stdout);
  }

  void Flush() {  // stream end: resolve everything still held
    if (!on) return;
    if (star) {
      Pv(star == 2 ? "**" : "*");
      star = 0;
    }
    if (dollar) Pc('$');
    if (slash) Pc('\\');
    if (math_dollar) Pc('$');
    if (intable || tablemode) {
      if (!row.empty()) table.push_back(row);
      row.clear();
      intable = tablemode = false;
    }
    FlushTable();
    if (!pre.empty()) Pv(pre), pre.clear();
    if (bold || ital || code || math || heading || fence || fencehead) {
      fputs(RST(), stdout);
    }
    bold = ital = code = heading = fence = fencehead = false;
    math = 0;
    dollar = math_dollar = slash = false;
    linestart = true;
    fflush(stdout);
  }

 private:
  static size_t Ulen(const std::string& s) { return DisplayWidth(s); }
  void Pv(const std::string& s) {
    fputs(s.c_str(), stdout);
    vis_line += Ulen(s);
  }
  void Pc(char c) {
    putchar(c);
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) vis_line++;
  }
  void EmitPre() {
    Pv(pre);
    pre.clear();
  }
  std::string Marker() const {  // pre minus its leading indentation
    size_t sp = pre.find_first_not_of(" \t");
    return sp == std::string::npos ? "" : pre.substr(sp);
  }

  void Step(char c) {
    if (!intable && !tablemode && !fence && !fencehead && c != '\n') {
      cur_raw += c;
    }
    if (intable) {  // line started with '|': one table row
      if (c == '\n') {
        table.push_back(row);
        row.clear();
        cur_raw.clear();
        vis_line = 0;
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
        table.push_back(row);
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
        fputs(RST(), stdout);
        putchar('\n');
        if (fence) fputs(CodeBlk(), stdout);  // color the block body
        fencehead = false;
        linestart = true;
        prev_raw.clear();
        prev_vis = 0;
      } else {
        putchar(c);
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
      putchar(c);
      if (c == '\n') {
        linestart = true;
        prev_raw.clear();
        prev_vis = 0;
      }
      return;
    }
    InlineChar(c);
  }

  // decide what kind of line this is from its first non-space characters
  void Classify(char c) {
    if (fence) {
      FenceClassify(c);
      return;
    }
    std::string mk = Marker();
    if (mk.empty()) {  // only indentation so far
      if (c == ' ' || c == '\t') {
        pre += c;
        return;
      }
      if (!table.empty() && c != '|') FlushTable();  // table ended on this line
      if (c == '#' || c == '`' || c == '-' || c == '*') {
        pre += c;
        return;
      }
      if (c == '|') {
        row = pre + c;
        pre.clear();
        cur_raw.clear();
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
        fputs(BOLD(), stdout);
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
    if (m == '`') {
      if (c == '`' && mk.size() < 3) {
        pre += c;
        if (Marker() == "```") {  // fence opens
          fputs(DIM(), stdout);
          Pv(pre);
          pre.clear();
          fence = true;
          fencehead = true;
          linestart = false;
        }
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
      Pv("• ");
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

  void FenceClassify(char c) {  // inside a fence only "```" is structural
    std::string mk = Marker();
    if (mk.size() >= 3) {  // "```" held: it only closes if the line ends here
      if (c == ' ') {
        pre += c;
        return;
      }
      if (c == '\n') {
        fputs(DIM(), stdout);
        fputs(pre.c_str(), stdout);
        pre.clear();
        fputs(RST(), stdout);
        putchar('\n');
        fence = false;
        return;  // linestart stays true
      }
      fputs(pre.c_str(), stdout);
      pre.clear();  // "```something": content
      putchar(c);
      linestart = false;
      return;
    }
    if (c == ' ' && mk.empty()) {
      pre += c;
      return;
    }
    if (c == '`') {
      pre += c;
      return;
    }
    fputs(pre.c_str(), stdout);
    pre.clear();
    if (c == '\n') {
      putchar('\n');
      prev_raw.clear();
      return;
    }
    putchar(c);
    linestart = false;
  }

  void InlineChar(char c) {
    if (dollar) {
      dollar = false;
      if (c == '$') {
        math = 2;
        fputs(MATH(), stdout);
        Pv("$$");
        return;
      }
      if (c != '\n' && c != ' ' && c != '\t') {
        math = 1;
        fputs(MATH(), stdout);
        Pc('$');
        MathChar(c);
        return;
      }
      Pc('$');
    }
    if (slash) {
      slash = false;
      if (!code && (c == '(' || c == '[')) {
        math = c == '(' ? 3 : 4;
        fputs(MATH(), stdout);
        Pc('\\');
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
          fputs(BoldOff(), stdout);
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
          fputs(BOLD(), stdout);
        }
      } else if (ital) {
        ital = false;
        fputs(ItalOff(), stdout);
      } else if (c != ' ' && c != '\n') {
        ital = true;
        fputs(ITAL(), stdout);
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
    if (c == '`') {
      code = !code;
      fputs(code ? CODE() : FgDfl(), stdout);
      return;
    }
    if (c == '\n') {
      EndLine();
      return;
    }
    Pc(c);
  }

  void MathChar(char c) {
    if (slash) {
      slash = false;
      Pc('\\');
      Pc(c);
      linestart = false;
      if ((math == 3 && c == ')') || (math == 4 && c == ']')) {
        math = 0;
        fputs(FgDfl(), stdout);
      }
      return;
    }
    if (c == '\\') {
      slash = true;
      return;
    }
    if (math == 1 && c == '$') {
      Pc(c);
      linestart = false;
      math = 0;
      fputs(FgDfl(), stdout);
      return;
    }
    if (math == 2) {
      if (math_dollar) {
        math_dollar = false;
        if (c == '$') {
          Pv("$$");
          linestart = false;
          math = 0;
          fputs(FgDfl(), stdout);
          return;
        }
        Pc('$');
      }
      if (c == '$') {
        math_dollar = true;
        return;
      }
    }
    if (c != '\n') {
      Pc(c);
      linestart = false;
      return;
    }
    if (math == 1 || math == 3) {
      math = 0;
      fputs(FgDfl(), stdout);
      EndLine();
      return;
    }
    fputs(RST(), stdout);
    putchar('\n');
    fputs(MATH(), stdout);
    bold = ital = code = heading = false;
    cur_raw.clear();
    prev_raw.clear();
    vis_line = prev_vis = 0;
    linestart = true;
  }

  void EndLine() {  // inline styles never span lines
    if (bold || ital || code || heading) {
      fputs(RST(), stdout);
      bold = ital = code = heading = false;
    }
    putchar('\n');
    prev_raw = cur_raw;
    prev_vis = vis_line;
    cur_raw.clear();
    vis_line = 0;
    linestart = true;
  }

  // --- tables --------------------------------------------------------------

  // "A | B" was already printed as prose and a |-separator just completed:
  // erase the printed header line and rebuild the table from raw text
  void RetroTable() {
    int w = static_cast<int>(TerminalColumns());
    int rows =
        prev_vis ? static_cast<int>((prev_vis - 1) / static_cast<size_t>(w)) + 1
                 : 1;
    for (int i = 0; i < rows; i++) {
      fputs("\033[A\033[2K", stdout);  // up + clear
    }
    table.push_back(prev_raw);
    table.push_back(Marker());
    pre.clear();
    cur_raw.clear();
    vis_line = 0;
    prev_raw.clear();
    prev_vis = 0;
    tablemode = true;  // body rows follow until a line without '|'
  }

  static bool Escaped(const std::string& s, size_t pos) {
    size_t slashes = 0;
    while (pos > slashes && s[pos - slashes - 1] == '\\') ++slashes;
    return slashes % 2;
  }

  // Exclusive end of a complete math span, or npos. Keeping delimiters is
  // deliberate: terminals cannot typeset TeX, so color adds structure while
  // preserving copy/paste fidelity.
  static size_t MathSpan(const std::string& s, size_t pos) {
    if (Escaped(s, pos)) return std::string::npos;
    std::string close;
    size_t begin = pos;
    if (s.compare(pos, 2, "$$") == 0) {
      close = "$$", begin += 2;
    } else if (s[pos] == '$' && pos + 1 < s.size() &&
               !isspace(static_cast<unsigned char>(s[pos + 1]))) {
      close = "$", begin++;
    } else if (s.compare(pos, 2, "\\(") == 0) {
      close = "\\)", begin += 2;
    } else if (s.compare(pos, 2, "\\[") == 0) {
      close = "\\]", begin += 2;
    } else {
      return std::string::npos;
    }
    for (size_t end = begin; (end = s.find(close, end)) != std::string::npos;
         ++end) {
      if (!Escaped(s, end)) return end + close.size();
    }
    return std::string::npos;
  }

  // Render inline Markdown/LaTeX and measure real terminal columns for wide
  // and combining characters.
  static std::string RenderCell(const std::string& s, size_t& vis) {
    std::string out;
    std::string visible;
    bool b = false, i = false, cd = false;
    for (size_t k = 0; k < s.size(); k++) {
      char c = s[k];
      if (c == '`') {
        cd = !cd;
        out += cd ? CODE() : FgDfl();
        continue;
      }
      size_t math_end = cd ? std::string::npos : MathSpan(s, k);
      if (math_end != std::string::npos) {
        std::string span = s.substr(k, math_end - k);
        out += MATH();
        out += span;
        out += FgDfl();
        visible += span;
        k = math_end - 1;
        continue;
      }
      if (c == '*' && !cd) {
        bool tight = k + 1 < s.size() && s[k + 1] != ' ';  // flanked: a marker
        if (k + 1 < s.size() && s[k + 1] == '*') {
          b = !b;
          out += b ? BOLD() : BoldOff();
          k++;
        } else if (i || tight) {
          i = !i;
          out += i ? ITAL() : ItalOff();
        } else {
          out += c;
          visible += c;
        }  // "2 * 3": literal
        continue;
      }
      out += c;
      visible += c;
    }
    vis = DisplayWidth(visible);
    if (b || i || cd) out += RST();
    return out;
  }

  static std::vector<std::string> SplitCells(const std::string& text) {
    std::vector<std::string> cells;
    std::string cell;
    bool code_span = false;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '`') code_span = !code_span;
      size_t end = code_span ? std::string::npos : MathSpan(text, i);
      if (end != std::string::npos) {
        cell += text.substr(i, end - i);
        i = end - 1;
      } else if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '|') {
        cell += "\\|";
        ++i;
      } else if (text[i] == '|' && !code_span) {
        cells.push_back(Trim(cell));
        cell.clear();
      } else {
        cell += text[i];
      }
    }
    cells.push_back(Trim(cell));
    return cells;
  }

  void FlushTable() {
    if (table.empty()) return;
    // no |---| separator row -> not actually a table (e.g. "| jq ."): pass
    // through
    bool any_sep = false;
    for (auto& raw : table) {
      std::string t = Trim(raw);
      if (t.size() > 1 && t.find_first_not_of("-:| ") == std::string::npos) {
        any_sep = true;
      }
    }
    if (!any_sep) {
      for (auto& raw : table) {
        fputs(raw.c_str(), stdout);
        putchar('\n');
      }
      table.clear();
      return;
    }
    std::vector<std::vector<std::pair<std::string, size_t>>>
        rows;  // cell, visible width
    std::vector<bool> sep;
    std::vector<size_t> w;
    for (auto& raw : table) {
      std::string t = Trim(raw);
      if (!t.empty() && t.front() == '|') t.erase(0, 1);
      if (!t.empty() && t.back() == '|') t.pop_back();
      std::vector<std::string> cells = SplitCells(t);
      bool is_sep = true;  // |---|:--:| alignment row
      for (auto& c : cells) {
        if (c.find_first_not_of("-: ") != std::string::npos) is_sep = false;
      }
      sep.push_back(is_sep);
      rows.emplace_back();
      for (size_t c = 0; c < cells.size(); c++) {
        size_t vis = 0;
        std::string out = is_sep ? "" : RenderCell(cells[c], vis);
        rows.back().push_back({out, vis});
        if (c >= w.size()) w.resize(c + 1, 0);
        if (vis > w[c]) w[c] = vis;
      }
    }
    for (size_t r = 0; r < rows.size(); r++) {
      if (sep[r]) {
        std::string ln = "|";
        for (size_t c = 0; c < w.size(); c++) {
          ln += std::string(w[c] + 2, '-') + "|";
        }
        printf("%s%s%s\n", DIM(), ln.c_str(), RST());
        continue;
      }
      std::string ln;
      for (size_t c = 0; c < w.size(); c++) {
        auto cell = c < rows[r].size()
                        ? rows[r][c]
                        : std::make_pair(std::string(), static_cast<size_t>(0));
        ln += std::string(DIM()) + "|" + RST() + " " + cell.first +
              std::string(w[c] - cell.second + 1, ' ');
      }
      printf("%s%s|%s\n", ln.c_str(), DIM(), RST());
    }
    table.clear();
    prev_raw.clear();
    prev_vis = 0;  // a table can't be a header row
  }
};

// one-shot render (for content that was held back during streaming)
inline void MdPrint(const std::string& s) {
  MdStream m;
  m.Feed(s);
  m.Flush();
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MD_H_
