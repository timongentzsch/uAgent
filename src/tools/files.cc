// Copyright 2026 Timon Gentzsch

#include "include/tools/files.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"

namespace uagent {

namespace {

struct EditDisplay {
  std::string body;
  int64_t added = 0;
  int64_t removed = 0;
};

// "<Verb> <path> (+added -removed)" over the diff lines.
std::string DiffReceipt(const char* verb, const std::string& path,
                        const EditDisplay& display) {
  return std::string(verb) + " " + DisplayPath(path) + " (+" +
         std::to_string(display.added) + " -" +
         std::to_string(display.removed) + ")\n" + display.body;
}

using LineDiff = CommonLineSpan;

void AppendLineDiff(EditDisplay& display,
                    const std::vector<std::string_view>& old_lines,
                    const std::vector<std::string_view>& new_lines,
                    const LineDiff& diff) {
  ForEachDiffLine(old_lines, new_lines, diff,
                  [&](char marker, std::string_view line) {
                    (display.body += marker).append(line) += '\n';
                  });
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
  std::vector<std::string_view> old_lines = DiffLines(before);
  std::vector<std::string_view> new_lines = DiffLines(after);
  LineDiff diff = TrimCommonLines(old_lines, new_lines);
  display.removed += static_cast<int64_t>(diff.old_end - diff.prefix) * applied;
  display.added += static_cast<int64_t>(diff.new_end - diff.prefix) * applied;

  int64_t line =
      1 + static_cast<int64_t>(std::count(
              data.begin(), data.begin() + static_cast<std::ptrdiff_t>(match),
              '\n'));
  std::string location = "line " + std::to_string(line);
  if (applied > 1) location += " · " + std::to_string(applied) + " matches";
  display.body += "@" + location + "\n";
  AppendLineDiff(display, old_lines, new_lines, diff);
}

ToolResult FileOpenFailure(const std::string& path) {
  std::error_code error(errno, std::generic_category());
  return ToolFailure(FileToolError(error), "error: cannot open " + path);
}

// Defined below, beside the directory preview that shares it.
bool LikelyTextFile(const std::filesystem::path& path);

// Scan skipped lines without retaining them; never allocate a complete giant
// line merely to truncate it. Cancellation is checked once per input chunk.
class FileLines {
 public:
  explicit FileLines(std::istream& input) : input_(input) {}

  bool Next(std::string& line, size_t cap, bool skip, bool& limited) {
    line.clear();
    bool found = false;
    while (!AbortRequested()) {
      if (begin_ == end_) {
        input_.read(buffer_, sizeof buffer_);
        begin_ = 0;
        end_ = static_cast<size_t>(input_.gcount());
        if (end_ == 0) return found;
      }
      const char* start = buffer_ + begin_;
      const char* newline =
          static_cast<const char*>(std::memchr(start, '\n', end_ - begin_));
      size_t bytes =
          newline ? static_cast<size_t>(newline - start) : end_ - begin_;
      found = true;
      if (!skip) {
        size_t keep = std::min(bytes, cap - line.size());
        line.append(start, keep);
        if (keep < bytes) {
          limited = true;
          return true;
        }
      }
      begin_ += bytes + (newline ? 1 : 0);
      if (newline) return true;
    }
    return false;
  }

  bool More() {
    return begin_ < end_ || input_.peek() != std::char_traits<char>::eof();
  }

