// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"

namespace uagent {
namespace {

constexpr size_t kFlushBytes = 256;
constexpr auto kFlushInterval = std::chrono::milliseconds(16);

const char* MathOpen(int math) {
  switch (math) {
    case 1:
      return "$";
    case 2:
      return "$$";
    case 3:
      return "\\(";
    default:
      return "\\[";
  }
}

bool Escaped(const std::string& text, size_t position) {
  size_t slashes = 0;
  while (position > slashes && text[position - slashes - 1] == '\\') {
    ++slashes;
  }
  return slashes % 2;
}

bool MarkdownEscapable(char value) {
  constexpr std::string_view kEscapable = R"(\`*_${}[]()#+-.!|>$)";
  return kEscapable.find(value) != std::string_view::npos;
}

bool PlausibleInlineMath(std::string_view text) {
  if (text.empty() || text.find("**") != std::string_view::npos ||
      text.find('`') != std::string_view::npos) {
    return false;
  }
  if (!isdigit(static_cast<unsigned char>(text.front()))) return true;
  size_t token = 0;
  while (token < text.size() &&
         (isdigit(static_cast<unsigned char>(text[token])) ||
          text[token] == '.' || text[token] == ',')) {
    ++token;
  }
  if (token == text.size() ||
      !isspace(static_cast<unsigned char>(text[token]))) {
    return true;
  }
  return text.find_first_of("=+-*/^_\\<>", token) != std::string_view::npos;
}

size_t MathSpan(const std::string& text, size_t position) {
  if (Escaped(text, position)) return std::string::npos;
  std::string close;
  size_t begin = position;
  if (text.compare(position, 2, "$$") == 0) {
    close = "$$";
    begin += 2;
  } else if (text[position] == '$' && position + 1 < text.size() &&
             !isspace(static_cast<unsigned char>(text[position + 1]))) {
    close = "$";
    ++begin;
  } else if (text.compare(position, 2, "\\(") == 0) {
    close = "\\)";
    begin += 2;
  } else if (text.compare(position, 2, "\\[") == 0) {
    close = "\\]";
    begin += 2;
  } else {
    return std::string::npos;
  }
  for (size_t end = begin; (end = text.find(close, end)) != std::string::npos;
       ++end) {
    if (Escaped(text, end)) continue;
    if (close == "$" && !PlausibleInlineMath(std::string_view(text).substr(
                            begin, end - begin))) {
      return std::string::npos;
    }
    return end + close.size();
  }
  return std::string::npos;
}

bool TakeGroup(const std::string& text, size_t& position, std::string& group) {
  if (position >= text.size() || text[position] != '{') return false;
  size_t original = position;
  size_t begin = ++position;
  int depth = 1;
  while (position < text.size() && depth > 0) {
    if (text[position] == '{') ++depth;
    if (text[position] == '}') --depth;
    ++position;
  }
  if (depth != 0) {
    position = original;
    return false;
  }
  group = text.substr(begin, position - begin - 1);
  return true;
}

std::string PrettyMath(const std::string& text);

std::string MathCommand(std::string_view command) {
  static constexpr std::pair<std::string_view, std::string_view> kCommands[] = {
      {"alpha", "α"},  {"beta", "β"},       {"gamma", "γ"},
      {"delta", "δ"},  {"epsilon", "ε"},    {"theta", "θ"},
      {"lambda", "λ"}, {"mu", "μ"},         {"pi", "π"},
      {"rho", "ρ"},    {"sigma", "σ"},      {"phi", "φ"},
      {"omega", "ω"},  {"Delta", "Δ"},      {"Gamma", "Γ"},
      {"Lambda", "Λ"}, {"Pi", "Π"},         {"Sigma", "Σ"},
      {"Phi", "Φ"},    {"Omega", "Ω"},      {"times", "×"},
      {"cdot", "·"},   {"pm", "±"},         {"le", "≤"},
      {"leq", "≤"},    {"ge", "≥"},         {"geq", "≥"},
      {"ne", "≠"},     {"neq", "≠"},        {"approx", "≈"},
      {"infty", "∞"},  {"sum", "∑"},        {"prod", "∏"},
      {"int", "∫"},    {"partial", "∂"},    {"nabla", "∇"},
      {"to", "→"},     {"rightarrow", "→"}, {"leftarrow", "←"},
      {"in", "∈"},     {"notin", "∉"},      {"forall", "∀"},
      {"exists", "∃"}, {"cup", "∪"},        {"cap", "∩"},
  };
  for (const auto& [name, value] : kCommands) {
    if (name == command) return std::string(value);
  }
  return {};
}

std::string ScriptText(const std::string& text, bool superscript) {
  static constexpr std::string_view kPlain = "0123456789+-=()ni";
  static constexpr std::string_view kSuper[] = {"⁰", "¹", "²", "³", "⁴", "⁵",
                                                "⁶", "⁷", "⁸", "⁹", "⁺", "⁻",
                                                "⁼", "⁽", "⁾", "ⁿ", "ⁱ"};
  static constexpr std::string_view kSub[] = {"₀", "₁", "₂", "₃", "₄", "₅",
                                              "₆", "₇", "₈", "₉", "₊", "₋",
                                              "₌", "₍", "₎", "ₙ", "ᵢ"};
  std::string out;
  for (char value : text) {
    size_t index = kPlain.find(value);
    if (index == std::string_view::npos) {
      return std::string(superscript ? "^(" : "_(") + PrettyMath(text) + ")";
    }
    out += superscript ? kSuper[index] : kSub[index];
  }
  return out;
}

std::string PrettyMath(const std::string& text) {
  std::string out;
  bool spaced = false;
  for (size_t i = 0; i < text.size();) {
    unsigned char value = static_cast<unsigned char>(text[i]);
    if (isspace(value) || text[i] == '~') {
      if (!out.empty() && !spaced) out += ' ';
      spaced = true;
      ++i;
      continue;
    }
    spaced = false;
    if (text[i] == '\\') {
      size_t begin = ++i;
      while (i < text.size() && isalpha(static_cast<unsigned char>(text[i]))) {
        ++i;
      }
      std::string command = text.substr(begin, i - begin);
      if (command == "frac") {
        size_t groups_begin = i;
        std::string numerator, denominator;
        if (TakeGroup(text, i, numerator) && TakeGroup(text, i, denominator)) {
          out += "(" + PrettyMath(numerator) + ")⁄(" + PrettyMath(denominator) +
                 ")";
          continue;
        }
        i = groups_begin;
      } else if (command == "sqrt") {
        std::string radicand;
        if (TakeGroup(text, i, radicand)) {
          out += "√(" + PrettyMath(radicand) + ")";
          continue;
        }
      } else if (command == "text" || command == "mathrm" ||
                 command == "operatorname") {
        std::string group;
        if (TakeGroup(text, i, group)) {
          out += group;
          continue;
        }
      } else if (command == "left" || command == "right") {
        continue;
      } else if (command == "quad" || command == "qquad") {
        if (!out.empty() && out.back() != ' ') out += ' ';
        continue;
      }
      std::string rendered = MathCommand(command);
      if (!rendered.empty()) {
        out += rendered;
      } else if (command.empty() && i < text.size()) {
        out += text[i++];
      } else {
        out += '\\' + command;
      }
      continue;
    }
    if (text[i] == '^' || text[i] == '_') {
      bool superscript = text[i++] == '^';
      std::string script;
      if (!TakeGroup(text, i, script) && i < text.size()) {
        size_t begin = i++;
        if (text[begin] == '\\') {
          while (i < text.size() &&
                 isalpha(static_cast<unsigned char>(text[i]))) {
            ++i;
          }
        }
        script = text.substr(begin, i - begin);
      }
      out += ScriptText(script, superscript);
      continue;
    }
    out += text[i];
    ++i;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string MathBody(const std::string& span) {
  if (span.starts_with("$$")) return span.substr(2, span.size() - 4);
  if (span.starts_with("\\(") || span.starts_with("\\[")) {
    return span.substr(2, span.size() - 4);
  }
  return span.substr(1, span.size() - 2);
}

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
      output += code ? CODE() : FgDfl();
      continue;
    }
    size_t math_end = code ? std::string::npos : MathSpan(text, index);
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

std::vector<std::string> SplitCells(const std::string& text) {
  std::vector<std::string> cells;
  std::string cell;
  bool code_span = false;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '`') code_span = !code_span;
    size_t end = code_span ? std::string::npos : MathSpan(text, index);
    if (end != std::string::npos) {
      cell += text.substr(index, end - index);
      index = end - 1;
    } else if (text[index] == '\\' && index + 1 < text.size() &&
               text[index + 1] == '|') {
      cell += "\\|";
      ++index;
    } else if (text[index] == '|' && !code_span) {
      cells.push_back(Trim(cell));
      cell.clear();
    } else {
      cell += text[index];
    }
  }
  cells.push_back(Trim(cell));
  return cells;
}

}  // namespace

