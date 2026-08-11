// Copyright 2026 Timon Gentzsch

#include "include/core/strings.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/env.h"
#include "include/core/term.h"

namespace uagent {

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

void ReplaceAll(std::string& s, const std::string& from,
                const std::string& to) {
  if (from.empty()) return;
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
       pos += to.size()) {
    s.replace(pos, from.size(), to);
  }
}

std::string AsciiLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

bool ContainsCaseInsensitive(std::string text, const std::string& query) {
  return query.empty() || AsciiLower(std::move(text)).find(AsciiLower(query)) !=
                              std::string::npos;
}

bool ParseInt64(const char* text, int64_t& value) {
  if (!text || !*text) return false;
  char* end = nullptr;
  errno = 0;
  int64_t parsed = strtoll(text, &end, 10);
  if (errno || !end || *end) return false;
  value = static_cast<int64_t>(parsed);
  return true;
}

bool ParseFiniteDouble(const char* text, double& value) {
  if (!text || !*text) return false;
  char* end = nullptr;
  errno = 0;
  double parsed = strtod(text, &end);
  if (errno || !end || *end || !std::isfinite(parsed)) return false;
  value = parsed;
  return true;
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  return s;
}

std::string ShellQuote(const std::string& s) {
  std::string q = "'";
  for (char c : s) c == '\'' ? q += "'\\''" : q += c;
  return q + "'";
}