 private:
  std::istream& input_;
  char buffer_[8192];
  size_t begin_ = 0;
  size_t end_ = 0;
};

}  // namespace

ToolResult ToolReadFile(const std::string& path, int64_t offset, int64_t limit,
                        const std::string& call_id) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kReadableFile)) {
    return std::move(*invalid);
  }
  if (!LikelyTextFile(path)) {
    if (offset != 1 || limit != 0) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "omit offset and limit for images/documents");
    }
    return Attachments().Add(path, call_id);
  }
  if (limit == 0) limit = ReadFileLines();  // 0 = unset
  // A negative limit is the internal "as much as allowed" idiom; only a
  // positive request that the cap reduces is worth reporting back.
  int64_t requested_lines = limit;
  if (limit <= 0 || limit > kReadFileMaxLines) limit = kReadFileMaxLines;
  if (offset < 1) offset = 1;
  errno = 0;
  std::ifstream f(path);
  if (!f) return FileOpenFailure(path);
  std::string line, out;
  int64_t total = 0, shown = 0, first = 0, last = 0;
  bool output_limited = false, line_truncated = false;
  FileLines lines(f);
  while (shown < limit && lines.Next(line, kReadFileBytes - out.size(),
                                     total + 1 < offset, output_limited)) {
    if (AbortRequested()) return ToolCancelled("error: read cancelled by user");
    ++total;
    if (total >= offset) {
      if (output_limited || line.size() >= kReadFileBytes - out.size()) {
        if (out.empty()) {
          out = Utf8Prefix(std::move(line), kReadFileBytes);
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
  if (AbortRequested()) return ToolCancelled("error: read cancelled by user");
  if (f.bad()) {
    return ToolFailure(ToolErrorCode::kInternal, "read failed");
  }
  bool more = output_limited || (shown >= limit && lines.More());
  if (total == 0) return ToolSuccess("(empty file)");
  if (offset > total && !more) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "offset " + std::to_string(offset) + " is beyond EOF (" +
                           std::to_string(total) + " lines)");
  }
  std::string header = "[" + path + " lines " + std::to_string(first) + "-" +
                       std::to_string(last);
  if (output_limited) {
    header += line_truncated ? "; line prefix limited; use a targeted search "
                               "or run to inspect the remainder"
                             : "; output byte limit reached; more available";
  } else if (more) {
    header += "; more available";
  } else {
    header += " of " + std::to_string(total);
  }
  if (requested_lines > kReadFileMaxLines) {
    header += "; limit " + std::to_string(kReadFileMaxLines) + " of " +
              std::to_string(requested_lines) + " requested";
  }
  ToolResult result = ToolSuccess(header + "]\n" + out);
  if (!output_limited) result.read_range = ReadRange{path, first, last};
  return result;
}

ToolResult ToolWriteFile(const std::string& path, const std::string& content) {
  return ToolWriteFileMode(path, content, kSharedFileMode);
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

struct Occurrences {
  int64_t count = 0;
  size_t first = std::string::npos;
};

Occurrences CountOccurrences(const std::string& hay,
                             const std::string& needle) {
  Occurrences found;
  if (needle.empty()) return found;
  for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos;
       pos += needle.size()) {
    if (found.count == 0) found.first = pos;
    ++found.count;
  }
  return found;
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
  out.reserve(text.size() + (crlf ? static_cast<size_t>(std::count(
                                        text.begin(), text.end(), '\n'))
                                  : 0));
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
    int64_t number =
        1 + std::count(data.begin(),
                       data.begin() + static_cast<std::ptrdiff_t>(begin), '\n');
    return "; nearby current line " + std::to_string(number) + ": " +
           Utf8Prefix(data.substr(begin, end - begin), 200);
  }
  return "; reread the current file before retrying";
}

