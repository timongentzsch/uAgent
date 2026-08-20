// Copyright 2026 Timon Gentzsch

#include "include/tools/files.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/path_policy.h"

namespace uagent {

namespace {

constexpr size_t kMaxDiffDisplayLines = 80;
constexpr size_t kMaxDiffDisplayLineBytes = 1000;

std::vector<std::string> DiffLines(const std::string& text) {
  std::vector<std::string> lines;
  for (size_t begin = 0; begin < text.size();) {
    size_t end = text.find('\n', begin);
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return lines;
}

struct EditDisplay {
  std::string body;
  int64_t added = 0;
  int64_t removed = 0;
  size_t lines = 0;
  bool truncated = false;
};

void AppendDisplayLine(EditDisplay& display, char marker,
                       const std::string& text) {
  if (display.lines >= kMaxDiffDisplayLines) {
    display.truncated = true;
    return;
  }
  std::string shown = Utf8Prefix(text, kMaxDiffDisplayLineBytes);
  if (shown.size() < text.size()) shown += "…";
  display.body += marker;
  display.body += shown;
  display.body += '\n';
  ++display.lines;
}

void AppendEditDisplay(EditDisplay& display, const std::string& data,
                       size_t match, const std::string& old_text,
                       const std::string& new_text, int64_t applied) {
  size_t line_start = match == 0 ? 0 : data.rfind('\n', match - 1);
  line_start = line_start == std::string::npos ? 0 : line_start + 1;
  size_t block_start = 0;  // one line of leading context, when there is one
  if (line_start > 1) {
    size_t previous = data.rfind('\n', line_start - 2);
    block_start = previous == std::string::npos ? 0 : previous + 1;
  }
  size_t affected_end = data.find('\n', match + old_text.size());
  size_t block_end = affected_end == std::string::npos
                         ? data.size()
                         : data.find('\n', affected_end + 1);
  if (block_end == std::string::npos) block_end = data.size();

  std::string before = data.substr(block_start, block_end - block_start);
  std::string after = before;
  after.replace(match - block_start, old_text.size(), new_text);
  std::vector<std::string> old_lines = DiffLines(before);
  std::vector<std::string> new_lines = DiffLines(after);
  size_t prefix = 0;
  while (prefix < old_lines.size() && prefix < new_lines.size() &&
         old_lines[prefix] == new_lines[prefix]) {
    ++prefix;
  }
  size_t suffix = 0;
  while (suffix < old_lines.size() - prefix &&
         suffix < new_lines.size() - prefix &&
         old_lines[old_lines.size() - suffix - 1] ==
             new_lines[new_lines.size() - suffix - 1]) {
    ++suffix;
  }
  size_t old_end = old_lines.size() - suffix;
  size_t new_end = new_lines.size() - suffix;
  display.removed += static_cast<int64_t>(old_end - prefix) * applied;
  display.added += static_cast<int64_t>(new_end - prefix) * applied;

  int64_t line = 1 + static_cast<int64_t>(
                         std::count(data.begin(), data.begin() + match, '\n'));
  std::string location = "line " + std::to_string(line);
  if (applied > 1) location += " · " + std::to_string(applied) + " matches";
  AppendDisplayLine(display, '@', location);
  if (prefix > 0) AppendDisplayLine(display, ' ', old_lines[prefix - 1]);
  for (size_t i = prefix; i < old_end; ++i) {
    AppendDisplayLine(display, '-', old_lines[i]);
  }
  for (size_t i = prefix; i < new_end; ++i) {
    AppendDisplayLine(display, '+', new_lines[i]);
  }
  if (suffix > 0) AppendDisplayLine(display, ' ', old_lines[old_end]);
}

ToolResult FileOpenFailure(const std::string& path) {
  std::error_code error(errno, std::generic_category());
  return ToolFailure(FileToolError(error), "error: cannot open " + path);
}

// Atomic write: temp file in the same directory, then rename — a disk-full or
// crash mid-write can never leave the target truncated. Keeps an existing
// file's permissions.
ToolResult ToolWriteFileMode(const std::string& path,
                             const std::string& content, mode_t create_mode) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kWritableFile)) {
    return std::move(*invalid);
  }
  return ToolAtomicWrite(path, content, create_mode, /*preserve_mode=*/true);
}

}  // namespace

