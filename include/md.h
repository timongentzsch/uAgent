// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MD_H_
#define UAGENT_INCLUDE_MD_H_
// Streaming Markdown-to-ANSI state. The parser implementation is compiled once
// in src/ui/markdown.cc; the terminal presenter owns this value type.

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uagent {

class MdStream {
 public:
  MdStream();

  void Feed(std::string_view text);
  void FeedPlain(std::string_view text);
  void Control(const char* sequence);
  void Flush();

 private:
  bool on;
  bool bold = false, ital = false, code = false;
  int math = 0;
  bool dollar = false, math_dollar = false, slash = false;
  std::string math_text;
  int star = 0;
  bool linestart = true;
  // Blank lines are withheld until text follows: a run of them collapses to
  // one, and a run at the end of the stream disappears. Models routinely end
  // a message with several, and each one is a wasted terminal row.
  size_t blank_held = 0;
  std::string pre;
  bool heading = false;
  bool fence = false;
  bool fencehead = false;
  bool intable = false;
  bool tablemode = false;
  std::string row;
  std::vector<std::string> table;
  std::string cur_raw, prev_raw;
  size_t prev_rows = 1;
  size_t pending_output = 0;
  bool output_started = false;
  std::chrono::steady_clock::time_point last_flush =
      std::chrono::steady_clock::now();

  void FlushOutput(size_t bytes, bool newline, bool force = false);
  void Pv(const std::string& text);
  void Pc(char value);
  void EmitPre();
  std::string_view Marker() const;
  void Step(char value);
  void Classify(char value);
  void FenceClassify(char value);
  void InlineChar(char value);
  void MathChar(char value);
  void FinishMath();
  void ReplayMath();
  void EndLine();
  void ReleaseBlanks();
  // prev_raw and prev_rows describe the same line and are always reset
  // together.
  void ForgetPreviousLine();
  void RetroTable();
  void FlushTable();
};

void MdPrint(const std::string& text);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MD_H_
