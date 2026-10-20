// Copyright 2026 Timon Gentzsch

#include <string>
#include <utility>
#include <vector>

#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/markdown.h"
#include "src/ui/markdown_internal.h"

namespace uagent {
namespace {

// ANSI escapes are zero-width to DisplayWidth, and TerminalSafe has already
// removed any raw escape the model could have emitted, so the rendered string
// measures exactly like its visible text.
std::string RenderCell(const std::string& text, size_t& visible_columns) {
  std::string output;
  bool bold = false, italic = false, code = false;
  for (size_t index = 0; index < text.size(); ++index) {
    char value = text[index];
    if (value == '`') {
      code = !code;
      if (code) output += CODE();
      output += value;
      if (!code) output += FgDfl();
      continue;
    }
    size_t math_end = code || (value != '$' && value != '\\')
                          ? std::string::npos
                          : MathSpan(text, index);
    if (math_end != std::string::npos) {
      std::string span = text.substr(index, math_end - index);
      std::string rendered = PrettyMath(MathBody(span));
      output += MATH();
      output += rendered;
      output += FgDfl();
      index = math_end - 1;
      continue;
    }
    if (!code && value == '\\' && index + 1 < text.size() &&
        MarkdownEscapable(text[index + 1])) {
      output += text[++index];
      continue;
    }
    if (value == '*' && !code) {
      bool tight = index + 1 < text.size() && text[index + 1] != ' ';
      if (index + 1 < text.size() && text[index + 1] == '*') {
        bold = !bold;
        output += bold ? BOLD() : BoldOff();
        ++index;
      } else if (italic || tight) {
        italic = !italic;
        output += italic ? ITAL() : ItalOff();
      } else {
        output += value;
      }
      continue;
    }
    output += value;
  }
  visible_columns = DisplayWidth(output);
  if (bold || italic || code) output += RST();
  return output;
}

}  // namespace

std::vector<std::string> SplitCells(const std::string& text) {
  std::vector<std::string> cells;
  std::string cell;
  bool code_span = false;
  for (size_t index = 0; index < text.size(); ++index) {
    char value = text[index];
    if (value == '`') code_span = !code_span;
    size_t end = code_span || (value != '$' && value != '\\')
                     ? std::string::npos
                     : MathSpan(text, index);
    if (end != std::string::npos) {
      cell += text.substr(index, end - index);
      index = end - 1;
    } else if (value == '\\' && index + 1 < text.size() &&
               text[index + 1] == '|') {
      cell += "\\|";
      ++index;
    } else if (value == '|' && !code_span) {
      cells.push_back(Trim(cell));
      cell.clear();
    } else {
      cell += text[index];
    }
  }
  cells.push_back(Trim(cell));
  return cells;
}

void MdStream::RetroTable() {
  for (size_t i = 0; i < prev_rows; ++i) {
    Put(kLineUp);
    Put(kEraseLine);
  }
  table.push_back(prev_raw);
  table.emplace_back(Marker());
  pre.clear();
  cur_raw.clear();
  ForgetRenderedLine();
  ForgetPreviousLine();
  tablemode = true;  // body rows follow until a line without '|'
}

// At the cap, render what has arrived and start a fresh block.
void MdStream::PushTableRow(const std::string& raw) {
  table.push_back(raw);
  if (table.size() < kMdTableRows) return;
  bool resume = tablemode;
  FlushTable();
  tablemode = resume;
}

void MdStream::FlushTable() {
  if (table.empty()) return;
  std::vector<std::vector<std::pair<std::string, size_t>>>
      rows;  // cell, visible width
  std::vector<bool> sep;
  std::vector<size_t> w;
  bool any_sep = false;  // a |---| row is what makes this block a table
  for (auto& raw : table) {
    std::string t = Trim(raw);
    if (t.size() > 1 && t.find_first_not_of("-:| ") == std::string::npos) {
      any_sep = true;
    }
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
      rows.back().emplace_back(std::move(out), vis);
      if (c >= w.size()) w.resize(c + 1, 0);
      if (vis > w[c]) w[c] = vis;
    }
  }
  // no separator row -> not actually a table (e.g. "| jq ."): pass through
  if (!any_sep) {
    for (const auto& raw : table) {
      Put(raw);
      Put('\n');
    }
    table.clear();
    return;
  }
  size_t header = 0;
  while (header < rows.size() && sep[header]) ++header;
  size_t table_width = w.empty() ? 0 : (w.size() - 1) * 2;
  for (size_t width : w) table_width += width;
  if (table_width > static_cast<size_t>(TerminalColumns())) {
    bool after_separator = false;
    for (size_t r = 0; r < rows.size(); ++r) {
      if (sep[r]) {
        after_separator = true;
        continue;
      }
      if (!after_separator && r == header) continue;
      if (r != header && r > 0) Put('\n');
      for (size_t c = 0; c < rows[r].size(); ++c) {
        std::string label = header < rows.size() && c < rows[header].size()
                                ? rows[header][c].first
                                : "Column " + std::to_string(c + 1);
        Put(DIM() + label + ":" + RST() + " " + rows[r][c].first + "\n");
      }
    }
    table.clear();
    ForgetPreviousLine();
    return;
  }
  const std::string no_cell;
  for (size_t r = 0; r < rows.size(); r++) {
    if (sep[r]) continue;
    std::string ln;
    ln.reserve(table_width);
    for (size_t c = 0; c < w.size(); c++) {
      bool present = c < rows[r].size();
      const std::string& text = present ? rows[r][c].first : no_cell;
      if (c > 0) ln += "  ";
      size_t padding =
          c + 1 < w.size() ? w[c] - (present ? rows[r][c].second : 0) : 0;
      ln += text;
      ln.append(padding, ' ');
    }
    Put((r == header ? BOLD() : "") + ln + RST() + "\n");
  }
  table.clear();
  ForgetPreviousLine();
}

}  // namespace uagent