bool EditedSize(size_t current, size_t old_size, size_t new_size, int64_t count,
                size_t& next) {
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
  return next <= kEditFileBytes;
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

struct ResolvedEdit {
  std::string old_text;
  Occurrences found;
  bool normalized_old = false;
};

ResolvedEdit ResolveEditText(const std::string& data, const FileEdit& edit,
                             bool file_crlf) {
  ResolvedEdit resolved{edit.old_text, CountOccurrences(data, edit.old_text),
                        false};
  if (resolved.found.count == 0) {  // copied output from line-numbering readers
    std::string stripped = StripLineNumbers(edit.old_text);
    if (stripped != resolved.old_text) {
      resolved.old_text = std::move(stripped);
      resolved.found = CountOccurrences(data, resolved.old_text);
    }
  }
  if (resolved.found.count == 0) {  // normalized model text, style unchanged
    std::string normalized =
        FileLineEnding(resolved.old_text, file_crlf, /*normalize_crlf=*/true);
    if (normalized != resolved.old_text) {
      resolved.old_text = std::move(normalized);
      resolved.found = CountOccurrences(data, resolved.old_text);
      resolved.normalized_old = resolved.found.count > 0;
    }
  }
  return resolved;
}

// What a batch of edits did to a buffer: the counts the receipt reports and
// the diff it renders.
struct EditRun {
  EditDisplay display;
  int64_t replacements = 0;
  int64_t already_applied = 0;
};

// Apply `edits` to `data`, or return the refusal the caller should report.
// The approval preview and the edit itself both come through here, so what a
// human approves is what gets written.
std::optional<ToolResult> ApplyEdits(std::string& data, const std::string& path,
                                     const std::vector<FileEdit>& edits,
                                     EditRun& run) {
  if (edits.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "at least one edit is required");
  }
  for (size_t i = 0; i < edits.size(); ++i) {
    const FileEdit& edit = edits[i];
    if (edit.old_text.empty()) {
      return ToolFailure(
          ToolErrorCode::kInvalidArguments,
          "edit " + std::to_string(i + 1) + " has an empty `old` value");
    }
    const bool file_crlf = MostlyCrLf(data);
    ResolvedEdit resolved = ResolveEditText(data, edit, file_crlf);
    const std::string& old_eff = resolved.old_text;
    const int64_t count = resolved.found.count;
    if (count == 0) {
      std::string normalized_new =
          FileLineEnding(edit.new_text, file_crlf, /*normalize_crlf=*/true);
      if (!edit.new_text.empty() &&
          (data.find(edit.new_text) != std::string::npos ||
           data.find(normalized_new) != std::string::npos)) {
        ++run.already_applied;
        continue;
      }
      return ToolFailure(ToolErrorCode::kNotFound,
                         "edit " + std::to_string(i + 1) +
                             " `old` not found in " + path +
                             EditRecoveryHint(data, old_eff));
    }
    if (!edit.replace_all && count > 1) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "edit " + std::to_string(i + 1) + " `old` matches " +
                             std::to_string(count) + " times in " + path +
                             "; add surrounding context or set `replace_all`");
    }
    size_t match = resolved.found.first;
    bool replacement_crlf =
        count == 1 ? CrLfAtMatch(data, match, old_eff.size(), file_crlf)
                   : file_crlf;
    std::string new_eff = FileLineEnding(edit.new_text, replacement_crlf,
                                         resolved.normalized_old);
    if (old_eff == new_eff) {
      ++run.already_applied;
      continue;
    }
    int64_t applied = edit.replace_all ? count : 1;
    size_t next_size = 0;
    if (!EditedSize(data.size(), old_eff.size(), new_eff.size(), applied,
                    next_size)) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "edit " + std::to_string(i + 1) +
                             " would exceed the edit byte limit");
    }
    AppendEditDisplay(run.display, data, match, old_eff, new_eff, applied);
    if (edit.replace_all) {
      ReplaceAllOccurrences(data, old_eff, new_eff, next_size);
    } else {
      data.replace(match, old_eff.size(), new_eff);
    }
    run.replacements += applied;
  }
  return std::nullopt;
}

}  // namespace

ToolResult ToolEditFile(const std::string& path,
                        const std::vector<FileEdit>& edits) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kReadableFile)) {
    return std::move(*invalid);
  }
  errno = 0;
  std::ifstream f(path, std::ios::binary);
  if (!f) return FileOpenFailure(path);
  std::error_code size_ec;
  auto bytes = std::filesystem::file_size(path, size_ec);
  if (!size_ec && bytes > kEditFileBytes) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       path + " is too large to edit atomically (" +
                           std::to_string(bytes) + " bytes; limit " +
                           std::to_string(kEditFileBytes) + ")");
  }
  std::string data;
  const bool grew = ReadBounded(f, kEditFileBytes, data);
  f.close();
  if (grew) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       path + " grew beyond the edit limit while reading");
  }

  const size_t original_size = data.size();
  std::string original = data;
  EditRun run;
  if (auto refusal = ApplyEdits(data, path, edits, run)) {
    return std::move(*refusal);
  }
  if (run.replacements == 0) {
    return ToolSuccess("already applied " + path + " (" +
                       std::to_string(run.already_applied) +
                       (run.already_applied == 1 ? " edit)" : " edits)"));
  }
  ToolResult write =
      ToolWriteFile(path, data);  // atomic replace, keeps permissions
  if (!write.Ok()) return write;
  ToolResult result =
      ToolSuccess("edited " + path + " (" + std::to_string(run.replacements) +
                  (run.replacements == 1 ? " replacement across "
                                         : " replacements across ") +
                  std::to_string(edits.size()) +
                  (edits.size() == 1 ? " edit; " : " edits; ") +
                  std::to_string(original_size) + " -> " +
                  std::to_string(data.size()) + " bytes)");
  result.display = DiffReceipt("Edited", path, run.display);
  result.effect = FileEffect{CanonicalAccessPath(path).string(), true,
                             std::move(original), HashHex(data)};
  return result;
}

