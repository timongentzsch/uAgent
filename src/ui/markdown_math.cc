// Copyright 2026 Timon Gentzsch

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/math.h"
#include "include/core/term.h"
#include "include/ui/markdown.h"
#include "src/ui/markdown_internal.h"

namespace uagent {
namespace {

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

}  // namespace

size_t MathSpan(const std::string& text, size_t position) {
  if (Escaped(text, position)) return std::string::npos;
  std::string_view close;
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

std::string PrettyMath(const std::string& text) {
  // Transliteration produces the same Unicode the fallback would have to undo,
  // so a terminal that cannot decode it keeps the source spelling instead.
  if (!g_unicode) return text;
  return TransliterateMath(text);
}

std::string MathBody(const std::string& span) {
  if (span.starts_with("$$")) return span.substr(2, span.size() - 4);
  if (span.starts_with("\\(") || span.starts_with("\\[")) {
    return span.substr(2, span.size() - 4);
  }
  return span.substr(1, span.size() - 2);
}

void MdStream::MathChar(char c) {
  // An unterminated span would otherwise buffer the rest of the response.
  if (math_text.size() >= kMdMathBytes) {
    ReplayMath();
    InlineChar(c);
    return;
  }
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
  Put(FgDfl());
}

void MdStream::ReplayMath() {
  int kind = math;
  if (slash) math_text += '\\';
  if (math_dollar) math_text += '$';
  std::string text = std::move(math_text);
  math_text.clear();
  math = 0;
  dollar = math_dollar = slash = false;
  Put(FgDfl());
  Pv(MathOpen(kind));
  for (char value : text) InlineChar(value);
}

}  // namespace uagent
