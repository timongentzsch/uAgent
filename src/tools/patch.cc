// Copyright 2026 Timon Gentzsch

#include "include/tools/patch.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr std::string_view kBegin = "*** Begin Patch";
constexpr std::string_view kEnd = "*** End Patch";
constexpr std::string_view kAdd = "*** Add File: ";
constexpr std::string_view kDelete = "*** Delete File: ";
constexpr std::string_view kUpdate = "*** Update File: ";
constexpr std::string_view kEndOfFile = "*** End of File";

std::string Joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    if (!text.empty()) text += '\n';
    text += line;
  }
  return text;
}

// A hunk as one exact replacement; false when it has nothing to find.
bool HunkEdit(const std::string& anchor, std::vector<std::string>& before,
              std::vector<std::string>& after, bool changed, json& edits) {
  if (!changed) return true;
  std::string old_text = Joined(before), new_text = Joined(after);
  if (before.empty()) {
    // Lines added under the line the hunk names.
    if (anchor.empty()) return false;
    old_text = anchor;
    new_text = anchor + "\n" + new_text;
  } else if (after.empty()) {
    // Whole lines go with their line break.
    old_text += '\n';
  }
  edits.push_back({{"old", std::move(old_text)}, {"new", std::move(new_text)}});
  return true;
}

}  // namespace

std::vector<ToolPart> ParsePatch(const std::string& patch, std::string& error) {
  std::vector<std::string> lines;
  for (std::string_view rest = patch; !rest.empty();) {
    const size_t end = rest.find('\n');
    std::string_view line = rest.substr(0, end);
    if (line.ends_with('\r')) line.remove_suffix(1);
    lines.emplace_back(line);
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
  }
  std::vector<ToolPart> calls;
  auto fail = [&](size_t line, const std::string& why) {
    error = "patch line " + std::to_string(line + 1) + ": " + why;
    return std::vector<ToolPart>{};
  };
  size_t at = 0;
  while (at < lines.size() && Trim(lines[at]).empty()) ++at;
  if (at == lines.size() || lines[at] != kBegin) {
    return fail(at, "a patch starts with `*** Begin Patch`");
  }
  auto section = [&] {
    return at >= lines.size() || lines[at].starts_with("*** ");
  };
  for (++at; at < lines.size() && lines[at] != kEnd;) {
    const std::string& head = lines[at];
    const size_t start = at++;
    if (head.starts_with(kAdd)) {
      std::string content;
      for (; !section(); ++at) {
        if (!lines[at].starts_with('+')) {
          return fail(at, "a line of a new file starts with `+`");
        }
        content += lines[at].substr(1) + "\n";
      }
      calls.push_back(
          {"write_file",
           {{"path", Trim(head.substr(kAdd.size()))}, {"content", content}}});
    } else if (head.starts_with(kDelete)) {
      calls.push_back(
          {"delete_file", {{"path", Trim(head.substr(kDelete.size()))}}});
    } else if (head.starts_with(kUpdate)) {
      json edits = json::array();
      std::string anchor;
      std::vector<std::string> before, after;
      bool changed = false;
      auto close = [&] {
        const bool found = HunkEdit(anchor, before, after, changed, edits);
        before.clear();
        after.clear();
        changed = false;
        return found;
      };
      for (; at < lines.size() &&
             (lines[at] == kEndOfFile || !lines[at].starts_with("*** "));
           ++at) {
        const std::string& line = lines[at];
        if (line == kEndOfFile) continue;
        if (line.starts_with("@@")) {
          if (!close()) return fail(at, "the hunk before has no context");
          anchor = Trim(line.substr(2));
        } else if (line.empty() || line[0] == ' ') {
          // A blank context line often arrives without its space.
          before.push_back(line.empty() ? "" : line.substr(1));
          after.push_back(before.back());
        } else if (line[0] == '-') {
          before.push_back(line.substr(1));
          changed = true;
        } else if (line[0] == '+') {
          after.push_back(line.substr(1));
          changed = true;
        } else {
          return fail(at, "a hunk line starts with a space, `-` or `+`");
        }
      }
      if (!close()) return fail(start, "a hunk needs context or `@@ LINE`");
      if (edits.empty()) return fail(start, "this file has no change");
      calls.push_back(
          {"edit_file",
           {{"path", Trim(head.substr(kUpdate.size()))}, {"edits", edits}}});
    } else {
      return fail(start,
                  "expected `*** Add File: `, `*** Update File: `, "
                  "`*** Delete File: ` or `*** End Patch`");
    }
  }
  if (at == lines.size()) return fail(at, "a patch ends with `*** End Patch`");
  if (calls.empty()) return fail(at, "the patch changes no file");
  return calls;
}

}  // namespace uagent
