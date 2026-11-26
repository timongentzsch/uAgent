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
#include "include/tools/image_result.h"

namespace uagent {

namespace {

std::optional<uint32_t> FilePermissions(const std::string& path) {
  struct stat info{};
  if (stat(path.c_str(), &info) != 0) return std::nullopt;
  return info.st_mode & 07777;
}

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

// The lines `content` changes in `previous`; an empty body when none.
EditDisplay WholeFileDiff(const std::string& previous,
                          const std::string& content) {
  std::vector<std::string_view> old_lines = DiffLines(previous);
  std::vector<std::string_view> new_lines = DiffLines(content);
  LineDiff diff = TrimCommonLines(old_lines, new_lines);
  EditDisplay display;
  if (diff.old_end == diff.prefix && diff.new_end == diff.prefix) {
    return display;
  }
  display.added = static_cast<int64_t>(diff.new_end - diff.prefix);
  display.removed = static_cast<int64_t>(diff.old_end - diff.prefix);
  AppendLineDiff(display, old_lines, new_lines, diff);
  return display;
}

EditDisplay DeletedFileDiff(const std::string& previous) {
  std::vector<std::string_view> old_lines = DiffLines(previous);
  std::vector<std::string_view> new_lines;
  LineDiff diff{0, old_lines.size(), 0};
  EditDisplay display;
  display.removed = static_cast<int64_t>(old_lines.size());
  AppendLineDiff(display, old_lines, new_lines, diff);
  return display;
}

ToolResult FileOpenFailure(const std::string& path) {
  std::error_code error(errno, std::generic_category());
  return ToolFailure(FileToolError(error), "cannot open " + path);
}

void AppendUtf8(std::string& out, uint32_t code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
    return;
  }
  if (code < 0x800) {
    out += static_cast<char>(0xC0 | code >> 6);
  } else {
    if (code < 0x10000) {
      out += static_cast<char>(0xE0 | code >> 12);
    } else {
      out += static_cast<char>(0xF0 | code >> 18);
      out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
    }
    out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
  }
  out += static_cast<char>(0x80 | (code & 0x3F));
}

// Whether `text` is UTF-8; `cut` accepts a sequence cut off at the end.
bool Utf8(std::string_view text, bool cut) {
  for (size_t at = 0; at < text.size();) {
    const unsigned char lead = Byte(text[at]);
    const size_t width = lead < 0x80   ? 1
                         : lead < 0xC2 ? 0
                         : lead < 0xE0 ? 2
                         : lead < 0xF0 ? 3
                         : lead < 0xF5 ? 4
                                       : 0;
    if (width == 0) return false;
    for (size_t next = 1; next < width; ++next) {
      if (at + next == text.size()) return cut;
      if ((Byte(text[at + next]) & 0xC0) != 0x80) return false;
    }
    at += width;
  }
  return true;
}

enum class TextEncoding { kUtf8, kUtf16Le, kUtf16Be };

// Scan skipped lines without retaining them; never allocate a complete giant
// line merely to truncate it. Cancellation is checked once per input chunk.
// UTF-16 input is decoded to UTF-8 chunk by chunk.
class FileLines {
 public:
  FileLines(std::istream& input, TextEncoding encoding)
      : input_(input), encoding_(encoding) {}