ToolErrorCode FileToolError(const std::error_code& error) {
  if (error == std::errc::no_such_file_or_directory) {
    return ToolErrorCode::kNotFound;
  }
  if (error == std::errc::permission_denied ||
      error == std::errc::operation_not_permitted ||
      error == std::errc::read_only_file_system) {
    return ToolErrorCode::kPermissionDenied;
  }
  return ToolErrorCode::kInternal;
}

ToolResult ToolAtomicWrite(const std::string& path, const std::string& content,
                           mode_t create_mode, bool preserve_mode) {
  std::string error;
  if (!AtomicWriteFile(path, content, create_mode, preserve_mode, error)) {
    return ToolFailure(ToolErrorCode::kInternal, "error: " + error);
  }
  return ToolSuccess("wrote " + std::to_string(content.size()) + " bytes to " +
                     path);
}

ToolResult ToolReadFile(const std::string& path, int64_t offset,
                        int64_t limit) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kReadableFile)) {
    return std::move(*invalid);
  }
  if (limit == 0) limit = ReadFileLines();  // 0 = unset
  int64_t max_lines = ReadFileMaxLines();
  if (limit <= 0 || limit > max_lines) limit = max_lines;
  int64_t max_bytes = ReadFileBytes();
  if (offset < 1) offset = 1;
  errno = 0;
  std::ifstream f(path);
  if (!f) return FileOpenFailure(path);
  std::string line, out;
  int64_t total = 0, shown = 0, first = 0, last = 0;
  bool output_limited = false, line_truncated = false;
  while (shown < limit && std::getline(f, line)) {
    if (AbortRequested()) return ToolCancelled("error: read cancelled");
    ++total;
    if (total >= offset) {
      std::optional<size_t> with_line = CheckedAdd(out.size(), line.size());
      std::optional<size_t> with_newline =
          with_line ? CheckedAdd(*with_line, 1) : std::nullopt;
      if (!with_newline || *with_newline > static_cast<size_t>(max_bytes)) {
        if (out.empty()) {
          out = Utf8Prefix(std::move(line), static_cast<size_t>(max_bytes));
          first = last = total;
          shown = 1;
          line_truncated = true;
        }
        output_limited = true;
        break;
      }
      out += line;
      out += '\n';
      if (!first) first = total;
      last = total;
      ++shown;
    }
  }
  bool more = output_limited ||
              (shown >= limit && f.peek() != std::char_traits<char>::eof());
  if (total == 0) return ToolSuccess("(empty file)");
  if (offset > total && !more) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: offset " + std::to_string(offset) +
                           " is beyond EOF (" + std::to_string(total) +
                           " lines)");
  }
  std::string header = "[" + path + " lines " + std::to_string(first) + "-" +
                       std::to_string(last);
  if (output_limited) {
    header += line_truncated ? "; line prefix limited; more available"
                             : "; output byte limit reached; more available";
  } else if (more) {
    header += "; more available";
  } else {
    header += " of " + std::to_string(total);
  }
  return ToolSuccess(header + "]\n" + out);
}

ToolResult ToolWriteFile(const std::string& path, const std::string& content) {
  return ToolWriteFileMode(path, content, 0644);
}

ToolResult ToolWritePrivateFile(const std::string& path,
                                const std::string& content) {
  return ToolWriteFileMode(path, content, 0600);
}

// strip read_file-style "   123\t" prefixes, but only if every non-empty line
// has one
std::string StripLineNumbers(const std::string& s) {
  std::istringstream in(s);
  std::string line, out;
  bool any = false, first = true;
  while (std::getline(in, line)) {
    std::string body = line;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t d = i;
    while (d < line.size() && isdigit(static_cast<unsigned char>(line[d]))) d++;
    if (d > i && d < line.size() && line[d] == '\t') {
      body = line.substr(d + 1);
      any = true;
    } else if (!Trim(line).empty()) {
      return s;  // a non-empty line without a prefix: don't strip anything
    }
    if (!first) out += '\n';
    out += body;
    first = false;
  }
  if (!any) return s;
  if (!s.empty() && s.back() == '\n') out += '\n';
  return out;
}

