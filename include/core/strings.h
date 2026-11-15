// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STRINGS_H_
#define UAGENT_INCLUDE_CORE_STRINGS_H_
// String, UTF-8 and display-width helpers, and terminal-safe formatters.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uagent {

// `char` is signed here: byte-wise tests go through this.
inline unsigned char Byte(char value) {
  return static_cast<unsigned char>(value);
}

std::string Trim(const std::string& s);

void ReplaceAll(std::string& s, const std::string& from, const std::string& to);

std::string AsciiLower(std::string text);

// Whether text holds query, ignoring the case of ASCII letters.
bool ContainsCaseInsensitive(std::string_view text, std::string_view query);

bool ParseInt64(const char* text, int64_t& value);

bool ParseFiniteDouble(const char* text, double& value);

// 1/true/yes/on and 0/false/no/off, case-insensitive. Anything else leaves
// `value` untouched and returns false.
bool ParseBool(std::string_view text, bool& value);

// drop one layer of matching surrounding quotes, if present
std::string Unquote(std::string s);

// wrap for /bin/sh: single-quote everything, closing and re-opening around a
// literal quote ('it'\''s'), so no byte of the payload can reach the shell.
std::string ShellQuote(const std::string& s);
// Zeroes a secret's bytes before clearing it.
void WipeString(std::string& s);

// Entries are returned as written, empties included: PATH reads an empty entry
// as the working directory, while a search path skips it.
std::vector<std::string> SplitPathList(const std::string& value,
                                       char separator = ':');

bool ExecutableOnPath(const std::string& name);

size_t Utf8BoundaryBefore(const std::string& s, size_t offset);

size_t Utf8BoundaryAfter(const std::string& s, size_t offset);

// Cap a string at a UTF-8 boundary without splitting a codepoint.
std::string Utf8Prefix(std::string s, size_t cap);

// `text` with every byte but RFC 3986's unreserved characters (letters,
// digits, -._~) and those in `keep` written as %XX.
std::string PercentEncode(std::string_view text, std::string_view keep = "");

// The same bound with an ellipsis when truncation is required.
std::string Utf8Trunc(std::string s, size_t cap);

// The bytes of the CSI escape sequence `text` starts with (ESC [, 0x20-0x3f
// bytes, one final 0x40-0x7e byte); 0 for none or an incomplete one.
size_t CsiSpan(std::string_view text);