std::optional<ToolResult> ApplyFileEdits(std::string& data,
                                         const std::string& path,
                                         const std::vector<FileEdit>& edits) {
  EditRun run;
  return ApplyEdits(data, path, edits, run);
}

namespace {

bool LikelyTextSample(std::string_view sample) {
  for (char raw : sample) {
    const unsigned char value = Byte(raw);
    if (value < 0x09 || (value > 0x0d && value < 0x20)) {
      return false;
    }
  }
  return true;
}

bool LikelyTextFile(const std::filesystem::path& path) {
  const std::string mime = AttachmentMime(path.string());
  if (mime.starts_with("image/") ||
      (mime.starts_with("application/") && mime != "application/octet-stream" &&
       mime != "application/json" && mime != "application/xml")) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  char sample[4096];
  input.read(sample, sizeof sample);
  std::streamsize size = input.gcount();
  if (!input && !input.eof()) return false;
  return LikelyTextSample(std::string_view(sample, static_cast<size_t>(size)));
}

// The whole contents of a tiny directory, when every entry is a small text
// file. Any reason not to inline them leaves the plain listing in place.
std::optional<std::string> SmallDirectoryPreview(
    const std::string& dir, const std::vector<std::string>& entries,
    const std::string& listing) {
  namespace fs = std::filesystem;
  uintmax_t total_bytes = 0;
  for (const std::string& entry : entries) {
    fs::path entry_path = fs::path(dir) / entry;
    std::error_code type_error;
    if (!fs::is_regular_file(fs::symlink_status(entry_path, type_error)) ||
        type_error || HiddenPath(entry_path.string())) {
      return std::nullopt;
    }
    std::error_code size_error;
    uintmax_t bytes = fs::file_size(entry_path, size_error);
    if (size_error || bytes > kReadFileBytes - total_bytes) return std::nullopt;
    total_bytes += bytes;
    if (!LikelyTextFile(entry_path)) return std::nullopt;
  }

  std::string preview = listing + "\n[small directory contents]\n";
  for (const std::string& entry : entries) {
    ToolResult read = ToolReadFile((fs::path(dir) / entry).string(), 1, -1);
    if (!read.Ok()) return std::nullopt;
    preview += "\n";
    preview += read.output;
    if (static_cast<int64_t>(preview.size()) > kReadFileResultChars) {
      return std::nullopt;
    }
  }
  return preview;
}

}  // namespace

// Prior contents when a +/- receipt is worth rendering; nullopt for binary,
// oversized, or unreadable files, which are written without a display.
std::optional<std::string> DiffableContents(const std::string& path) {
  std::error_code ec;
  auto bytes = std::filesystem::file_size(path, ec);
  if (ec || bytes > kEditFileBytes) {
    return std::nullopt;
  }
  if (!LikelyTextFile(path)) return std::nullopt;
  return ReadFile(path, kEditFileBytes);
}

ToolResult ToolDeleteFileWithDisplay(const std::string& path) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kDeletableFile)) {
    return std::move(*invalid);
  }
  FileEffect effect{CanonicalAccessPath(path).string(), true,
                    DiffableContents(path), ""};
  // The path policy above already rejected a missing or non-regular target, so
  // remove() reporting nothing removed means it vanished in between.
  std::error_code ec;
  if (!std::filesystem::remove(path, ec)) {
    if (ec) {
      return ToolFailure(FileToolError(ec), "error: cannot delete " + path);
    }
    return ToolFailure(ToolErrorCode::kNotFound,
                       "path does not exist: " + path);
  }
  ToolResult result = ToolSuccess("deleted " + path);
  // Binary/oversized still deletes, just without a diff receipt.
  if (!effect.before || effect.before->empty()) {
    result.display = "Deleted " + DisplayPath(path) + "\n";
  } else {
    result.display = DeletedFileDiffDisplay(path, *effect.before);
  }
  result.effect = std::move(effect);
  return result;
}

