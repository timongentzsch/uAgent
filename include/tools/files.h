// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_FILES_H_
#define UAGENT_INCLUDE_TOOLS_FILES_H_
// File inspection and editing tools, plus the durable memories the agent
// writes for itself. Every write goes through the atomic replace in
// core/fs.h, so a crash cannot leave a truncated file behind.

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
#include "include/core/strings.h"
#include "include/tools/path_policy.h"
#include "include/tools/tool.h"

namespace uagent {

inline ToolErrorCode FileToolError(const std::error_code& error) {
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

inline ToolResult ToolReadFile(const std::string& path, int64_t offset,
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
  if (!f) {
    std::error_code error(errno, std::generic_category());
    return ToolFailure(
        path.empty() ? ToolErrorCode::kInvalidArguments : FileToolError(error),
        "error: cannot open " + path);
  }
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
  if (more && ReadFileCountsTotal()) {
    // Optional compatibility mode: exact totals require scanning the tail.
    char blk[1 << 16];
    bool ended_nl = true;
    while (f.read(blk, sizeof blk), f.gcount() > 0) {
      if (AbortRequested()) return ToolCancelled("error: read cancelled");
      total += static_cast<int64_t>(std::count(blk, blk + f.gcount(), '\n'));
      ended_nl = blk[f.gcount() - 1] == '\n';
    }
    if (!ended_nl) ++total;  // final line without trailing newline
  }
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
  } else if (more && !ReadFileCountsTotal()) {
    header += "; more available";
  } else {
    header += " of " + std::to_string(total);
  }
  return ToolSuccess(header + "]\n" + out);
}

// Atomic write: temp file in the same directory, then rename — a disk-full or
// crash mid-write can never leave the target truncated. Keeps an existing
// file's permissions.
inline ToolResult ToolWriteFileMode(const std::string& path,
                                    const std::string& content,
                                    mode_t create_mode) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kWritableFile)) {
    return std::move(*invalid);
  }
  std::string error;
  if (!AtomicWriteFile(path, content, create_mode, /*preserve_mode=*/true,
                       error)) {
    return ToolFailure(path.empty() ? ToolErrorCode::kInvalidArguments
                                    : ToolErrorCode::kInternal,
                       "error: " + error);
  }
  return ToolSuccess("wrote " + std::to_string(content.size()) + " bytes to " +
                     path);
}

inline ToolResult ToolWriteFile(const std::string& path,
                                const std::string& content) {
  return ToolWriteFileMode(path, content, 0644);
}

inline ToolResult ToolWritePrivateFile(const std::string& path,
                                       const std::string& content) {
  return ToolWriteFileMode(path, content, 0600);
}

// strip read_file-style "   123\t" prefixes, but only if every non-empty line
// has one
inline std::string StripLineNumbers(const std::string& s) {
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

inline int64_t CountOccurrences(const std::string& hay,
                                const std::string& needle) {
  if (needle.empty()) return 0;
  int64_t n = 0;
  for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos;
       pos += needle.size()) {
    ++n;
  }
  return n;
}

struct FileEdit {
  std::string old_text;
  std::string new_text;
  bool replace_all = false;
};

inline constexpr size_t kMaxFileEdits = 64;

inline bool MostlyCrLf(const std::string& text) {
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

inline bool CrLfAtMatch(const std::string& data, size_t match, size_t length,
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

inline std::string FileLineEnding(const std::string& text, bool crlf,
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

inline bool EditedSize(size_t current, size_t old_size, size_t new_size,
                       int64_t count, int64_t max_bytes, size_t& next) {
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

inline void ReplaceAllOccurrences(std::string& data, const std::string& old_s,
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

inline ToolResult ToolEditFile(const std::string& path,
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
  if (!f) {
    std::error_code error(errno, std::generic_category());
    return ToolFailure(
        path.empty() ? ToolErrorCode::kInvalidArguments : FileToolError(error),
        "error: cannot open " + path);
  }
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
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: edit " + std::to_string(i + 1) +
                             " `old` not found in " + path);
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
      return ToolFailure(
          ToolErrorCode::kInvalidArguments,
          "error: edit " + std::to_string(i + 1) + " makes no change");
    }
    int64_t applied = edit.replace_all ? count : 1;
    size_t next_size = 0;
    if (!EditedSize(data.size(), old_eff.size(), new_eff.size(), applied,
                    max_bytes, next_size)) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: edit " + std::to_string(i + 1) +
                             " would exceed the edit byte limit");
    }
    if (edit.replace_all) {
      ReplaceAllOccurrences(data, old_eff, new_eff, next_size);
    } else {
      data.replace(match, old_eff.size(), new_eff);
    }
    replacements += applied;
  }
  ToolResult write =
      ToolWriteFile(path, data);  // atomic replace, keeps permissions
  if (!write.Ok()) return write;
  return ToolSuccess(
      "edited " + path + " (" + std::to_string(replacements) +
      (replacements == 1 ? " replacement across " : " replacements across ") +
      std::to_string(edits.size()) +
      (edits.size() == 1 ? " edit; " : " edits; ") +
      std::to_string(original_size) + " -> " + std::to_string(data.size()) +
      " bytes)");
}