std::vector<std::string> SplitPathList(const std::string& value,
                                       char separator) {
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

size_t Utf8BoundaryBefore(const std::string& s, size_t offset) {
  offset = std::min(offset, s.size());
  while (offset > 0 && offset < s.size() &&
         (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) {
    --offset;
  }
  return offset;
}

size_t Utf8BoundaryAfter(const std::string& s, size_t offset) {
  offset = std::min(offset, s.size());
  while (offset < s.size() &&
         (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) {
    ++offset;
  }
  return offset;
}

std::string Utf8Prefix(std::string s, size_t cap) {
  if (s.size() <= cap) return s;
  s.resize(Utf8BoundaryBefore(s, cap));
  return s;
}

std::string Utf8Trunc(std::string s, size_t cap) {
  if (s.size() <= cap) return s;
  return Utf8Prefix(std::move(s), cap) + "…";
}

namespace {

struct Glyph {
  size_t bytes = 1;
  size_t width = 1;
};

// Decode one terminal glyph. CSI escape sequences consume bytes at zero
// width; invalid or incomplete UTF-8 degrades to one column per byte.
Glyph NextGlyph(const std::string& s, size_t offset, std::mbstate_t& state,
                bool skip_ansi) {
  if (skip_ansi && s[offset] == '\x1b' && offset + 1 < s.size() &&
      s[offset + 1] == '[') {
    size_t end = offset + 2;
    while (end < s.size()) {
      unsigned char value = static_cast<unsigned char>(s[end++]);
      if (value >= 0x40 && value <= 0x7e) break;
    }
    return {end - offset, 0};
  }
  wchar_t wide = 0;
  size_t consumed =
      std::mbrtowc(&wide, s.data() + offset, s.size() - offset, &state);
  if (consumed == static_cast<size_t>(-1) ||
      consumed == static_cast<size_t>(-2)) {
    state = {};
    return {1, 1};
  }
  return {consumed ? consumed : 1,
          static_cast<size_t>(std::max(0, ::wcwidth(wide)))};
}

}  // namespace

size_t DisplayWidth(const std::string& s) {
  std::mbstate_t state{};
  size_t width = 0;
  for (size_t offset = 0; offset < s.size();) {
    Glyph glyph = NextGlyph(s, offset, state, /*skip_ansi=*/true);
    width += glyph.width;
    offset += glyph.bytes;
  }
  return width;
}

std::string DisplayTrunc(std::string s, size_t columns) {
  if (DisplayWidth(s) <= columns) return s;
  if (columns == 0) return "";
  size_t limit = columns - 1;  // reserve one column for …
  std::mbstate_t state{};
  size_t offset = 0, width = 0;
  while (offset < s.size()) {
    Glyph glyph = NextGlyph(s, offset, state, /*skip_ansi=*/true);
    if (width + glyph.width > limit) break;
    width += glyph.width;
    offset += glyph.bytes;
  }
  return s.substr(0, offset) + "…";
}

size_t JsonEstimatedBytes(const json& value) {
  if (value.is_string()) {
    return SaturatingAdd(value.get_ref<const std::string&>().size(), 2);
  }
  size_t total = 16;
  if (value.is_array()) {
    for (const json& item : value) {
      total = SaturatingAdd(total, JsonEstimatedBytes(item));
    }
  } else if (value.is_object()) {
    for (const auto& [key, item] : value.items()) {
      total = SaturatingAdd(total, key.size());
      total = SaturatingAdd(total, JsonEstimatedBytes(item));
    }
  }
  return total;
}

int64_t EstimatedTokens(size_t bytes) {
  size_t tokens = bytes / 4;
  size_t cap = static_cast<size_t>(std::numeric_limits<int64_t>::max());
  return tokens > cap ? std::numeric_limits<int64_t>::max()
                      : static_cast<int64_t>(tokens);
}

std::string StripTrailingSlashes(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::vector<std::string> WrapLines(const std::string& s, size_t columns) {
  std::vector<std::string> rows;
  if (s.empty() || columns == 0) {
    rows.push_back(s);
    return rows;
  }
  std::mbstate_t state{};
  std::string current;
  size_t current_width = 0;
  for (size_t offset = 0; offset < s.size();) {
    Glyph glyph = NextGlyph(s, offset, state, /*skip_ansi=*/false);
    // A single wide glyph wider than the row must not infinite-loop, so put it
    // on a row of its own even if it overflows columns.
    if (current_width + glyph.width > columns && !current.empty()) {
      rows.push_back(current);
      current.clear();
      current_width = 0;
    }
    current.append(s, offset, glyph.bytes);
    current_width += glyph.width;
    offset += glyph.bytes;
  }
  if (!current.empty() || rows.empty()) rows.push_back(current);
  return rows;
}

std::string FirstLine(const std::string& s) {
  return s.substr(0, s.find('\n'));
}

std::string OneLine(const std::string& s, size_t cap) {
  return Utf8Trunc(FirstLine(s), cap);
}

std::string TerminalSafe(const std::string& s) {
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

std::string TerminalSummary(const std::string& text, size_t reserved_columns) {
  std::string summary = TerminalSafe(FirstLine(text));
  size_t newline = text.find('\n');
  if (newline != std::string::npos && newline + 1 < text.size()) {
    summary += " …";
  }
  size_t width = static_cast<size_t>(std::max(
      int64_t{1}, TerminalColumns() - static_cast<int64_t>(reserved_columns)));
  return DisplayTrunc(std::move(summary), width);
}

std::string SpinnerLabel(const std::string& label) {
  size_t columns =
      static_cast<size_t>(std::max(int64_t{1}, TerminalColumns() - 12));
  return DisplayTrunc(TerminalSafe(label), columns);
}

uint64_t Fnv1aUpdate(uint64_t hash, const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string UrlHost(std::string url) {
  std::string host = UrlAuthority(std::move(url));
  if (auto p = host.find(':'); p != std::string::npos) {
    host.resize(p);
  }
  return host;
}

bool OpenrouterUrl(std::string url) {
  url = UrlHost(std::move(url));
  constexpr std::string_view kSuffix = ".openrouter.ai";
  return url == "openrouter.ai" ||
         (url.size() > kSuffix.size() && url.ends_with(kSuffix));
}

bool LoopbackUrl(std::string url) {
  std::string host = UrlHost(std::move(url));
  return host == "127.0.0.1" || host == "localhost";
}

bool OpenaiUrl(std::string url) {
  return UrlHost(std::move(url)) == "api.openai.com";
}

std::string UrlAuthority(std::string url) {
  if (auto pos = url.find("://"); pos != std::string::npos) {
    url = url.substr(pos + 3);
  }
  if (auto pos = url.find('/'); pos != std::string::npos) {
    url.resize(pos);
  }
  if (auto pos = url.rfind('@'); pos != std::string::npos) {
    url = url.substr(pos + 1);
  }
  return AsciiLower(std::move(url));
}

std::string ModelLabel(const std::string& model, const std::string& effort) {
  return model + " (" + (effort.empty() ? "default" : effort) + ")";
}

std::string RouteKey(const std::string& base_url, const std::string& provider,
                     const std::string& model, const std::string& effort) {
  return UrlAuthority(base_url) + "|" + provider + "|" + model + "|" + effort;
}

bool ExecutableOnPath(const std::string& name) {
  const char* path_value = getenv("PATH");
  if (!path_value || name.empty()) return false;
  for (std::string directory : SplitPathList(path_value)) {
    if (directory.empty()) directory = ".";
    if (access((directory + "/" + name).c_str(), X_OK) == 0) return true;
  }
  return false;
}

std::string FmtCount(int64_t number) {
  if (number < 1000) return std::to_string(number);
  const char* suffix = "K";
  double divisor = 1000.0;
  if (number >= 1'000'000'000) {
    suffix = "B";
    divisor = 1'000'000'000.0;
  } else if (number >= 1'000'000) {
    suffix = "M";
    divisor = 1'000'000.0;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(1)
         << static_cast<double>(number) / divisor << suffix;
  return output.str();
}

std::string FmtCost(double cost) {
  std::ostringstream output;
  output << '$' << std::fixed << std::setprecision(cost < 1.0 ? 4 : 2) << cost;
  return output.str();
}

std::string FmtAgo(int64_t seconds) {
  if (seconds < 60) return "just now";
  int64_t minutes = seconds / 60;
  int64_t hours = minutes / 60;
  int64_t days = hours / 24;
  if (days > 0) return std::to_string(days) + "d ago";
  if (hours > 0) return std::to_string(hours) + "h ago";
  return std::to_string(minutes) + "m ago";
}

int64_t TerminalColumns() {
  struct winsize size{};
  for (int fd : {STDOUT_FILENO, STDIN_FILENO}) {
    if (ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
      return size.ws_col;
    }
  }
  return std::max(int64_t{1}, EnvLong("COLUMNS", 80));
}

std::string Hex64(uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

}  // namespace uagent