ToolResult ToolWriteFileWithDisplay(const std::string& path,
                                    const std::string& content,
                                    bool overwrite) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kWritableFile)) {
    return std::move(*invalid);
  }
  std::error_code ec;
  bool existed = std::filesystem::is_regular_file(path, ec);
  std::optional<std::string> previous;
  if (existed) {
    previous = DiffableContents(path);
  } else if (LikelyTextSample(std::string_view(content).substr(0, 4096))) {
    previous.emplace();
  }
  ToolResult result =
      ToolAtomicWrite(path, content, kSharedFileMode, true, overwrite);
  if (!result.Ok()) return result;
  result.effect =
      FileEffect{CanonicalAccessPath(path).string(), existed,
                 existed ? previous : std::nullopt, HashHex(content)};
  if (!previous) return result;
  result.display = WholeFileDiffDisplay(path, *previous, content, existed);
  return result;
}

std::string WholeFileDiffDisplay(const std::string& path,
                                 const std::string& previous,
                                 const std::string& content, bool existed) {
  std::vector<std::string_view> old_lines = DiffLines(previous);
  std::vector<std::string_view> new_lines = DiffLines(content);
  LineDiff diff = TrimCommonLines(old_lines, new_lines);
  if (diff.old_end == diff.prefix && diff.new_end == diff.prefix) return "";

  EditDisplay display;
  display.added = static_cast<int64_t>(diff.new_end - diff.prefix);
  display.removed = static_cast<int64_t>(diff.old_end - diff.prefix);
  AppendLineDiff(display, old_lines, new_lines, diff);
  return DiffReceipt(existed ? "Replaced" : "Created", path, display);
}

std::string WriteDiffPreview(const std::string& path,
                             const std::string& content) {
  std::error_code ec;
  const bool existed = std::filesystem::is_regular_file(path, ec);
  const std::string diff = WholeFileDiffDisplay(
      path, DiffableContents(path).value_or(""), content, existed);
  return diff.empty() ? "no changes" : diff;
}

std::string DeletedFileDiffDisplay(const std::string& path,
                                   const std::string& previous) {
  std::vector<std::string_view> old_lines = DiffLines(previous);
  std::vector<std::string_view> new_lines;
  LineDiff diff{0, old_lines.size(), 0};
  EditDisplay display;
  display.removed = static_cast<int64_t>(old_lines.size());
  AppendLineDiff(display, old_lines, new_lines, diff);
  return DiffReceipt("Deleted", path, display);
}

ToolResult ToolListDir(const std::string& path, int64_t offset, int64_t limit,
                       bool include_small_files) {
  std::string p = path.empty() ? "." : path;
  if (auto invalid = ValidatePathTarget(p, PathTarget::kDirectory)) {
    return std::move(*invalid);
  }
  if (offset < 1) offset = 1;
  if (limit <= 0) limit = kListDirEntries;
  std::error_code ec;
  std::vector<std::string> entries;
  std::filesystem::directory_iterator iterator(p, ec), iterator_end;
  for (; !ec && iterator != iterator_end; iterator.increment(ec)) {
    const auto& e = *iterator;
    if (static_cast<int64_t>(entries.size()) >= kListDirScanEntries) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "directory exceeds scan limit (" +
                             std::to_string(kListDirScanEntries) + " entries)");
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
  if (offset > static_cast<int64_t>(entries.size())) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "offset is beyond directory entries (" +
                           std::to_string(entries.size()) + ")");
  }
  size_t begin = static_cast<size_t>(offset - 1);
  size_t available = entries.size() - begin;
  size_t count = limit > static_cast<int64_t>(available)
                     ? available
                     : static_cast<size_t>(limit);
  size_t end = begin + count;
  std::string out = "[" + p + " entries " + std::to_string(offset) + "-" +
                    std::to_string(end) + " of " +
                    std::to_string(entries.size()) + "]\n";
  for (size_t i = begin; i < end; ++i) {
    out += entries[i];
    out += '\n';
  }

  constexpr size_t kPreviewFiles = 4;
  if (!include_small_files || offset != 1 || end != entries.size() ||
      entries.size() > kPreviewFiles) {
    return ToolSuccess(std::move(out));
  }
  if (std::optional<std::string> preview =
          SmallDirectoryPreview(p, entries, out)) {
    return ToolSuccess(std::move(*preview), kReadFileResultChars);
  }
  return ToolSuccess(std::move(out));
}

}  // namespace uagent