MdStream::MdStream() { on = g_tty && EnvStr("UAGENT_MARKDOWN", "1") != "0"; }

void MdStream::Feed(const std::string& s) {
  std::string safe = TerminalSafe(s);
  if (!on) {
    fputs(safe.c_str(), stdout);
    FlushOutput(safe.size(), safe.find('\n') != std::string::npos);
    return;
  }
  for (char c : safe) Step(c);
  FlushOutput(safe.size(), safe.find('\n') != std::string::npos);
}

void MdStream::FeedPlain(const std::string& s) {
  std::string safe = TerminalSafe(s);
  fputs(safe.c_str(), stdout);
  FlushOutput(safe.size(), false, true);
}

void MdStream::Control(const char* s) {
  fputs(s, stdout);
  pending_output += strlen(s);
}

void MdStream::Flush() {  // stream end: resolve everything still held
  if (!on) {
    FlushOutput(0, false, true);
    return;
  }
  while (math) ReplayMath();
  if (star) Pv(star == 2 ? "**" : "*");
  if (dollar) Pc('$');
  if (slash) Pc('\\');
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
  math_text.clear();
  star = 0;
  dollar = math_dollar = slash = false;
  linestart = true;
  FlushOutput(0, false, true);
}

void MdStream::FlushOutput(size_t bytes, bool newline, bool force) {
  pending_output += bytes;
  auto now = std::chrono::steady_clock::now();
  if (force || newline || !output_started || pending_output >= kFlushBytes ||
      now - last_flush >= kFlushInterval) {
    fflush(stdout);
    pending_output = 0;
    output_started = true;
    last_flush = now;
  }
}