namespace {

int64_t CountOccurrences(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return 0;
  int64_t n = 0;
  for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos;
       pos += needle.size()) {
    ++n;
  }
  return n;
}

bool MostlyCrLf(const std::string& text) {
  size_t newlines =
      static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
  if (!newlines) return false;
  size_t crlf = 0;
  for (size_t pos = 0; (pos = text.find("\r\n", pos)) != std::string::npos;
       pos += 2) {
    ++crlf;
  }
  return crlf * 2 >= newlines;
}

bool CrLfAtMatch(const std::string& data, size_t match, size_t length,
                 bool fallback) {
  size_t end = std::min(data.size(), match + length);
  size_t newline = data.find('\n', match);
  if (newline < end) return newline > 0 && data[newline - 1] == '\r';
  newline = data.find('\n', end);
  if (newline != std::string::npos) {
    return newline > 0 && data[newline - 1] == '\r';
  }
  if (match > 0) {
    newline = data.rfind('\n', match - 1);
    if (newline != std::string::npos) {
      return newline > 0 && data[newline - 1] == '\r';
    }
  }
  return fallback;
}

std::string FileLineEnding(const std::string& text, bool crlf,
                           bool normalize_crlf) {
  std::string out;
  out.reserve(text.size() +
              (crlf ? std::count(text.begin(), text.end(), '\n') : 0));
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n' &&
        normalize_crlf) {
      if (crlf) out += '\r';
      out += '\n';
      ++i;
    } else {
      if (text[i] == '\n' && crlf && (i == 0 || text[i - 1] != '\r')) {
        out += '\r';
      }
      out += text[i];
    }
  }
  return out;
}

std::string EditRecoveryHint(const std::string& data,
                             const std::string& old_text) {
  std::istringstream input(old_text);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (Trim(line).size() < 4) continue;
    size_t match = data.find(line);
    if (match == std::string::npos) continue;
    size_t begin = match == 0 ? 0 : data.rfind('\n', match - 1) + 1;
    size_t end = data.find('\n', match);
    if (end == std::string::npos) end = data.size();
    int64_t number = 1 + std::count(data.begin(), data.begin() + begin, '\n');
    return "; nearby current line " + std::to_string(number) + ": " +
           Utf8Prefix(data.substr(begin, end - begin), 200);
  }
  return "; reread the current file before retrying";
}

bool EditedSize(size_t current, size_t old_size, size_t new_size, int64_t count,
                int64_t max_bytes, size_t& next) {
  if (new_size >= old_size) {
    size_t growth = new_size - old_size;
    if (growth && static_cast<uint64_t>(count) >
                      (std::numeric_limits<size_t>::max() - current) / growth) {
      return false;
    }
    next = current + growth * static_cast<size_t>(count);
  } else {
    next = current - (old_size - new_size) * static_cast<size_t>(count);
  }
  return max_bytes <= 0 || next <= static_cast<size_t>(max_bytes);
}

void ReplaceAllOccurrences(std::string& data, const std::string& old_s,
                           const std::string& new_s, size_t next_size) {
  std::string out;
  out.reserve(next_size);
  size_t copied = 0;
  while (true) {
    size_t match = data.find(old_s, copied);
    if (match == std::string::npos) break;
    out.append(data, copied, match - copied);
    out += new_s;
    copied = match + old_s.size();
  }
  out.append(data, copied, std::string::npos);
  data.swap(out);
}

}  // namespace

