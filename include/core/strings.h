// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STRINGS_H_
#define UAGENT_INCLUDE_CORE_STRINGS_H_
// String, UTF-8, and display-width helpers, plus the terminal-safe and
// human-readable formatters shared by the REPL and the tool trace.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uagent {

std::string Trim(const std::string& s);

void ReplaceAll(std::string& s, const std::string& from, const std::string& to);

std::string AsciiLower(std::string text);

bool ContainsCaseInsensitive(std::string text, const std::string& query);

bool ParseInt64(const char* text, int64_t& value);

bool ParseFiniteDouble(const char* text, double& value);

// drop one layer of matching surrounding quotes, if present
std::string Unquote(std::string s);

// wrap for /bin/sh: single-quote everything, closing and re-opening around a
// literal quote ('it'\''s'), so no byte of the payload can reach the shell.
std::string ShellQuote(const std::string& s);

// Entries are returned as written, empties included: PATH reads an empty entry
// as the working directory, while a search path skips it.
std::vector<std::string> SplitPathList(const std::string& value,
                                       char separator = ':');

bool ExecutableOnPath(const std::string& name);

size_t Utf8BoundaryBefore(const std::string& s, size_t offset);

size_t Utf8BoundaryAfter(const std::string& s, size_t offset);

// Cap a string at a UTF-8 boundary without splitting a codepoint.
std::string Utf8Prefix(std::string s, size_t cap);

// The same bound with an ellipsis when truncation is required.
std::string Utf8Trunc(std::string s, size_t cap);

// Terminal column width for valid UTF-8 in the active locale. Invalid or
// incomplete sequences degrade to one column rather than breaking rendering.
size_t DisplayWidth(const std::string& s);

std::string DisplayTrunc(std::string s, size_t columns);
std::string DisplayTail(std::string text, size_t columns);
std::string ActivityLabel(const std::string& label, size_t columns);

// Window of up to `columns` display columns starting at display column `start`
// (left edge), never splitting a UTF-8 codepoint. Short text is returned
// left-aligned. Used by the rolling reasoning ticker.
std::string DisplayWindow(const std::string& text, size_t start,
                          size_t columns);

// Wrap ANSI-free display text into rows each bounded by `columns` display
// columns, never splitting a UTF-8 codepoint. Used for line-wrapping terminal
// input/output where DisplayTrunc (ellipsis truncation) is not wanted.
std::vector<std::string> WrapLines(const std::string& s, size_t columns);

int64_t EstimatedTokens(size_t bytes);

std::string StripTrailingSlashes(std::string s);

std::string FirstLine(const std::string& s);

// Bounded first line for protocol/data previews, not terminal rendering.
std::string OneLine(const std::string& s, size_t cap = 80);

// Model, tool and MCP text is untrusted terminal input. Preserve normal text,
// tabs and newlines but render control bytes visibly instead of letting them
// execute terminal commands. Piped output is not a terminal and remains exact.
std::string TerminalSafe(std::string_view s);

std::string FmtCount(int64_t n);
std::string FmtCost(double cost);

// coarse "how long ago", for the session picker
std::string FmtAgo(int64_t seconds);
int64_t TerminalColumns();

// Usable columns after reserving room for a prompt, marker or trailing cell.
// Never zero: callers pass the result straight to a width-bounded formatter.
size_t TerminalWidth(int64_t reserved = 0);

std::string TerminalSummary(const std::string& text,
                            size_t reserved_columns = 0);

std::string SpinnerLabel(const std::string& label);

inline constexpr uint64_t kFnv1aOffsetBasis = 1469598103934665603ULL;
uint64_t Fnv1aUpdate(uint64_t hash, const char* data, size_t size);

std::string Hex64(uint64_t value);

// Stable short digest for session and workspace identity.
std::string HashHex(const std::string& data);

std::string UrlHost(std::string url);
std::string RedactedUrl(std::string url);

std::string RouteKey(const std::string& base_url, const std::string& provider,
                     const std::string& model, const std::string& effort);

bool OpenrouterUrl(std::string url);

bool OpenaiUrl(std::string url);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STRINGS_H_
