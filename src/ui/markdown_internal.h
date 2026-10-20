// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_UI_MARKDOWN_INTERNAL_H_
#define UAGENT_SRC_UI_MARKDOWN_INTERNAL_H_
// What the streaming renderer, its math and its tables share.

#include <cstddef>
#include <string>
#include <vector>

namespace uagent {

// Each accumulator holds model-supplied text that only ends when the model
// says so. Past these bounds the stream degrades to plain output rather than
// growing: the line stops being measured, the math span replays as literal
// text, the table renders the rows it has.
inline constexpr size_t kMdLineBytes = size_t{1} << 20;
inline constexpr size_t kMdMathBytes = size_t{64} * 1024;
inline constexpr size_t kMdTableRows = 2000;

bool MarkdownEscapable(char value);

// The end of the math span opening at `position` ($…$, $$…$$, \(…\), \[…\]),
// or npos when none does.
size_t MathSpan(const std::string& text, size_t position);
// A span's body without its delimiters, and that body as the terminal shows
// it.
std::string MathBody(const std::string& span);
std::string PrettyMath(const std::string& text);

// A table row's cells, split at the pipes outside code and math.
std::vector<std::string> SplitCells(const std::string& text);

}  // namespace uagent

#endif  // UAGENT_SRC_UI_MARKDOWN_INTERNAL_H_
