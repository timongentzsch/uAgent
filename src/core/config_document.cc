// Copyright 2026 Timon Gentzsch

#include "include/core/config_document.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/core/strings.h"

namespace uagent {
namespace {

// The loader trims, drops an optional `export `, then splits on the first `=`.
// This mirrors it exactly so a line this file calls an assignment is the same
// line the loader would.
bool AssignmentKey(const std::string& line, std::string& key) {
  std::string text = Trim(line);
  if (text.empty() || text[0] == '#') return false;
  if (text.starts_with("export ")) text = Trim(text.substr(7));
  size_t equals = text.find('=');
  if (equals == std::string::npos || equals == 0) return false;
  key = Trim(text.substr(0, equals));
  return !key.empty();
}

bool BareValue(const std::string& value) {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return isalnum(character) ||
           std::string_view("_./:@%+,-").find(static_cast<char>(character)) !=
               std::string_view::npos;
  });
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      if (start < text.size()) lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

}  // namespace

bool ConfigValueLiteral(const std::string& value, std::string& literal,
                        std::string& error) {
  if (BareValue(value)) {
    literal = value;
    return true;
  }
  // Single quotes are literal for the loader, which strips exactly one layer.
  if (value.find('\'') == std::string::npos) {
    literal = "'" + value + "'";
    return true;
  }
  // `$` would be interpolated inside double quotes at resolve time.
  if (value.find('"') == std::string::npos &&
      value.find('$') == std::string::npos) {
    literal = "\"" + value + "\"";
    return true;
  }
  error =
      "value mixes quotes and interpolation characters this config grammar "
      "cannot represent; set it by hand";
  return false;
}

ConfigDocument ConfigDocument::Parse(const std::string& bytes) {
  ConfigDocument document;
  document.final_newline_ = bytes.empty() || bytes.back() == '\n';
  document.lines_ = SplitLines(bytes);
  // CRLF is preserved by keeping the carriage return on each line and only
  // deciding the terminator that new lines are written with.
  size_t carriage = 0;
  for (std::string& line : document.lines_) {
    if (!line.empty() && line.back() == '\r') ++carriage;
  }
  if (carriage > 0 && carriage == document.lines_.size()) {
    document.terminator_ = "\r\n";
    for (std::string& line : document.lines_) line.pop_back();
  }
  return document;
}

size_t ConfigDocument::Count(std::string_view key) const {
  size_t count = 0;
  for (const std::string& line : lines_) {
    std::string found;
    if (AssignmentKey(line, found) && found == key) ++count;
  }
  return count;
}

bool ConfigDocument::Set(const std::string& key, const std::string& value,
                         std::string& error) {
  size_t assignments = Count(key);
  if (assignments > 1) {
    error = key + " is assigned " + std::to_string(assignments) +
            " times in this file; resolve the duplicates by hand first";
    return false;
  }
  std::string literal;
  if (!ConfigValueLiteral(value, literal, error)) return false;
  for (std::string& line : lines_) {
    std::string found;
    if (!AssignmentKey(line, found) || found != key) continue;
    // Keep an `export ` prefix and the original indentation.
    std::string text = Trim(line);
    std::string prefix = text.starts_with("export ") ? "export " : "";
    size_t indent = line.find_first_not_of(" \t");
    line = line.substr(0, indent == std::string::npos ? 0 : indent) + prefix +
           key + "=" + literal;
    return true;
  }
  lines_.push_back(key + "=" + literal);
  return true;
}

bool ConfigDocument::Unset(const std::string& key, std::string& error) {
  size_t assignments = Count(key);
  if (assignments > 1) {
    error = key + " is assigned " + std::to_string(assignments) +
            " times in this file; resolve the duplicates by hand first";
    return false;
  }
  if (assignments == 0) return true;  // already absent
  std::erase_if(lines_, [&](const std::string& line) {
    std::string found;
    return AssignmentKey(line, found) && found == key;
  });
  return true;
}

std::string ConfigDocument::Render() const {
  std::string out;
  for (size_t index = 0; index < lines_.size(); ++index) {
    out += lines_[index];
    if (index + 1 < lines_.size() || final_newline_) out += terminator_;
  }
  return out;
}

std::string ConfigUnifiedDiff(const std::string& before,
                              const std::string& after,
                              const std::string& label) {
  std::vector<std::string> old_lines = SplitLines(before);
  std::vector<std::string> new_lines = SplitLines(after);
  const CommonLineSpan span = TrimCommonLines(old_lines, new_lines);
  const size_t prefix = span.prefix;
  std::string diff = "--- " + label + "\n+++ " + label + "\n";
  size_t context = prefix > 0 ? 1 : 0;
  for (size_t index = prefix - context; index < prefix; ++index) {
    diff += "  " + old_lines[index] + "\n";
  }
  for (size_t index = prefix; index < span.old_end; ++index) {
    diff += "- " + old_lines[index] + "\n";
  }
  for (size_t index = prefix; index < span.new_end; ++index) {
    diff += "+ " + new_lines[index] + "\n";
  }
  if (span.new_end < new_lines.size()) {
    diff += "  " + new_lines[span.new_end] + "\n";
  }
  return diff;
}

}  // namespace uagent