  bool Next(std::string& line, size_t cap, bool skip, bool& limited) {
    line.clear();
    bool found = false;
    while (!AbortRequested()) {
      if (begin_ == end_) {
        Fill();
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
  void Fill() {
    begin_ = end_ = 0;
    if (encoding_ == TextEncoding::kUtf8) {
      input_.read(buffer_, sizeof buffer_);
      end_ = static_cast<size_t>(input_.gcount());
      return;
    }
    // A two-byte unit becomes at most three bytes, so half a buffer fits.
    char raw[sizeof buffer_ / 2];
    std::string text;
    while (text.empty()) {
      input_.read(raw, sizeof raw);
      const size_t size = static_cast<size_t>(input_.gcount()) & ~size_t{1};
      if (size == 0) break;
      for (size_t at = 0; at < size; at += 2) {
        const uint32_t first = Byte(raw[at]), second = Byte(raw[at + 1]);
        const uint32_t unit = encoding_ == TextEncoding::kUtf16Le
                                  ? first | second << 8
                                  : first << 8 | second;
        uint32_t code = unit;
        if (high_ && unit >= 0xDC00 && unit < 0xE000) {
          code = 0x10000 + ((high_ - 0xD800) << 10) + (unit - 0xDC00);
        } else {
          if (high_) AppendUtf8(text, 0xFFFD);
          if (unit >= 0xD800 && unit < 0xDC00) {
            high_ = unit;
            continue;
          }
          if (unit >= 0xDC00 && unit < 0xE000) code = 0xFFFD;
        }
        high_ = 0;
        AppendUtf8(text, code);
      }
    }
    std::memcpy(buffer_, text.data(), text.size());
    end_ = text.size();
  }

  std::istream& input_;
  TextEncoding encoding_;
  uint32_t high_ = 0;  // a UTF-16 high surrogate awaiting its pair
  char buffer_[8192];
  size_t begin_ = 0;
  size_t end_ = 0;
};

// A notebook field that is a string or a list of them, as one string.
std::string NotebookText(const json& owner, const char* key) {
  const json value = JsonValue(owner, key, json());
  if (value.is_string()) return value.get<std::string>();
  std::string text;
  for (const json& part : value.is_array() ? value : json::array()) {
    if (part.is_string()) text += part.get<std::string>();
  }
  return text;
}

// A cell's source and its text outputs; rich outputs are only named.
std::string NotebookCell(const json& cell, int64_t number) {
  std::string text = "[cell " + std::to_string(number) + " " +
                     JsonValue(cell, "cell_type", "") + "]\n";
  const auto add = [&text](const std::string& part) {
    if (part.empty()) return;
    text += part;
    if (text.back() != '\n') text += '\n';
  };
  add(NotebookText(cell, "source"));
  for (const json& output : JsonValue(cell, "outputs", json::array())) {
    const std::string type = JsonValue(output, "output_type", "");
    if (type == "stream") add("[output]\n" + NotebookText(output, "text"));
    if (type == "error") {
      add("[error] " + JsonValue(output, "ename", "") + ": " +
          JsonValue(output, "evalue", ""));
    }
    const json* data = JsonObject(output, "data");
    if (!data) continue;
    for (const auto& item : data->items()) {
      add(item.key() == "text/plain"
              ? "[output]\n" + NotebookText(*data, "text/plain")
              : "[" + item.key() + " output]");
    }
  }
  return text;
}

// A notebook's cells from `offset` (negative: the last cells); nullopt for a
// file that is not one.
std::optional<ToolResult> ReadNotebook(const std::string& path, int64_t offset,
                                       int64_t limit) {
  const json notebook =
      json::parse(ReadFile(path, EditFileBytes()).value_or(""), nullptr, false);
  const json* cells = JsonArray(notebook, "cells");
  if (!cells) return std::nullopt;
  const auto total = static_cast<int64_t>(cells->size());
  if (total == 0) return ListResult("(empty notebook)", 0, "cell");
  if (offset < 0) offset = std::max<int64_t>(1, total + offset + 1);
  if (offset < 1) offset = 1;
  if (offset > total) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "offset " + std::to_string(offset) +
                           " is beyond the last cell (" +
                           std::to_string(total) + ")");
  }
  if (limit <= 0) limit = total;
  const size_t budget = ReadFileBytes();
  std::string out;
  int64_t last = offset - 1;
  bool cut = false;
  while (last < total && last - offset + 1 < limit) {
    std::string cell =
        NotebookCell((*cells)[static_cast<size_t>(last)], last + 1);
    if (cell.size() > budget - out.size()) {
      if (out.empty()) {
        out = Utf8Prefix(std::move(cell), budget);
        ++last;
        cut = true;
      }
      break;
    }
    out += cell;
    ++last;
  }
  ToolResult result = ListResult(std::move(out), last - offset + 1, "cell");
  result.hint = "[" + path + " cells " + std::to_string(offset) + "-" +
                std::to_string(last) +
                (cut            ? "; cell cut at the output byte limit"
                 : last < total ? "; more available"
                                : " of " + std::to_string(total)) +
                "]";
  return result;
}

// A PDF's text by page through poppler; today's attachment when that is not
// installed or cannot read the file.
ToolResult ReadPdf(const std::string& path, int64_t offset, int64_t limit,
                   const std::string& call_id) {
  const CapturedProcess info = CaptureProcess({"pdfinfo", path});
  int64_t total = 0;
  // Its warnings share the pipe, and the line may be the first.
  const std::string lines = "\n" + info.output;
  if (size_t at = lines.find("\nPages:"); at != std::string::npos) {
    std::istringstream(lines.substr(at + 7)) >> total;
  }
  if (!info.Ok() || total <= 0) {
    ToolResult attached = Attachments().Add(path, call_id);
    if (attached.Ok()) {
      attached.output += info.error.empty()
                             ? "; its text could not be read"
                             : "; install poppler (poppler-utils on "
                               "Debian/Ubuntu) to read its pages as text";
    }
    return attached;
  }
  if (offset < 0) offset = std::max<int64_t>(1, total + offset + 1);
  if (offset < 1) offset = 1;
  if (offset > total) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "offset " + std::to_string(offset) +
                           " is beyond the last page (" +
                           std::to_string(total) + ")");
  }
  const int64_t asked =
      offset - 1 +
      std::min(total - offset + 1, limit > 0 ? limit : kReadPdfPages);
  const CapturedProcess text =
      CaptureProcess({"pdftotext", "-q", "-f", std::to_string(offset), "-l",
                      std::to_string(asked), "-enc", "UTF-8", path, "-"});
  if (!text.Ok()) {
    return ToolFailure(ToolErrorCode::kInternal,
                       "pdftotext failed; ask for fewer pages");
  }
  const size_t budget = ReadFileBytes();
  std::string out;
  int64_t last = offset - 1;
  bool cut = false, any_text = false;
  for (size_t begin = 0; last < asked && begin < text.output.size();) {
    size_t end = text.output.find('\f', begin);
    if (end == std::string::npos) end = text.output.size();
    const std::string_view words =
        std::string_view(text.output).substr(begin, end - begin);
    begin = end + 1;
    const bool blank = Trim(std::string(words)).empty();
    any_text |= !blank;
    std::string page = "[page " + std::to_string(last + 1) + "]" +
                       (blank ? " (no text)\n" : "\n" + std::string(words));
    if (page.back() != '\n') page += '\n';
    if (page.size() > budget - out.size()) {
      if (out.empty()) {
        out = Utf8Prefix(std::move(page), budget);
        ++last;
        cut = true;
      }
      break;
    }
    out += page;
    ++last;
  }
  if (last < offset) {
    return ToolFailure(ToolErrorCode::kInternal, "pdftotext returned no pages");
  }
  ToolResult result = ListResult(std::move(out), last - offset + 1, "page");
  result.hint = "[" + path + " pages " + std::to_string(offset) + "-" +
                std::to_string(last) + " of " + std::to_string(total) +
                (cut ? "; page cut at the output byte limit" : "") +
                (any_text ? "" : "; no text layer, likely scanned") + "]";
  return result;
}

}  // namespace

