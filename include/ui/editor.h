// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_UI_EDITOR_H_
#define UAGENT_INCLUDE_UI_EDITOR_H_
#include <string>
namespace uagent {
// Caller owns cooked terminal mode. Failed/cancelled editors preserve input.
bool EditExternalText(std::string& text, int terminal_fd, size_t limit);
}  // namespace uagent
#endif