inline ToolResult ToolEditFile(const std::string& path,
                               const std::string& old_s,
                               const std::string& new_s,
                               bool replace_all = false) {
  return ToolEditFile(path, {{old_s, new_s, replace_all}});
}

inline bool LikelyTextFile(const std::filesystem::path& path) {
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

inline ToolResult ToolListDir(const std::string& path, int64_t offset = 0,
                              int64_t limit = 0,
                              bool include_small_files = false) {
  std::string p = path.empty() ? "." : path;
  if (auto invalid = ValidatePathTarget(p, PathTarget::kDirectory)) {
    return std::move(*invalid);
  }
  if (offset < 0) offset = 0;
  if (limit <= 0) limit = ListDirEntries();
  int64_t scan_cap = ListDirScanEntries();
  std::error_code ec;
  struct Entry {
    std::string name;
  };
  std::vector<Entry> entries;
  for (auto& e : std::filesystem::directory_iterator(p, ec)) {
    if (static_cast<int64_t>(entries.size()) >= scan_cap) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: directory exceeds scan limit (" +
                             std::to_string(scan_cap) + " entries)");
    }
    std::error_code type_error;
    bool directory = e.is_directory(type_error);
    entries.push_back({e.path().filename().string() + (directory ? "/" : "")});
  }
  if (ec) {
    return ToolFailure(FileToolError(ec), "error: cannot open directory " + p);
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const Entry& lhs, const Entry& rhs) { return lhs.name < rhs.name; });
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
  for (size_t i = static_cast<size_t>(offset); i < end; ++i) {
    out += entries[i].name;
    out += '\n';
  }

  constexpr size_t kPreviewFiles = 4;
  if (!include_small_files || offset != 0 || end != entries.size() ||
      entries.size() > kPreviewFiles) {
    return ToolSuccess(std::move(out));
  }

  uintmax_t total_bytes = 0;
  uintmax_t max_bytes = static_cast<uintmax_t>(ReadFileBytes());
  for (const Entry& entry : entries) {
    std::filesystem::path entry_path = std::filesystem::path(p) / entry.name;
    std::error_code type_error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::symlink_status(entry_path, type_error)) ||
        type_error) {
      return ToolSuccess(std::move(out));
    }
    std::error_code size_error;
    uintmax_t bytes = std::filesystem::file_size(entry_path, size_error);
    if (size_error || bytes > max_bytes - total_bytes) {
      return ToolSuccess(std::move(out));
    }
    total_bytes += bytes;

    if (!LikelyTextFile(entry_path)) return ToolSuccess(std::move(out));
  }

  std::string preview = out + "\n[small directory contents]\n";
  for (const Entry& entry : entries) {
    ToolResult read =
        ToolReadFile((std::filesystem::path(p) / entry.name).string(), 1, -1);
    if (!read.Ok()) return ToolSuccess(std::move(out));
    preview += "\n";
    preview += read.output;
    if (static_cast<int64_t>(preview.size()) > ReadFileResultChars()) {
      return ToolSuccess(std::move(out));
    }
  }
  return ToolSuccess(std::move(preview), ReadFileResultChars());
}

// Durable notes the agent writes for itself, reloaded with the project
// instructions at the start of every session. Global memories follow the user,
// project memories stay with the workspace that opted into ./.uagent — writing
// one is what creates that directory, and the call needs approval first.
inline ToolResult ToolMemory(const std::string& name, const std::string& scope,
                             const std::string& content) {
  namespace fs = std::filesystem;
  if (Trim(name).empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory name must not be empty");
  }
  if (scope != "project" && scope != "global") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: scope must be \"project\" or \"global\"");
  }
  int64_t max_bytes = MemoryBytes();
  if (static_cast<int64_t>(content.size()) > max_bytes) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: a memory is limited to " +
                           std::to_string(max_bytes) +
                           " bytes; keep it to the durable lesson");
  }
  std::error_code ec;
  std::string base = GlobalBase();
  if (scope == "project") {
    fs::path cwd = fs::current_path(ec);
    if (ec) {
      return ToolFailure(
          FileToolError(ec),
          "error: cannot resolve the workspace: " + ec.message());
    }
    base = ProjectBase(cwd).string();
  }
  std::string dir = MakePrivateDir(base, kMemoryDir);
  std::string file = dir + "/" + SafeFileComponent(name) + ".md";
  if (Trim(content).empty()) {
    if (fs::remove(file, ec)) return ToolSuccess("forgot " + file);
    return ToolFailure(ec ? FileToolError(ec) : ToolErrorCode::kNotFound,
                       "error: no such memory");
  }
  int64_t max_files = MaxMemories();
  if (!fs::exists(file, ec)) {
    int64_t count = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
      count += it->path().extension() == ".md";
    }
    if (count >= max_files) {
      return ToolFailure(
          ToolErrorCode::kLimitExceeded,
          "error: " + scope + " memory is full (" + std::to_string(max_files) +
              "); delete or consolidate one before adding another");
    }
  }
  return ToolWritePrivateFile(file, content);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_FILES_H_