ToolResult ToolReadFile(const std::string& path, int64_t offset, int64_t limit,
                        const std::string& call_id) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kReadableFile)) {
    return std::move(*invalid);
  }
  if (path.ends_with(".ipynb")) {
    if (auto notebook = ReadNotebook(path, offset, limit)) {
      return std::move(*notebook);
    }
  }
  if (AttachmentMime(path) == "application/pdf") {
    return ReadPdf(path, offset, limit, call_id);
  }
  errno = 0;
  std::ifstream f(path);
  if (!f) return FileOpenFailure(path);
  char mark[3] = {};
  f.read(mark, sizeof mark);
  const std::string_view bom(mark, static_cast<size_t>(f.gcount()));
  TextEncoding encoding = TextEncoding::kUtf8;
  std::streamoff start = 0;
  if (bom.starts_with("\xFF\xFE")) {
    encoding = TextEncoding::kUtf16Le;
  } else if (bom.starts_with("\xFE\xFF")) {
    encoding = TextEncoding::kUtf16Be;
  } else if (bom == "\xEF\xBB\xBF") {
    start = 3;
  }
  if (encoding != TextEncoding::kUtf8) {
    start = 2;
  } else if (!LikelyTextFile(path)) {
    // A range on media is ignored. A region of an image needs its size.
    ToolResult attached = Attachments().Add(path, call_id);
    int width = 0, height = 0;
    if (attached.Ok() && !RasterMime(ReadFile(path, 64).value_or("")).empty() &&
        ImageSize(path, width, height).Ok() && width > 0 && height > 0) {
      attached.output +=
          "; " + std::to_string(width) + "x" + std::to_string(height) + " px";
    }
    return attached;
  }
  f.clear();
  f.seekg(start);
  std::string line, out;
  bool output_limited = false, line_truncated = false, latin1 = false;
  if (offset < 0) {  // the last -offset lines
    FileLines counter(f, encoding);
    int64_t count = 0;
    bool skipped = false;
    while (counter.Next(line, 0, true, skipped)) ++count;
    if (AbortRequested()) return ToolCancelled("error: read cancelled by user");
    offset = std::max<int64_t>(1, count + offset + 1);
    f.clear();
    f.seekg(start);
  }
  if (limit == 0) limit = ReadFileLines();  // 0 = unset
  // A negative limit is the internal "as much as allowed" idiom; only a
  // positive request that the cap reduces is worth reporting back.
  int64_t requested_lines = limit;
  if (limit <= 0 || limit > kReadFileMaxLines) limit = kReadFileMaxLines;
  if (offset < 1) offset = 1;
  int64_t total = 0, shown = 0, first = 0, last = 0;
  FileLines lines(f, encoding);
  const size_t read_bytes = ReadFileBytes();
  while (shown < limit && lines.Next(line, read_bytes - out.size(),
                                     total + 1 < offset, output_limited)) {
    if (AbortRequested()) return ToolCancelled("error: read cancelled by user");
    ++total;
    if (total >= offset) {
      // Text that is not UTF-8 is most likely Latin-1.
      if (!Utf8(line, output_limited)) {
        std::string decoded;
        for (const char raw : line) AppendUtf8(decoded, Byte(raw));
        line = std::move(decoded);
        latin1 = true;
      }
      if (output_limited || line.size() >= read_bytes - out.size()) {
        if (out.empty()) {
          out = Utf8Prefix(std::move(line), read_bytes);
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
  if (total == 0) return ListResult("(empty file)", 0, "line");
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
  // Edits match bytes, which differ from decoded text outside ASCII.
  if (encoding != TextEncoding::kUtf8) header += "; decoded from UTF-16";
  if (latin1) header += "; decoded from Latin-1";
  ToolResult result = ListResult(std::move(out), last - first + 1, "line");
  const std::string range = first == last ? "Line " + std::to_string(first)
                                          : "Lines " + std::to_string(first) +
                                                "–" + std::to_string(last);
  const std::string label = range + (line_truncated ? " · partial line"
                                     : more ? " · more available"
                                            : " of " + std::to_string(total));
  // The existing output owns the bytes. Number its lines when opened.
  result.parts.push_back(
      {{"kind", "code"}, {"label", label}, {"first_line", first}});
  result.body_kind = "fields";
  result.hint = header + "]";
  if (!output_limited) result.read_range = ReadRange{path, first, last};
  return result;
}

ToolResult ToolReadImageRegion(const std::string& path,
                               const std::string& region,
                               const std::string& call_id) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kReadableFile)) {
    return std::move(*invalid);
  }
  int x = -1, y = -1, w = 0, h = 0;
  char comma[3] = {};
  std::istringstream fields(region);
  fields >> x >> comma[0] >> y >> comma[1] >> w >> comma[2] >> h;
  if (!fields || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      std::string_view(comma, 3) != ",,,") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "region is x,y,width,height in pixels");
  }
  if (RasterMime(ReadFile(path, 64).value_or("")).empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "region needs a raster image");
  }
  int width = 0, height = 0;
  const CapturedProcess size = ImageSize(path, width, height);
  if (!size.error.empty()) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "cannot read the image; install ImageMagick on Linux");
  }
  if (width <= 0 || height <= 0) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "image could not be decoded");
  }
  const std::string full =
      std::to_string(width) + "x" + std::to_string(height) + " px";
  if (x >= width || y >= height) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "region starts outside the " + full + " image");
  }
  w = std::min(w, width - x);
  h = std::min(h, height - y);
  ScopedTempFile crop(
      (std::filesystem::temp_directory_path() / "uagent-region-XXXXXX")
          .string());
  if (!crop || !CropImage(path, x, y, w, h, crop.Path()).Ok()) {
    return ToolFailure(ToolErrorCode::kInternal, "cannot cut the region");
  }
  const std::string cut = ReadFile(crop.Path(), MiB(size_t{64})).value_or("");
  return ToolAttachedFile(cut, "region", ".png",
                          "Region " + std::to_string(x) + "," +
                              std::to_string(y) + "," + std::to_string(w) +
                              "," + std::to_string(h) + " of " +
                              DisplayPath(path) + " (" + full + ")",
                          call_id, kArtifactsDir);
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
  size_t match = 0;
  std::istringstream search(old_text);
  std::string line;
  while (std::getline(search, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (Trim(line).size() < 4) continue;
    size_t found = data.find(line);
    if (found != std::string::npos) {
      match = found;
      break;
    }
  }
  size_t begin = match == 0 ? 0 : data.rfind('\n', match - 1) + 1;
  // Two preceding lines of context, then at most five numbered lines.
  for (int i = 0; i < 2 && begin > 0; ++i) {
    begin = begin <= 1 ? 0 : data.rfind('\n', begin - 2) + 1;
  }
  int64_t number =
      1 + std::count(data.begin(),
                     data.begin() + static_cast<std::ptrdiff_t>(begin), '\n');
  std::string out =
      "; current file context (revision " + HashHex(data) + "):\n";
  for (int i = 0; i < 5 && begin < data.size(); ++i) {
    size_t end = data.find('\n', begin);
    if (end == std::string::npos) end = data.size();
    out += std::to_string(number++) + ": " +
           Utf8Prefix(data.substr(begin, end - begin), 220) + "\n";
    begin = end + 1;
  }
  return out;
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
  return next <= EditFileBytes();
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

// Applies `edits` to `data`, or returns the refusal to report. The approval
// preview and the edit both come through here.
std::optional<ToolResult> ApplyEdits(std::string& data, const std::string& path,
                                     const std::vector<FileEdit>& edits,
                                     EditRun& run) {
  if (edits.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "at least one edit is required");
  }
  const std::string original = data;
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
      ToolResult failure = ToolFailure(
          ToolErrorCode::kNotFound,
          "edit " + std::to_string(i + 1) + " `old` not found in " + path +
              "; applied=false (no edits in this batch were written)" +
              EditRecoveryHint(original, old_eff));
      failure.recovery_key =
          HashHex("edit_file\n" + CanonicalAccessPath(path).string() + "\n" +
                  HashHex(original) + "\n" + old_eff);
      return failure;
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
  const FolderLock lock(path);
  errno = 0;
  std::ifstream f(path, std::ios::binary);
  if (!f) return FileOpenFailure(path);
  std::error_code size_ec;
  auto bytes = std::filesystem::file_size(path, size_ec);
  if (!size_ec && bytes > EditFileBytes()) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       path + " is too large to edit atomically (" +
                           std::to_string(bytes) + " bytes; limit " +
                           std::to_string(EditFileBytes()) + ")");
  }
  std::string data;
  const bool grew = ReadBounded(f, EditFileBytes(), data);
  f.close();
  if (grew) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       path + " grew beyond the edit limit while reading");
  }

  const size_t original_size = data.size();
  const auto original_mode = FilePermissions(path);
  std::string original = data;
  EditRun run;
  if (auto refusal = ApplyEdits(data, path, edits, run)) {
    return std::move(*refusal);
  }
  if (run.replacements == 0) {
    ToolResult applied = ToolSuccess("already applied " + path + " (" +
                                     Plural(run.already_applied, "edit") + ")");
    applied.facts.no_change = true;
    return applied;
  }
  ToolResult write =
      ToolWriteFile(path, data);  // atomic replace, keeps permissions
  if (!write.Ok()) return write;
  ToolResult result = ChangeResult(
      "edited " + path + " (" + Plural(run.replacements, "replacement") +
          " across " + Plural(edits.size(), "edit") + "; " +
          std::to_string(original_size) + " -> " + std::to_string(data.size()) +
          " bytes)",
      std::move(run.display.body), run.display.added, run.display.removed);
  result.effect = FileEffect{CanonicalAccessPath(path).string(), true,
                             std::move(original), HashHex(data), original_mode};
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

}  // namespace