ToolResult ToolEditFile(const std::string& path,
                        const std::vector<FileEdit>& edits) {
  if (edits.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: at least one edit is required");
  }
  if (auto invalid = ValidatePathTarget(path, PathTarget::kReadableFile)) {
    return std::move(*invalid);
  }
  errno = 0;
  std::ifstream f(path, std::ios::binary);
  if (!f) return FileOpenFailure(path);
  std::error_code size_ec;
  auto bytes = std::filesystem::file_size(path, size_ec);
  int64_t max_bytes = EditFileBytes();
  if (!size_ec && max_bytes > 0 && bytes > static_cast<uintmax_t>(max_bytes)) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: " + path + " is too large to edit atomically (" +
                           std::to_string(bytes) + " bytes; limit " +
                           std::to_string(max_bytes) + ")");
  }
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  f.close();
  if (max_bytes > 0 && data.size() > static_cast<size_t>(max_bytes)) {
    return ToolFailure(
        ToolErrorCode::kLimitExceeded,
        "error: " + path + " grew beyond the edit limit while reading");
  }

  const size_t original_size = data.size();
  int64_t replacements = 0;
  int64_t already_applied = 0;
  EditDisplay display;
  for (size_t i = 0; i < edits.size(); ++i) {
    const FileEdit& edit = edits[i];
    if (edit.old_text.empty()) {
      return ToolFailure(
          ToolErrorCode::kInvalidArguments,
          "error: edit " + std::to_string(i + 1) + " has an empty `old` value");
    }
    const bool file_crlf = MostlyCrLf(data);
    std::string old_eff = edit.old_text;
    int64_t count = CountOccurrences(data, old_eff);
    bool normalized_old = false;
    if (count == 0) {  // accept copied output from line-numbering readers
      std::string stripped = StripLineNumbers(edit.old_text);
      if (stripped != edit.old_text) {
        old_eff = stripped;
        count = CountOccurrences(data, old_eff);
      }
    }
    if (count == 0) {  // tolerate normalized model text without changing style
      std::string normalized =
          FileLineEnding(old_eff, file_crlf, /*normalize_crlf=*/true);
      if (normalized != old_eff) {
        old_eff = normalized;
        count = CountOccurrences(data, old_eff);
        normalized_old = count > 0;
      }
    }
    if (count == 0) {
      std::string normalized_new =
          FileLineEnding(edit.new_text, file_crlf, /*normalize_crlf=*/true);
      if (!edit.new_text.empty() &&
          (data.find(edit.new_text) != std::string::npos ||
           data.find(normalized_new) != std::string::npos)) {
        ++already_applied;
        continue;
      }
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: edit " + std::to_string(i + 1) +
                             " `old` not found in " + path +
                             EditRecoveryHint(data, old_eff));
    }
    if (!edit.replace_all && count > 1) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: edit " + std::to_string(i + 1) +
                             " `old` matches " + std::to_string(count) +
                             " times in " + path +
                             "; add surrounding context or set `replace_all`");
    }
    size_t match = data.find(old_eff);
    bool replacement_crlf =
        count == 1 ? CrLfAtMatch(data, match, old_eff.size(), file_crlf)
                   : file_crlf;
    std::string new_eff =
        FileLineEnding(edit.new_text, replacement_crlf, normalized_old);
    if (old_eff == new_eff) {
      ++already_applied;
      continue;
    }
    int64_t applied = edit.replace_all ? count : 1;
    size_t next_size = 0;
    if (!EditedSize(data.size(), old_eff.size(), new_eff.size(), applied,
                    max_bytes, next_size)) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: edit " + std::to_string(i + 1) +
                             " would exceed the edit byte limit");
    }
    AppendEditDisplay(display, data, match, old_eff, new_eff, applied);
    if (edit.replace_all) {
      ReplaceAllOccurrences(data, old_eff, new_eff, next_size);
    } else {
      data.replace(match, old_eff.size(), new_eff);
    }
    replacements += applied;
  }
  if (replacements == 0) {
    return ToolSuccess("already applied " + path + " (" +
                       std::to_string(already_applied) +
                       (already_applied == 1 ? " edit)" : " edits)"));
  }
  ToolResult write =
      ToolWriteFile(path, data);  // atomic replace, keeps permissions
  if (!write.Ok()) return write;
  ToolResult result = ToolSuccess(
      "edited " + path + " (" + std::to_string(replacements) +
      (replacements == 1 ? " replacement across " : " replacements across ") +
      std::to_string(edits.size()) +
      (edits.size() == 1 ? " edit; " : " edits; ") +
      std::to_string(original_size) + " -> " + std::to_string(data.size()) +
      " bytes)");
  result.display = "Edited " + DisplayPath(path) + " (+" +
                   std::to_string(display.added) + " -" +
                   std::to_string(display.removed) + ")\n" + display.body;
  if (display.truncated) result.display += " … diff truncated\n";
  return result;
}

ToolResult ToolEditFile(const std::string& path, const std::string& old_s,
                        const std::string& new_s, bool replace_all) {
  return ToolEditFile(path, {{old_s, new_s, replace_all}});
}

