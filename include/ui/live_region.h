// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_LIVE_REGION_H_
#define UAGENT_INCLUDE_UI_LIVE_REGION_H_
// The rows at the bottom of the terminal that still change: the streaming
// tail, the status row and the composer, or a picker in its place. Above them
// is scrollback, written once. Flush repaints only the rows that changed, as
// one synchronized write.

#include <cstddef>
#include <string>
#include <vector>

namespace uagent {

class InteractiveOutput;

class LiveRegion {
 public:
  explicit LiveRegion(const InteractiveOutput& output) : output_(output) {}

  // Finished text for scrollback. Text that finishes the tail starts with it,
  // as the transcript commits it; what the tail already scrolled away is not
  // written twice.
  void Commit(std::string text);
  void SetTail(std::string text);
  void SetStatus(std::string row);
  void SetComposer(std::vector<std::string> rows, size_t caret_row,
                   size_t caret_column);
  // Stands in for the composer while a picker asks; empty gives it back.
  void SetOverlay(std::string text);
  void Flush();
  // Erases the region before something else owns the screen; the next Flush
  // draws it anew wherever the cursor then is.
  void Clear();

 private:
  // Back to the frame's top row, counting the rows a resize reflowed.
  std::string ToTop(size_t width) const;

  const InteractiveOutput& output_;
  std::string committed_, tail_, status_, overlay_;
  size_t emitted_ = 0;  // bytes of the tail already in scrollback
  // The tail as last drawn, and its rows at the frame's top; a commit that
  // finishes it adopts those rows in place instead of writing them again.
  std::string drawn_tail_;
  size_t drawn_tail_rows_ = 0, adopted_ = 0;
  std::vector<std::string> composer_;
  size_t composer_row_ = 0, composer_column_ = 0;
  std::vector<std::string> drawn_;  // the frame on screen, top to bottom
  size_t cursor_row_ = 0, cursor_column_ = 0;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_LIVE_REGION_H_