bool LikelyTextFile(const std::filesystem::path& path) {
  const std::string mime = AttachmentMime(path.string());
  if (mime.starts_with("image/") ||
      (mime.starts_with("application/") && mime != kOctetStreamMime &&
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

namespace {

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
    if (size_error || bytes > ReadFileBytes() - total_bytes) {
      return std::nullopt;
    }
    total_bytes += bytes;
    if (!LikelyTextFile(entry_path)) return std::nullopt;
  }

  std::string preview = listing;
  for (const std::string& entry : entries) {
    ToolResult read = ToolReadFile((fs::path(dir) / entry).string(), 1, -1);
    if (!read.Ok()) return std::nullopt;
    preview += "\n==> " + entry + " <==\n";
    preview += read.output;
    if (static_cast<int64_t>(preview.size()) > ReadFileResultChars()) {
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
  if (ec || bytes > EditFileBytes()) {
    return std::nullopt;
  }
  if (!LikelyTextFile(path)) return std::nullopt;
  return ReadFile(path, EditFileBytes());
}

ToolResult ToolDeleteFileWithDisplay(const std::string& path) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kDeletableFile)) {
    return std::move(*invalid);
  }
  const FolderLock lock(path);
  FileEffect effect{CanonicalAccessPath(path).string(), true,
                    DiffableContents(path), "", FilePermissions(path)};
  // The path policy above already rejected a missing or non-regular target, so
  // remove() reporting nothing removed means it vanished in between.
  std::error_code ec;
  if (!std::filesystem::remove(path, ec)) {
    if (ec) {
      return ToolFailure(FileToolError(ec), "cannot delete " + path);
    }
    return ToolFailure(ToolErrorCode::kNotFound,
                       "path does not exist: " + path);
  }
  // Binary/oversized still deletes, just without a diff.
  ToolResult result = ToolSuccess("deleted " + path);
  if (effect.before && !effect.before->empty()) {
    EditDisplay diff = DeletedFileDiff(*effect.before);
    result = ChangeResult(std::move(result.output), std::move(diff.body),
                          diff.added, diff.removed);
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
  const FolderLock lock(path);
  std::error_code ec;
  bool existed = std::filesystem::is_regular_file(path, ec);
  const auto original_mode = existed ? FilePermissions(path) : std::nullopt;
  std::optional<std::string> previous;
  if (existed) {
    previous = DiffableContents(path);
  } else if (LikelyTextSample(std::string_view(content).substr(0, 4096))) {
    previous.emplace();
  }
  ToolResult result =
      ToolAtomicWrite(path, content, kSharedFileMode, true, overwrite);
  if (!result.Ok()) return result;
  if (previous) {
    EditDisplay diff = WholeFileDiff(*previous, content);
    if (!diff.body.empty()) {
      result = ChangeResult(std::move(result.output), std::move(diff.body),
                            diff.added, diff.removed);
    } else {
      result.facts.no_change = existed && *previous == content;
    }
  }
  result.facts.outcome = existed ? "replaced" : "created";
  result.effect = FileEffect{CanonicalAccessPath(path).string(), existed,
                             existed ? std::move(previous) : std::nullopt,
                             HashHex(content), original_mode};
  return result;
}

std::string WholeFileDiffDisplay(const std::string& path,
                                 const std::string& previous,
                                 const std::string& content, bool existed) {
  EditDisplay display = WholeFileDiff(previous, content);
  if (display.body.empty()) return "";
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
  return DiffReceipt("Deleted", path, DeletedFileDiff(previous));
}

ToolResult ToolListDir(const std::string& path, int64_t offset, int64_t limit,
                       bool include_small_files) {
  std::string p = path.empty() ? "." : path;
  if (auto invalid = ValidatePathTarget(p, PathTarget::kDirectory)) {
    return std::move(*invalid);
  }
  if (offset < 1) offset = 1;
  if (limit <= 0) limit = ListDirEntries();
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
    return ToolFailure(FileToolError(ec), "cannot open directory " + p);
  }
  std::sort(entries.begin(), entries.end());
  if (entries.empty()) return ListResult("(empty directory)", 0, "entry");
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
  std::string out;
  for (size_t i = begin; i < end; ++i) {
    out += entries[i];
    out += '\n';
  }
  const std::string header = "[" + p + " entries " + std::to_string(offset) +
                             "-" + std::to_string(end) + " of " +
                             std::to_string(entries.size()) + "]";

  constexpr size_t kPreviewFiles = 4;
  std::optional<std::string> preview;
  if (include_small_files && offset == 1 && end == entries.size() &&
      entries.size() <= kPreviewFiles) {
    preview = SmallDirectoryPreview(p, entries, out);
  }
  ToolResult result = ListResult(preview ? std::move(*preview) : std::move(out),
                                 static_cast<int64_t>(count), "entry");
  result.hint = header;
  if (offset > 1 || end < entries.size()) {
    result.parts = json::array({FieldsPart(
        {{"Entries", std::to_string(offset) + "–" + std::to_string(end) +
                         " of " + std::to_string(entries.size())}})});
  }
  if (preview) {
    AddHint(result, "[small directory contents]");
    result.result_chars = ReadFileResultChars();
  }
  return result;
}

}  // namespace uagent