namespace {

bool LikelyTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  char sample[4096];
  input.read(sample, sizeof sample);
  std::streamsize size = input.gcount();
  if (!input && !input.eof()) return false;
  for (std::streamsize i = 0; i < size; ++i) {
    unsigned char value = static_cast<unsigned char>(sample[i]);
    if (value == 0 || value < 0x09 || (value > 0x0d && value < 0x20)) {
      return false;
    }
  }
  return true;
}

// The whole contents of a tiny directory, when every entry is a small text
// file. Any reason not to inline them leaves the plain listing in place.
std::optional<std::string> SmallDirectoryPreview(
    const std::string& dir, const std::vector<std::string>& entries,
    const std::string& listing) {
  namespace fs = std::filesystem;
  uintmax_t total_bytes = 0;
  uintmax_t max_bytes = static_cast<uintmax_t>(ReadFileBytes());
  for (const std::string& entry : entries) {
    fs::path entry_path = fs::path(dir) / entry;
    std::error_code type_error;
    if (!fs::is_regular_file(fs::symlink_status(entry_path, type_error)) ||
        type_error) {
      return std::nullopt;
    }
    std::error_code size_error;
    uintmax_t bytes = fs::file_size(entry_path, size_error);
    if (size_error || bytes > max_bytes - total_bytes) return std::nullopt;
    total_bytes += bytes;
    if (!LikelyTextFile(entry_path)) return std::nullopt;
  }

  std::string preview = listing + "\n[small directory contents]\n";
  for (const std::string& entry : entries) {
    ToolResult read = ToolReadFile((fs::path(dir) / entry).string(), 1, -1);
    if (!read.Ok()) return std::nullopt;
    preview += "\n";
    preview += read.output;
    if (static_cast<int64_t>(preview.size()) > ReadFileResultChars()) {
      return std::nullopt;
    }
  }
  return preview;
}

}  // namespace

ToolResult ToolListDir(const std::string& path, int64_t offset, int64_t limit,
                       bool include_small_files) {
  std::string p = path.empty() ? "." : path;
  if (auto invalid = ValidatePathTarget(p, PathTarget::kDirectory)) {
    return std::move(*invalid);
  }
  if (offset < 0) offset = 0;
  if (limit <= 0) limit = ListDirEntries();
  int64_t scan_cap = ListDirScanEntries();
  std::error_code ec;
  std::vector<std::string> entries;
  for (auto& e : std::filesystem::directory_iterator(p, ec)) {
    if (static_cast<int64_t>(entries.size()) >= scan_cap) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: directory exceeds scan limit (" +
                             std::to_string(scan_cap) + " entries)");
    }
    std::error_code type_error;
    bool directory = e.is_directory(type_error);
    entries.push_back(e.path().filename().string() + (directory ? "/" : ""));
  }
  if (ec) {
    return ToolFailure(FileToolError(ec), "error: cannot open directory " + p);
  }
  std::sort(entries.begin(), entries.end());
  if (entries.empty()) return ToolSuccess("(empty directory)");
  if (offset >= static_cast<int64_t>(entries.size())) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: offset is beyond directory entries (" +
                           std::to_string(entries.size()) + ")");
  }
  size_t begin = static_cast<size_t>(offset);
  size_t available = entries.size() - begin;
  size_t count = limit > static_cast<int64_t>(available)
                     ? available
                     : static_cast<size_t>(limit);
  size_t end = begin + count;
  std::string out = "[" + p + " entries " + std::to_string(offset + 1) + "-" +
                    std::to_string(end) + " of " +
                    std::to_string(entries.size()) + "]\n";
  for (size_t i = begin; i < end; ++i) {
    out += entries[i];
    out += '\n';
  }

  constexpr size_t kPreviewFiles = 4;
  if (!include_small_files || offset != 0 || end != entries.size() ||
      entries.size() > kPreviewFiles) {
    return ToolSuccess(std::move(out));
  }
  if (std::optional<std::string> preview =
          SmallDirectoryPreview(p, entries, out)) {
    return ToolSuccess(std::move(*preview), ReadFileResultChars());
  }
  return ToolSuccess(std::move(out));
}

}  // namespace uagent
