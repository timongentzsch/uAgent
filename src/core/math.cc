// Copyright 2026 Timon Gentzsch
#include "include/core/math.h"

#include <cctype>
#include <string>
#include <string_view>
namespace uagent {
namespace {
bool TakeGroup(std::string_view text, size_t& pos, std::string& out) {
  if (pos >= text.size() || text[pos] != '{') return false;
  size_t orig = pos;
  size_t beg = ++pos;
  int depth = 1;
  while (pos < text.size() && depth > 0) {
    if (text[pos] == '{') ++depth;
    if (text[pos] == '}') --depth;
    ++pos;
  }
  if (depth != 0) {
    pos = orig;
    return false;
  }
  out = std::string(text.substr(beg, pos - beg - 1));
  return true;
}
std::string Pretty(std::string_view text);
std::string Script(std::string_view text, bool sup) {
  constexpr std::string_view kPlain = "0123456789+-=()ni";
  constexpr std::string_view kSuper[] = {"⁰", "¹", "²", "³", "⁴", "⁵",
                                         "⁶", "⁷", "⁸", "⁹", "⁺", "⁻",
                                         "⁼", "⁽", "⁾", "ⁿ", "ⁱ"};
  constexpr std::string_view kSub[] = {"₀", "₁", "₂", "₃", "₄", "₅",
                                       "₆", "₇", "₈", "₉", "₊", "₋",
                                       "₌", "₍", "₎", "ₙ", "ᵢ"};
  std::string out;
  for (char value : text) {
    size_t index = kPlain.find(value);
    if (index == std::string_view::npos) {
      return std::string(sup ? "^(" : "_(") + Pretty(text) + ")";
    }
    out += sup ? kSuper[index] : kSub[index];
  }
  return out;
}
std::string Pretty(std::string_view text) {
  std::string out;
  bool spaced = false;
  for (size_t i = 0; i < text.size();) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    if (std::isspace(ch) || ch == '~') {
      if (!out.empty() && !spaced) out += ' ';
      spaced = true;
      ++i;
      continue;
    }
    spaced = false;
    if (text[i] == '\\') {
      size_t begin = ++i;
      while (i < text.size() &&
             std::isalpha(static_cast<unsigned char>(text[i]))) {
        ++i;
      }
      std::string_view cmd = text.substr(begin, i - begin);
      if (cmd == "frac") {
        size_t groups = i;
        std::string num;
        std::string den;
        if (TakeGroup(text, i, num) && TakeGroup(text, i, den)) {
          out += "(" + Pretty(num) + ")⁄(" + Pretty(den) + ")";
          continue;
        }
        i = groups;
      } else if (cmd == "sqrt") {
        std::string rad;
        if (TakeGroup(text, i, rad)) {
          out += "√(" + Pretty(rad) + ")";
          continue;
        }
      } else if (cmd == "text" || cmd == "mathrm" || cmd == "operatorname") {
        std::string grp;
        if (TakeGroup(text, i, grp)) {
          out += grp;
          continue;
        }
      } else if (cmd == "left" || cmd == "right") {
        continue;
      } else if (cmd == "quad" || cmd == "qquad") {
        if (!out.empty() && out.back() != ' ') out += ' ';
        continue;
      }
      std::string rendered = MathCommand(cmd);
      if (!rendered.empty()) {
        out += rendered;
        continue;
      }
      if (cmd.empty() && i < text.size()) {
        out += text[i++];
      } else {
        out += '\\';
        out += cmd;
      }
      continue;
    }
    if (text[i] == '^' || text[i] == '_') {
      bool sup = text[i++] == '^';
      std::string script;
      if (!TakeGroup(text, i, script) && i < text.size()) {
        size_t begin = i++;
        if (text[begin] == '\\') {
          while (i < text.size() &&
                 std::isalpha(static_cast<unsigned char>(text[i]))) {
            ++i;
          }
        }
        script = std::string(text.substr(begin, i - begin));
      }
      out += Script(script, sup);
      continue;
    }
    out += text[i++];
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
}  // namespace
std::string TransliterateMath(std::string_view raw) { return Pretty(raw); }
}  // namespace uagent
