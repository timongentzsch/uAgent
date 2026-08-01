// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STRINGS_H_
#define UAGENT_INCLUDE_CORE_STRINGS_H_
// String, UTF-8, and display-width helpers, plus the terminal-safe and
// human-readable formatters shared by the REPL and the tool trace.

#include <cstdint>
#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

std::string Trim(const std::string& s);

void ReplaceAll(std::string& s, const std::string& from, const std::string& to);

std::string AsciiLower(std::string text);

bool ContainsCaseInsensitive(std::string text, const std::string& query);

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

// cap a string at a UTF-8 boundary (never splits a codepoint), appending "…"
std::string Utf8Prefix(std::string s, size_t cap);

std::string Utf8Trunc(std::string s, size_t cap);

// Terminal column width for valid UTF-8 in the active locale. Invalid or
// incomplete sequences degrade to one column rather than breaking rendering.
size_t DisplayWidth(const std::string& s);

std::string DisplayTrunc(std::string s, size_t columns);

size_t JsonEstimatedBytes(const json& value);

std::string StripTrailingSlashes(std::string s);

std::string FirstLine(const std::string& s);

// Bounded first line for protocol/data previews, not terminal rendering.
std::string OneLine(const std::string& s, size_t cap = 80);

// Model, tool and MCP text is untrusted terminal input. Preserve normal text,
// tabs and newlines but render control bytes visibly instead of letting them
// execute terminal commands. Piped output is not a terminal and remains exact.
std::string TerminalSafe(const std::string& s);

std::string FmtCount(int64_t n);
std::string FmtCost(double cost);

// coarse "how long ago", for the session picker
std::string FmtAgo(int64_t seconds);
int64_t TerminalColumns();

std::string TerminalSummary(const std::string& text,
                            size_t reserved_columns = 0);

std::string SpinnerLabel(std::string label);

uint64_t Fnv1aUpdate(uint64_t hash, const char* data, size_t size);

std::string Hex64(uint64_t value);

std::string UrlHost(std::string url);

std::string UrlAuthority(std::string url);

std::string RouteKey(const std::string& base_url, const std::string& provider,
                     const std::string& model, const std::string& effort);

bool OpenrouterUrl(std::string url);

bool LoopbackUrl(std::string url);

bool OpenrouterCompatibleUrl(std::string url);

bool OpenaiUrl(std::string url);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STRINGS_H_
