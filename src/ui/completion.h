// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_UI_COMPLETION_H_
#define UAGENT_SRC_UI_COMPLETION_H_
// What Tab completes in the composer and the rows it lists below the draft.

#include <cstddef>
#include <string>
#include <vector>

namespace uagent {

// One completion candidate and the span of the draft it replaces. Two sources
// share this: slash commands, which replace the whole line, and `@paths`,
// which replace only the token under the cursor.
struct Suggestion {
  std::string name;
  std::string description;
  bool wants_argument = false;
};

struct Suggestions {
  size_t begin = 0;
  size_t end = 0;
  std::vector<Suggestion> matches;
};

Suggestions CompletionMatches(const std::string& buffer, size_t cursor);

}  // namespace uagent

#endif  // UAGENT_SRC_UI_COMPLETION_H_