void MdStream::Pv(const std::string& s) {
  fputs(s.c_str(), stdout);
  vis_line += DisplayWidth(s);
}

void MdStream::Pc(char c) {
  putchar(c);
  if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) vis_line++;
}

void MdStream::EmitPre() {
  Pv(pre);
  pre.clear();
}

std::string MdStream::Marker() const {  // pre minus its leading indentation
  size_t sp = pre.find_first_not_of(" \t");
  return sp == std::string::npos ? "" : pre.substr(sp);
}

void MdStream::Step(char c) {
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
      ForgetPreviousLine();
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
      ForgetPreviousLine();
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

void MdStream::FenceClassify(
    char c) {  // inside a fence only "```" is structural
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

void MdStream::InlineChar(char c) {
  if (dollar) {
    dollar = false;
    if (c == '$') {
      math = 2;
      fputs(MATH(), stdout);
      math_text.clear();
      return;
    }
    if (c != '\n' && c != ' ' && c != '\t') {
      math = 1;
      fputs(MATH(), stdout);
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
      fputs(MATH(), stdout);
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

void MdStream::MathChar(char c) {
  if (slash) {
    slash = false;
    if ((math == 3 && c == ')') || (math == 4 && c == ']')) {
      FinishMath();
    } else {
      math_text += '\\';
      math_text += c;
    }
    return;
  }
  if (c == '\\') {
    slash = true;
    return;
  }
  if (math == 1 && c == '$') {
    if (!PlausibleInlineMath(math_text)) {
      math_text += '$';
      ReplayMath();
      return;
    }
    FinishMath();
    return;
  }
  if (math == 2) {
    if (math_dollar) {
      math_dollar = false;
      if (c == '$') {
        FinishMath();
        return;
      }
      math_text += '$';
    }
    if (c == '$') {
      math_dollar = true;
      return;
    }
  }
  if (c != '\n') {
    math_text += c;
    return;
  }
  // Inline math never spans lines; display math gives up at a blank line.
  // Either way the held text was not math after all — replay it verbatim.
  bool inline_span = math == 1 || math == 3;
  if (!inline_span && (math_text.empty() || math_text.back() != '\n')) {
    math_text += '\n';
    return;
  }
  do {
    ReplayMath();
  } while (math);
  EndLine();
}

void MdStream::FinishMath() {
  Pv(PrettyMath(math_text));
  math_text.clear();
  math = 0;
  dollar = math_dollar = slash = false;
  linestart = false;
  fputs(FgDfl(), stdout);
}

void MdStream::ReplayMath() {
  int kind = math;
  if (slash) math_text += '\\';
  if (math_dollar) math_text += '$';
  std::string text = std::move(math_text);
  math_text.clear();
  math = 0;
  dollar = math_dollar = slash = false;
  fputs(FgDfl(), stdout);
  Pv(MathOpen(kind));
  for (char value : text) InlineChar(value);
}

void MdStream::ForgetPreviousLine() {
  prev_raw.clear();
  prev_rows = 1;
}

void MdStream::EndLine() {  // inline styles never span lines
  if (bold || ital || code || heading) {
    fputs(RST(), stdout);
    bold = ital = code = heading = false;
  }
  putchar('\n');
  prev_raw = cur_raw;
  size_t columns = TerminalWidth();
  prev_rows = vis_line ? (vis_line - 1) / columns + 1 : 1;
  cur_raw.clear();
  vis_line = 0;
  linestart = true;
}

void MdStream::RetroTable() {
  for (size_t i = 0; i < prev_rows; ++i) {
    fputs("\033[A\033[2K", stdout);  // up + clear
  }
  table.push_back(prev_raw);
  table.push_back(Marker());
  pre.clear();
  cur_raw.clear();
  vis_line = 0;
  ForgetPreviousLine();
  tablemode = true;  // body rows follow until a line without '|'
}

void MdStream::FlushTable() {
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
      if (r != header && r > 0) putchar('\n');
      for (size_t c = 0; c < rows[r].size(); ++c) {
        std::string label = header < rows.size() && c < rows[header].size()
                                ? rows[header][c].first
                                : "Column " + std::to_string(c + 1);
        printf("%s%s:%s %s\n", DIM(), label.c_str(), RST(),
               rows[r][c].first.c_str());
      }
    }
    table.clear();
    ForgetPreviousLine();
    return;
  }
  for (size_t r = 0; r < rows.size(); r++) {
    if (sep[r]) continue;
    std::string ln;
    for (size_t c = 0; c < w.size(); c++) {
      auto cell = c < rows[r].size()
                      ? rows[r][c]
                      : std::make_pair(std::string(), static_cast<size_t>(0));
      if (c > 0) ln += "  ";
      size_t padding = c + 1 < w.size() ? w[c] - cell.second : 0;
      ln += cell.first + std::string(padding, ' ');
    }
    printf("%s%s%s\n", r == header ? BOLD() : "", ln.c_str(), RST());
  }
  table.clear();
  ForgetPreviousLine();
}

void MdPrint(const std::string& text) {
  MdStream stream;
  stream.Feed(text);
  stream.Flush();
}

}  // namespace uagent
