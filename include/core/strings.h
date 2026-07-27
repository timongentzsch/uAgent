// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STRINGS_H_
#define UAGENT_INCLUDE_CORE_STRINGS_H_
// String, UTF-8, and display-width helpers, plus the terminal-safe and
// human-readable formatters shared by the REPL and the tool trace.

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/term.h"

namespace uagent {

inline std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

inline void ReplaceAll(std::string& s, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return;
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
       pos += to.size()) {
    s.replace(pos, from.size(), to);
  }
}

// drop one layer of matching surrounding quotes, if present
inline std::string Unquote(std::string s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  return s;
}

// wrap for /bin/sh: single-quote everything, closing and re-opening around a
// literal quote ('it'\''s'), so no byte of the payload can reach the shell.
inline std::string ShellQuote(const std::string& s) {
  std::string q = "'";
  for (char c : s) c == '\'' ? q += "'\\''" : q += c;
  return q + "'";
}

// Entries are returned as written, empties included: PATH reads an empty entry
// as the working directory, while a search path skips it.
inline std::vector<std::string> SplitPathList(const std::string& value,
                                              char separator = ':') {
  std::vector<std::string> entries;
  for (size_t begin = 0;;) {
    size_t end = value.find(separator, begin);
    entries.push_back(end == std::string::npos
                          ? value.substr(begin)
                          : value.substr(begin, end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return entries;
}

inline bool ExecutableOnPath(const std::string& name) {
  const char* path_value = getenv("PATH");
  if (!path_value || name.empty()) return false;
  for (std::string directory : SplitPathList(path_value)) {
    if (directory.empty()) directory = ".";
    if (access((directory + "/" + name).c_str(), X_OK) == 0) return true;
  }
  return false;
}

inline size_t Utf8BoundaryBefore(const std::string& s, size_t offset) {
  offset = std::min(offset, s.size());
  while (offset > 0 && offset < s.size() &&
         (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) {
    --offset;
  }
  return offset;
}

inline size_t Utf8BoundaryAfter(const std::string& s, size_t offset) {
  offset = std::min(offset, s.size());
  while (offset < s.size() &&
         (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) {
    ++offset;
  }
  return offset;
}

// cap a string at a UTF-8 boundary (never splits a codepoint), appending "…"
inline std::string Utf8Prefix(std::string s, size_t cap) {
  if (s.size() <= cap) return s;
  s.resize(Utf8BoundaryBefore(s, cap));
  return s;
}

inline std::string Utf8Trunc(std::string s, size_t cap) {
  if (s.size() <= cap) return s;
  return Utf8Prefix(std::move(s), cap) + "…";
}

// Terminal column width for valid UTF-8 in the active locale. Invalid or
// incomplete sequences degrade to one column rather than breaking rendering.
inline size_t DisplayWidth(const std::string& s) {
  std::mbstate_t state{};
  size_t width = 0;
  for (size_t offset = 0; offset < s.size();) {
    wchar_t wide = 0;
    size_t consumed =
        std::mbrtowc(&wide, s.data() + offset, s.size() - offset, &state);
    if (consumed == static_cast<size_t>(-1) ||
        consumed == static_cast<size_t>(-2)) {
      state = {};
      ++offset;
      ++width;
      continue;
    }
    if (consumed == 0) consumed = 1;
    int columns = ::wcwidth(wide);
    if (columns > 0) width += static_cast<size_t>(columns);
    offset += consumed;
  }
  return width;
}

inline size_t JsonEstimatedBytes(const json& value) {
  if (value.is_string()) return value.get_ref<const std::string&>().size() + 2;
  size_t total = 16;
  if (value.is_array()) {
    for (const json& item : value) total += JsonEstimatedBytes(item);
  } else if (value.is_object()) {
    for (const auto& [key, item] : value.items()) {
      total += key.size() + JsonEstimatedBytes(item);
    }
  }
  return total;
}

// first line of a string, capped — for one-line previews
inline std::string StripTrailingSlashes(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

inline std::string OneLine(const std::string& s, size_t cap = 80) {
  return Utf8Trunc(s.substr(0, s.find('\n')), cap);
}

// Model, tool and MCP text is untrusted terminal input. Preserve normal text,
// tabs and newlines but render control bytes visibly instead of letting them
// execute terminal commands. Piped output is not a terminal and remains exact.
inline std::string TerminalSafe(const std::string& s) {
  if (!g_tty) return s;
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c == '\n' || c == '\t' || c >= 0x20) {
      if (c == 0x7f) {
        out += "\\x7f";
      } else {
        out += static_cast<char>(c);
      }
    } else if (c == '\r') {
      out += "\\r";
    } else {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

inline std::string FmtTokens(int64_t n) {
  if (n < 1000) return std::to_string(n);
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << static_cast<double>(n) / 1000.0
      << 'K';
  return out.str();
}

inline std::string FmtCost(double c) {
  std::ostringstream out;
  out << '$' << std::fixed << std::setprecision(c < 1.0 ? 4 : 2) << c;
  return out.str();
}

// coarse "how long ago", for the session picker
inline std::string FmtAgo(int64_t seconds) {
  if (seconds < 60) return "just now";
  int64_t m = seconds / 60, h = m / 60, d = h / 24;
  if (d > 0) return std::to_string(d) + "d ago";
  if (h > 0) return std::to_string(h) + "h ago";
  return std::to_string(m) + "m ago";
}

inline int64_t TerminalColumns() {
  struct winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    return size.ws_col;
  }
  return std::max(int64_t{1}, EnvLong("COLUMNS", 80));
}

// Fit text into the rest of the row. Callers pass the decoration itself, not a
// hand-counted column budget, so the two cannot drift.
inline std::string TerminalFit(const std::string& text,
                               const std::string& prefix = "",
                               const std::string& suffix = "") {
  int64_t reserve =
      static_cast<int64_t>(DisplayWidth(prefix) + DisplayWidth(suffix));
  return OneLine(text, static_cast<size_t>(std::max(
                           int64_t{1}, TerminalColumns() - reserve - 2)));
}

inline uint64_t Fnv1aUpdate(uint64_t hash, const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::string Hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

inline std::string UrlHost(std::string url) {
  if (auto p = url.find("://"); p != std::string::npos) {
    url = url.substr(p + 3);
  }
  if (auto p = url.find('/'); p != std::string::npos) {
    url.resize(p);
  }
  if (auto p = url.rfind('@'); p != std::string::npos) {
    url = url.substr(p + 1);
  }
  if (auto p = url.find(':'); p != std::string::npos) {
    url.resize(p);
  }
  std::transform(url.begin(), url.end(), url.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return url;
}

inline bool OpenrouterUrl(std::string url) {
  url = UrlHost(std::move(url));
  constexpr const char* kSuffix = ".openrouter.ai";
  return url == "openrouter.ai" || (url.size() > strlen(kSuffix) &&
                                    url.compare(url.size() - strlen(kSuffix),
                                                strlen(kSuffix), kSuffix) == 0);
}

inline bool OpenaiUrl(std::string url) {
  return UrlHost(std::move(url)) == "api.openai.com";
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STRINGS_H_