// Characters in UTF-8 text; never more than a browser's maxLength counts.
inline size_t Utf8Chars(std::string_view text) {
  return static_cast<size_t>(std::count_if(
      text.begin(), text.end(),
      [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// Terminal columns in the active locale; an invalid sequence counts as one.
size_t DisplayWidth(const std::string& s);

// Physical rows at a fixed width; SGR is zero-width, tabs stop every eight.
size_t DisplayRows(const std::string& s, size_t columns);

// The first, and the last, `columns` display columns, with … when cut. Plain
// text only: a cut can drop a style's escape, so style the result.
std::string DisplayTrunc(std::string s, size_t columns);
std::string DisplayTail(std::string text, size_t columns);
std::string StatusLabel(const std::string& label, size_t columns);

// Rows each bounded by `columns` display columns, never splitting a UTF-8
// codepoint; escape sequences take no columns.
std::vector<std::string> WrapLines(const std::string& s, size_t columns);

// Tokenizers vary by model; the pre-request estimate assumes 4 bytes a token.
inline constexpr size_t kEstimatedBytesPerToken = 4;
int64_t EstimatedTokens(size_t bytes);

std::string StripTrailingSlashes(std::string s);

std::string FirstLine(const std::string& s);

// Bounded first line for protocol/data previews, not terminal rendering.
std::string OneLine(const std::string& s, size_t cap = 80);

// Model, tool and MCP text is untrusted terminal input: control bytes are
// rendered visibly rather than executed. Piped output remains exact.
std::string TerminalSafe(std::string_view s);

// The agent's own row glyphs in ASCII; unchanged while `g_unicode` holds.
std::string AsciiGlyphs(std::string_view s);

std::string FmtCount(int64_t n);

// A count and its noun, plural the regular English way: "2 entries".
template <std::integral Count>
std::string Plural(Count count, std::string_view noun) {
  std::string text = std::to_string(count) + " " + std::string(noun);
  if (count == 1) return text;
  if (noun.size() > 1 && noun.back() == 'y' &&
      std::string_view("aeiou").find(noun[noun.size() - 2]) ==
          std::string_view::npos) {
    text.pop_back();
    return text + "ies";
  }
  const bool sibilant = noun.ends_with('s') || noun.ends_with('x') ||
                        noun.ends_with("ch") || noun.ends_with("sh");
  return text + (sibilant ? "es" : "s");
}
std::string FmtCost(double cost);

// "840ms", "2.4s", "41m 1s", "1h 12m", "2d 3h".
std::string FmtDuration(double seconds);

// Decimal byte count: "512 B", "1.4 kB", "2.4 MB".
std::string FmtBytes(int64_t bytes);

// coarse "how long ago", for the session picker
std::string FmtAgo(int64_t seconds);
int64_t TerminalColumns();

// Usable columns after `reserved`; never zero.
size_t TerminalWidth(int64_t reserved = 0);

// Stable short digest for session and folder identity.
std::string HashHex(const std::string& data);
std::string HexEncode(std::string_view data);
bool HexDecode(std::string_view data, std::string& bytes, size_t limit);

// The one list of key stems that name a secret: every redaction policy must
// agree on it, or a stem added for one leaks through the others.
const std::vector<std::string>& CredentialKeyStems();

// True when a key name contains any stem, case-insensitively.
bool CredentialLikeKey(const std::string& key);

std::string UrlHost(std::string url);
std::string RedactedUrl(std::string url);

std::string RouteKey(const std::string& base_url, const std::string& provider,
                     const std::string& model, const std::string& effort);

// HashHex truncated to `chars`.
std::string TruncatedHash(std::string_view data, size_t chars);

// Scope prefix of a `scope/name` key ("project" in "project/foo").
// No '/' -> whole input; empty input -> empty.
std::string_view ScopePrefix(std::string_view key) noexcept;

// The lines two sequences share at each end, and so the span that changed.
// Not a minimal (LCS) diff: enough for the small targeted edits shown here.
struct CommonLineSpan {
  size_t prefix = 0;
  size_t old_end = 0;
  size_t new_end = 0;
};

// `text` as diff lines without their line ends; the views borrow `text`.
std::vector<std::string_view> DiffLines(std::string_view text);

template <typename Lines>
inline CommonLineSpan TrimCommonLines(const Lines& old_lines,
                                      const Lines& new_lines) {
  size_t prefix = 0;
  while (prefix < old_lines.size() && prefix < new_lines.size() &&
         old_lines[prefix] == new_lines[prefix]) {
    ++prefix;
  }
  size_t suffix = 0;
  while (suffix < old_lines.size() - prefix &&
         suffix < new_lines.size() - prefix &&
         old_lines[old_lines.size() - 1 - suffix] ==
             new_lines[new_lines.size() - 1 - suffix]) {
    ++suffix;
  }
  return {prefix, old_lines.size() - suffix, new_lines.size() - suffix};
}

// The span as diff lines, one line of context either side: `emit(marker,
// line)` gets ' ' for context, '-' for removed and '+' for added lines.
template <typename Lines, typename Emit>
inline void ForEachDiffLine(const Lines& old_lines, const Lines& new_lines,
                            const CommonLineSpan& span, Emit&& emit) {
  if (span.prefix > 0) emit(' ', old_lines[span.prefix - 1]);
  for (size_t i = span.prefix; i < span.old_end; ++i) emit('-', old_lines[i]);
  for (size_t i = span.prefix; i < span.new_end; ++i) emit('+', new_lines[i]);
  if (span.old_end < old_lines.size()) emit(' ', old_lines[span.old_end]);
}

// A unified diff of two texts, for display only.
std::string ConfigUnifiedDiff(const std::string& before,
                              const std::string& after,
                              const std::string& label);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STRINGS_H_
