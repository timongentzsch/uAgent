// Copyright 2026 Timon Gentzsch

#include "src/ui/completion.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "include/cli.h"

namespace uagent {

namespace {

// What a partially typed "/word" could still become; aliases stay hidden.
// The letters may be spread out ("/mdl" finds /model), but a name that starts
// with what was typed ranks first.
Suggestions SlashMatches(const std::string& buffer) {
  Suggestions found{0, buffer.size(), {}, true};
  if (buffer.empty() || buffer[0] != '/' ||
      buffer.find(' ') != std::string::npos) {
    return found;
  }
  std::vector<Suggestion> scattered;
  for (const SlashCommandSpec& command : SlashCommandRegistry()) {
    if (!*command.description) continue;
    const std::string_view name = command.name;
    size_t at = 0;
    for (char ch : name) at += at < buffer.size() && buffer[at] == ch;
    if (at < buffer.size()) continue;
    (name.starts_with(buffer) ? found.matches : scattered)
        .push_back({command.name, command.description, *command.argument != 0});
  }
  found.matches.insert(found.matches.end(), scattered.begin(), scattered.end());
  return found;
}

// A path is completed one segment at a time, from the directory the token
// already names -- the way a shell does it. Listing one directory per keypress
// needs no index, no cache to invalidate and no subprocess, and it cannot
// offer a build tree the repository ignores unless the typist walked into one.
Suggestions PathMatches(const std::string& buffer, size_t cursor) {
  Suggestions found{cursor, cursor, {}};
  size_t at = buffer.rfind('@', cursor == 0 ? 0 : cursor - 1);
  if (at == std::string::npos || at >= cursor) return found;
  // Only at a word boundary: an email address or a decorator is not a path.
  if (at > 0 && std::isspace(static_cast<unsigned char>(buffer[at - 1])) == 0) {
    return found;
  }
  std::string typed = buffer.substr(at + 1, cursor - at - 1);
  if (typed.find_first_of(" \t\n") != std::string::npos) return found;
  found.begin = at;

  size_t slash = typed.rfind('/');
  std::string parent =
      slash == std::string::npos ? "" : typed.substr(0, slash + 1);
  std::string prefix =
      slash == std::string::npos ? typed : typed.substr(slash + 1);

  namespace fs = std::filesystem;
  std::error_code error;
  // A cap, not a page: the whole set is what the shared prefix is computed
  // from, while only the first few are ever drawn.
  constexpr size_t kCandidateCap = 256;
  for (fs::directory_iterator it(parent.empty() ? "." : parent, error), end;
       it != end && !error && found.matches.size() < kCandidateCap;
       it.increment(error)) {
    std::string name = it->path().filename().string();
    if (!std::string_view(name).starts_with(prefix)) continue;
    // Hidden entries stay hidden until the typist asks for one by name.
    if (name.starts_with(".") && !prefix.starts_with(".")) continue;
    std::error_code kind_error;
    if (fs::is_directory(it->status(kind_error)) && !kind_error) name += "/";
    found.matches.push_back({"@" + parent + name, "", false});
  }
  std::sort(
      found.matches.begin(), found.matches.end(),
      [](const Suggestion& a, const Suggestion& b) { return a.name < b.name; });
  return found;
}

}  // namespace

Suggestions CompletionMatches(const std::string& buffer, size_t cursor) {
  Suggestions slash = SlashMatches(buffer);
  if (!slash.matches.empty()) return slash;
  return PathMatches(buffer, cursor);
}

}  // namespace uagent
