// Copyright 2026 Timon Gentzsch

#include "include/media/attachments.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/core/capture.h"
#include "include/core/checked.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Extension to MIME type. Also read backwards to name a saved image, so the
// first entry for a type is the one its files get (.jpg, never .jpeg).
constexpr std::pair<const char*, const char*> kTypes[] = {
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".webp", "image/webp"},
    {".gif", "image/gif"},
    {".heic", "image/heic"},
    {".heif", "image/heif"},
    {".svg", "image/svg+xml"},
    {".pdf", "application/pdf"},
    {".txt", "text/plain"},
    {".md", "text/markdown"},
    {".json", "application/json"},
    {".html", "text/html"},
    {".xml", "application/xml"},
    {".csv", "text/csv"},
    {".tsv", "text/tsv"},
    {".doc", "application/msword"},
    {".docx",
     "application/"
     "vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".rtf", "application/rtf"},
    {".odt", "application/vnd.oasis.opendocument.text"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pptx",
     "application/"
     "vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".xls", "application/vnd.ms-excel"},
    {".xlsx",
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".mp3", "audio/mpeg"},
    {".wav", "audio/wav"},
    {".ogg", "audio/ogg"},
    {".oga", "audio/ogg"},
    {".flac", "audio/flac"},
    {".m4a", "audio/mp4"},
    {".opus", "audio/opus"},
    {".aac", "audio/aac"},
    {".aiff", "audio/aiff"},
    {".aif", "audio/aiff"},
    {".mp4", "video/mp4"},
    {".mov", "video/quicktime"},
    {".webm", "video/webm"},
    {".mpeg", "video/mpeg"},
    {".mpg", "video/mpeg"},
};

}  // namespace

// Empty for anything that is not an image: callers use that to reject
// non-image content outright.
std::string ImageExtension(const std::string& mime) {
  if (!std::string_view(mime).starts_with("image/")) return "";
  for (const auto& [suffix, value] : kTypes) {
    if (mime == value) return suffix;
  }
  return "";
}

std::string RasterMime(std::string_view bytes) {
  if (bytes.size() >= 24 &&
      bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) &&
      bytes.substr(12, 4) == "IHDR") {
    return "image/png";
  }
  if (bytes.size() >= 4 &&
      bytes.starts_with(std::string_view("\xff\xd8\xff", 3))) {
    return "image/jpeg";
  }
  if (bytes.size() >= 13 &&
      (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a"))) {
    return "image/gif";
  }
  if (bytes.size() >= 16 && bytes.starts_with("RIFF") &&
      bytes.substr(8, 4) == "WEBP") {
    return "image/webp";
  }
  // HEIC/HEIF stills share the ISO BMFF `ftyp` header with MP4 video, so
  // the brand allowlist is the whole check: still-photo brands in, video
  // brands (isom, mp41, avc1, ...) out. The brand sits at offset 8.
  if (bytes.size() >= 12 && bytes.substr(4, 4) == "ftyp") {
    const std::string_view brand = bytes.substr(8, 4);
    for (std::string_view still : {"heic", "heix", "hevc", "hevx", "heim",
                                   "heis", "hevm", "hevs", "mif1", "msf1"}) {
      if (brand == still) return "image/heic";
    }
  }
  return {};
}

// Video twin of the still check above: the same ftyp box with a video
// brand means a video container. Extension-declared videos with unlisted
// brands still pass below on the declared type; only image magic wins over
// a video claim, never the reverse.
std::string VideoMime(std::string_view bytes) {
  if (bytes.size() >= 12 && bytes.substr(4, 4) == "ftyp") {
    const std::string_view brand = bytes.substr(8, 4);
    for (std::string_view video :
         {"isom", "iso2", "mp41", "mp42", "avc1", "mmp4", "mp71"}) {
      if (brand == video) return "video/mp4";
    }
  }
  return {};
}

// Speech containers have real magic too: ID3 tags, RIFF/WAVE, FLAC, OggS.
// Anything else rides on the declared extension, same as documents.
std::string AudioMime(std::string_view bytes) {
  if (bytes.size() >= 3 && bytes.starts_with("ID3")) return "audio/mpeg";
  if (bytes.size() >= 12 && bytes.starts_with("RIFF") &&
      bytes.substr(8, 4) == "WAVE") {
    return "audio/wav";
  }
  if (bytes.size() >= 4 && bytes.starts_with("fLaC")) return "audio/flac";
  if (bytes.size() >= 4 && bytes.starts_with("OggS")) return "audio/ogg";
  return {};
}

// SVG has no magic number: scan the prolog (BOM, whitespace, <?...?>,
// <!--...-->, <!DOCTYPE...>) for the <svg root element, at most ScanBytes
// in. Anything else -- including bare <?xml without svg, HTML, or plain
// text -- is not an SVG document.
std::string SvgMime(std::string_view bytes) {
  constexpr size_t kScanBytes = size_t{64} * 1024;
  const size_t end = std::min(bytes.size(), kScanBytes);
  size_t pos = 0;
  if (end - pos >= 3 && bytes.substr(pos, 3) == "\xEF\xBB\xBF") pos += 3;
  auto skip_blank = [&] {
    while (pos < end && (bytes[pos] == ' ' || bytes[pos] == '\t' ||
                         bytes[pos] == '\r' || bytes[pos] == '\n')) {
      ++pos;
    }
  };
  auto skip_to = [&](std::string_view mark) {
    const size_t found = bytes.find(mark, pos);
    if (found == std::string_view::npos || found > end) return false;
    pos = found + mark.size();
    return true;
  };
  for (;;) {
    skip_blank();
    if (pos >= end) return "";
    if (bytes[pos] != '<') return "";
    if (bytes.substr(pos, 4) == "<!--") {
      pos += 4;
      if (!skip_to("-->")) return "";
      continue;
    }
    if (bytes.substr(pos, 2) == "<?") {
      pos += 2;
      if (!skip_to("?>")) return "";
      continue;
    }
    if (bytes.substr(pos, 9) == "<!DOCTYPE" ||
        bytes.substr(pos, 9) == "<!doctype") {
      // A doctype can hide '>' inside an internal [...] subset.
      size_t depth = 0;
      while (pos < end) {
        if (bytes[pos] == '[') ++depth;
        if (bytes[pos] == ']') depth -= depth > 0 ? 1 : 0;
        ++pos;
        if (depth == 0 && bytes[pos - 1] == '>') break;
      }
      if (pos > end) return "";
      continue;
    }
    break;
  }
  // The document element itself must be svg (case-insensitive for
  // hand-written files), followed by a tag delimiter.
  auto lower = [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
  };
  constexpr std::string_view kTag = "<svg";
  if (pos + kTag.size() > end) return "";
  for (size_t i = 0; i < kTag.size(); ++i) {
    if (lower(bytes[pos + i]) != kTag[i]) return "";
  }
  pos += kTag.size();
  if (pos >= end) return "";
  const char next = bytes[pos];
  if (next != ' ' && next != '\t' && next != '\r' && next != '\n' &&
      next != '>' && next != '/') {
    return "";
  }
  return "image/svg+xml";
}

bool IsRasterMime(const std::string& mime) {
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" ||
         mime == "image/webp";
}

bool IsAudioMime(const std::string& mime) { return mime.starts_with("audio/"); }

bool IsVideoMime(const std::string& mime) { return mime.starts_with("video/"); }

std::string AudioFormat(const std::string& mime) {
  if (mime == "audio/wav") return "wav";
  if (mime == "audio/mpeg") return "mp3";
  if (mime == "audio/flac") return "flac";
  if (mime == "audio/ogg") return "ogg";
  if (mime == "audio/mp4") return "m4a";
  if (mime == "audio/opus") return "opus";
  if (mime == "audio/aac") return "aac";
  if (mime == "audio/aiff") return "aiff";
  return "wav";
}

std::string ImageDetail() {
  std::string detail = EnvStr("UAGENT_IMAGE_DETAIL");
  return detail == "low" || detail == "high" || detail == "original" ||
                 detail == "auto"
             ? detail
             : "";
}

std::string AttachmentMime(const std::string& name) {
  std::string ext =
      AsciiLower(std::filesystem::path(name).extension().string());
  for (const auto& [suffix, mime] : kTypes) {
    if (ext == suffix) return mime;
  }
  return "application/octet-stream";
}

bool InspectAttachment(std::string path, Attachment& out, std::string& error) {
  path = Unquote(path);
  std::error_code ec;
  std::filesystem::path file(path);
  uintmax_t bytes = std::filesystem::file_size(file, ec);
  if (ec) {
    error = "cannot read " + path;
    return false;
  }
  if (!std::filesystem::is_regular_file(file, ec)) {
    error = "attachment is not a regular file";
    return false;
  }
  std::string prefix, error_read;
  // One head read covers the raster/HEIC signatures as well as the SVG
  // prolog scan below; vector documents may legitimately start with
  // kilobytes of XML declarations before <svg.
  if (!ReadRegularFile(path, size_t{64} * 1024, prefix, error_read, true)) {
    error = error_read;
    return false;
  }
  std::string mime = RasterMime(prefix);
  if (mime.empty()) mime = SvgMime(prefix);
  if (mime.empty()) mime = AudioMime(prefix);
  if (mime.empty()) mime = VideoMime(prefix);
  if (mime.empty()) {
    mime = AttachmentMime(path);
    if (IsRasterMime(mime)) {
      error = "invalid image signature: " + path;
      return false;
    }
    if (mime == "image/svg+xml") {
      error = "not an SVG document: " + path;
      return false;
    }
  }
  auto absolute = std::filesystem::absolute(file, ec);
  if (ec) {
    error = "cannot resolve attachment path";
    return false;
  }
  out = {absolute.string(),
         file.filename().string(),
         mime,
         bytes,
         mime.starts_with("image/"),
         {}};
  return true;
}

// Whether images arrive natively or through a configured vision route is a
// host detail, not something the model should plan around: with either one in
// place the prompt says nothing at all, and only a session that can do neither
// is told to expect file paths.
const char* ModelImageInputInstruction(bool image_input_available,
                                       bool image_fallback_available) {
  if (image_input_available || image_fallback_available) return "";
  return " Image input unavailable; image attachments are provided only as "
         "file paths.";
}

const char* ModelAudioInputInstruction(bool audio_input_available) {
  if (audio_input_available) return "";
  return " Audio input unavailable; audio attachments are provided only as "
         "file paths.";
}

const char* ModelVideoInputInstruction(bool video_input_available) {
  if (video_input_available) return "";
  return " Video input unavailable; video attachments are provided only as "
         "file paths.";
}

std::string Base64File(const Attachment& attachment, uintmax_t max_bytes,
                       std::string& error, const std::string& prefix) {
  std::unique_ptr<FILE, int (*)(FILE*)> file(
      fopen(attachment.path.c_str(), "rb"), &fclose);
  if (!file) {
    error = "cannot open " + attachment.path;
    return "";
  }
  struct stat st{};
  if (fstat(fileno(file.get()), &st) != 0 || !S_ISREG(st.st_mode)) {
    error = "attachment is not a regular file: " + attachment.path;
    return "";
  }
  uintmax_t current_bytes = static_cast<uintmax_t>(st.st_size);
  if (current_bytes > max_bytes) {
    error = "attachment exceeds remaining byte limit: " + attachment.path;
    return "";
  }
  std::string out = prefix;
  if (current_bytes > std::numeric_limits<size_t>::max()) {
    error = "attachment is too large to encode: " + attachment.path;
    return "";
  }
  std::optional<size_t> padded =
      CheckedAdd(static_cast<size_t>(current_bytes), 2);
  std::optional<size_t> encoded =
      padded ? CheckedMul(*padded / 3, 4) : std::nullopt;
  std::optional<size_t> reserved =
      encoded ? CheckedAdd(prefix.size(), *encoded) : std::nullopt;
  if (!reserved) {
    error = "attachment size overflow";
    return "";
  }
  out.reserve(*reserved);
  std::vector<unsigned char> block(size_t{48} * 1024);
  uintmax_t read_bytes = 0;
  size_t held = 0;  // bytes of an incomplete triple carried into the next read
  // Stop at end of file rather than reading once more to discover it: a short
  // read already sets the flag, and reading a failed stream leaves its
  // position indeterminate.
  while (!feof(file.get()) && !ferror(file.get())) {
    size_t n = fread(block.data() + held, 1, block.size() - held, file.get());
    if (n == 0) break;
    read_bytes += n;
    if (read_bytes > max_bytes) {
      error = "attachment grew beyond the byte limit while reading: " +
              attachment.path;
      return "";
    }
    size_t whole = (held + n) - (held + n) % 3;
    for (size_t i = 0; i < whole; i += 3) {
      const unsigned char* in = block.data() + i;
      out += kBase64Alphabet[in[0] >> 2];
      out += kBase64Alphabet[((in[0] & 3) << 4) | (in[1] >> 4)];
      out += kBase64Alphabet[((in[1] & 15) << 2) | (in[2] >> 6)];
      out += kBase64Alphabet[in[2] & 63];
    }
    held = held + n - whole;
    for (size_t i = 0; i < held; ++i) block[i] = block[whole + i];
  }
  if (held > 0) {
    unsigned char second = held > 1 ? block[1] : 0;
    out += kBase64Alphabet[block[0] >> 2];
    out += kBase64Alphabet[((block[0] & 3) << 4) | (second >> 4)];
    out += held > 1 ? kBase64Alphabet[(second & 15) << 2] : '=';
    out += '=';
  }
  if (!ferror(file.get())) return out;
  error = "failed to read " + attachment.path;
  return "";
}

bool Base64Decode(std::string_view input, std::string& output,
                  size_t max_bytes) {
  static const auto kTable = [] {
    std::array<int, 256> values{};
    values.fill(-1);
    for (int i = 0; i < 64; ++i) {
      values[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
    }
    return values;
  }();
  if (input.size() % 4 != 0) return false;
  std::optional<size_t> decoded = CheckedMul(input.size() / 4, 3);
  if (!decoded) return false;
  if (!input.empty() && input.back() == '=') --*decoded;
  if (input.size() >= 2 && input[input.size() - 2] == '=') --*decoded;
  if (*decoded > max_bytes) return false;
  output.clear();
  output.reserve(*decoded);
  for (size_t i = 0; i < input.size(); i += 4) {
    int a = kTable[static_cast<unsigned char>(input[i])];
    int b = kTable[static_cast<unsigned char>(input[i + 1])];
    int c = input[i + 2] == '='
                ? 0
                : kTable[static_cast<unsigned char>(input[i + 2])];
    int d = input[i + 3] == '='
                ? 0
                : kTable[static_cast<unsigned char>(input[i + 3])];
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (input[i + 2] == '=' && input[i + 3] != '=') ||
        (i + 4 != input.size() &&
         (input[i + 2] == '=' || input[i + 3] == '='))) {
      return false;
    }
    output.push_back(static_cast<char>((a << 2) | (b >> 4)));
    if (input[i + 2] != '=') {
      output.push_back(static_cast<char>((b << 4) | (c >> 2)));
    }
    if (input[i + 3] != '=') output.push_back(static_cast<char>((c << 6) | d));
    if (output.size() > max_bytes) return false;
  }
  return true;
}

json AttachmentContent(const std::string& prompt,
                       const std::vector<Attachment>& attachments,
                       std::string& error) {
  uintmax_t bytes = 0;
  for (const Attachment& attachment : attachments) {
    std::error_code ec;
    uintmax_t current = std::filesystem::file_size(attachment.path, ec);
    if (ec || !std::filesystem::is_regular_file(attachment.path, ec)) {
      error = "cannot read regular attachment " + attachment.path;
      return nullptr;
    }
    if (current > std::numeric_limits<uintmax_t>::max() - bytes) {
      error = "attachment size overflow";
      return nullptr;
    }
    bytes += current;
  }
  int64_t limit_mb = AttachmentLimitMb();
  uintmax_t limit = static_cast<uintmax_t>(limit_mb) * 1024 * 1024;
  if (bytes > limit) {
    error = "attachments total " +
            std::to_string(bytes / (size_t{1024} * 1024)) + " MB; limit is " +
            std::to_string(limit_mb) + " MB";
    return nullptr;
  }

  std::string text = prompt + "\n\nAttached:";
  for (const Attachment& attachment : attachments) {
    text += "\n- path " + JsonDump(attachment.path);
    if (!attachment.source_call_id.empty()) {
      text += " (from tool call " + JsonDump(attachment.source_call_id) + ")";
    }
  }
  json content = json::array({{{"type", "text"}, {"text", text}}});
  for (const Attachment& attachment : attachments) {
    content.push_back({{"type", "attachment"},
                       {"path", attachment.path},
                       {"name", attachment.name},
                       {"mime", attachment.mime},
                       {"bytes", attachment.bytes},
                       {"id", attachment.asset_id}});
  }
  return content;
}

Attachment AttachmentFromJson(const json& item) {
  Attachment attachment;
  attachment.path = JsonValue(item, "path", "");
  attachment.name = JsonValue(item, "name", "attachment");
  attachment.mime = JsonValue(item, "mime", "application/octet-stream");
  attachment.image = JsonValue(item, "image", false);
  attachment.asset_id = JsonValue(item, "id", "");
  return attachment;
}

std::pair<json, bool> ComposeSteeredContent(const std::string& input,
                                            const json& attachments,
                                            std::string& error) {
  std::vector<Attachment> files;
  if (attachments.is_array()) {
    for (const json& item : attachments) {
      Attachment file = AttachmentFromJson(item);
      if (!file.path.empty()) files.push_back(std::move(file));
    }
  }
  if (files.empty()) return {json(input), false};
  json content = AttachmentContent(input, files, error);
  if (!error.empty()) return {json(input), false};
  return {std::move(content), true};
}

namespace {
bool TextMime(const std::string& mime) {
  return mime.starts_with("text/") || mime == "application/json" ||
         mime == "application/xml";
}

// SVG has no wire-safe bytes: rasterize once to PNG, then run the normal
// inspect/resize/encode flow on the raster. ImageMagick first on both
// platforms (its built-in renderer needs no daemon); QuickLook covers
// stock macOS when ImageMagick is missing or its SVG coder is locked
// down. Providers never see raw SVG.
bool RasterizeVector(const std::string& path, const std::string& out,
                     std::string& error) {
  auto raster = CaptureProcess({"magick", "-limit", "memory", "128MiB",
                                "-limit", "map", "256MiB", "-density", "192",
                                "-background", "none", path + "[0]", "-resize",
                                "2048x2048>", "-strip", "png:" + out});
  if (raster.Ok()) return true;
#ifdef __APPLE__
  const std::string dir = std::filesystem::path(out).parent_path().string();
  auto thumb = CaptureProcess(
      {"/usr/bin/qlmanage", "-t", "-s", "2048", "-o", dir, path});
  const std::string rendered =
      dir + "/" + std::filesystem::path(path).filename().string() + ".png";
  std::error_code ec;
  if (thumb.Ok() && rendered != out) {
    std::filesystem::rename(rendered, out, ec);
    if (!ec) return true;
  }
#endif
  error =
      "cannot prepare SVG image; install ImageMagick (brew install "
      "imagemagick) or attach a PNG instead";
  return false;
}
std::string PreparedImage(const Attachment& attachment, std::string& mime,
                          std::string& error) {
  constexpr size_t kImageBytes = size_t{4} * 1024 * 1024;
  static std::mutex mutex;
  static std::map<std::string, std::pair<std::string, std::string>> cache;
  std::error_code ec;
  auto stamp = std::filesystem::last_write_time(attachment.path, ec);
  if (ec) {
    error = "image is no longer available";
    return "";
  }
  std::string key =
      attachment.path + ":" +
      std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         stamp.time_since_epoch())
                         .count()) +
      ":" + std::to_string(attachment.bytes);
  {
    std::lock_guard lock(mutex);
    if (auto found = cache.find(key); found != cache.end()) {
      mime = found->second.first;
      return found->second.second;
    }
  }
  // Platform helpers keep image decoders out of the harness binary. No shell,
  // bounded lifetime, original retained. Linux uses ImageMagick when installed.
  // Vector originals rasterize first: neither inspector below reads SVG,
  // and the raster re-enters the normal inspect/resize/encode flow.
  std::string path = attachment.path;
  // Scratch files live exactly as long as this function: every early return
  // below used to unlink them by hand.
  ScopedTempFile raster_file(
      (std::filesystem::temp_directory_path() / "uagent-svg-XXXXXX").string());
  ScopedTempFile temporary_file(
      (std::filesystem::temp_directory_path() / "uagent-image-XXXXXX")
          .string());
  mime = attachment.mime;
  if (mime == "image/svg+xml") {
    if (!raster_file) {
      error = "cannot prepare image";
      return "";
    }
    if (!RasterizeVector(attachment.path, raster_file.Path(), error)) {
      return "";
    }
    path = raster_file.Path();
    mime = "image/png";
  }
#ifdef __APPLE__
  auto inspected = CaptureProcess(
      {"/usr/bin/sips", "-g", "pixelWidth", "-g", "pixelHeight", path});
  int width = 0, height = 0;
  std::istringstream lines(inspected.output);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string label;
    int value = 0;
    if (fields >> label >> value) {
      if (label == "pixelWidth:") width = value;
      if (label == "pixelHeight:") height = value;
    }
  }
#else
  auto inspected = CaptureProcess(
      {"magick", "identify", "-ping", "-format", "%w %h", path + "[0]"});
  int width = 0, height = 0;
  std::istringstream(inspected.output) >> width >> height;
#endif
  if ((inspected.error.empty() && !inspected.Ok()) ||
      (inspected.Ok() && (width <= 0 || height <= 0))) {
    error = "image could not be decoded";
    return "";
  }
  // HEIC/HEIF data URIs are rejected by most vision endpoints, so they
  // always normalize through the converter below instead of passing
  // through like small PNGs do. A missing converter then fails loudly
  // here instead of degrading the whole route on a provider rejection.
  const bool normalize = mime == "image/heic" || mime == "image/heif";
  if (width > kMaxImageDimension || height > kMaxImageDimension ||
      attachment.bytes > kImageBytes || normalize) {
    const std::string format =
        (mime == "image/jpeg" || mime == "image/heic" || mime == "image/heif")
            ? "jpeg"
            : "png";
    if (!temporary_file) {
      error = "cannot prepare image";
      return "";
    }
#ifdef __APPLE__
    auto resized = CaptureProcess({"/usr/bin/sips", "-Z",
                                   std::to_string(kMaxImageDimension), "-s",
                                   "format", format, "-s", "formatOptions",
                                   "85", path, "--out", temporary_file.Path()});
#else
    const std::string geometry = std::to_string(kMaxImageDimension) + "x" +
                                 std::to_string(kMaxImageDimension) + ">";
    auto resized = CaptureProcess(
        {"magick", "-limit", "memory", "128MiB", "-limit", "map", "256MiB",
         path + "[0]", "-auto-orient", "-resize", geometry, "-quality", "85",
         format + ":" + temporary_file.Path()});
#endif
    if (!resized.Ok()) {
      error =
          "image resizing failed; install ImageMagick on Linux or attach a "
          "smaller image";
      return "";
    }
    path = temporary_file.Path();
    mime = "image/" + format;
  }
  Attachment prepared = attachment;
  prepared.path = path;
  std::string encoded =
      Base64File(prepared, kImageBytes, error, "data:" + mime + ";base64,");
  if (!error.empty()) return "";
  std::lock_guard lock(mutex);
  cache[key] = {mime, encoded};
  size_t total = 0;
  for (const auto& item : cache) total += item.second.second.size();
  while (total > kImageCacheBytes && !cache.empty()) {
    total -= cache.begin()->second.second.size();
    cache.erase(cache.begin());
  }
  return encoded;
}
}  // namespace

namespace {
// Identity for within-request dedup: same bytes on disk. Size and mtime
// catch edits; an unreadable mtime disables dedup for the part.
std::string AttachmentFingerprint(const Attachment& attachment) {
  if (attachment.path.empty()) return "";
  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(attachment.path, ec);
  if (ec) return "";
  const int64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            mtime.time_since_epoch())
                            .count();
  return attachment.path + "|" + std::to_string(attachment.bytes) + "|" +
         std::to_string(nanos);
}
}  // namespace

bool PrepareAttachments(json& messages,
                        const ProviderCapabilities& capabilities,
                        bool vision_fallback, const std::string& route,
                        std::string& error, json* deliveries) {
  bool changed = false;
  uintmax_t remaining =
      static_cast<uintmax_t>(AttachmentLimitMb()) * 1024 * 1024;
  if (deliveries) *deliveries = json::array();
  // Fingerprints of parts already prepared in this request. Re-reading an
  // unchanged file (the model re-reading an attached screenshot, the same
  // path queued twice in one step) must reference the first copy instead
  // of duplicating payload and re-rendering the attachment row on every
  // agent step. Path, size and mtime catch edits; a changed file prepares
  // normally with its own delivery row.
  std::map<std::string, std::string> delivered;
  for (size_t index = 0; index < messages.size(); ++index) {
    json& message = messages[index];
    if (!message.contains("content") || !message["content"].is_array()) {
      continue;
    }
    json prepared = json::array();
    for (const json& part : message["content"]) {
      const std::string type = JsonValue(part, "type", "");
      if (type != "attachment") {
        if ((type == "file" && !capabilities.file_input) ||
            (type == "image_url" && !capabilities.image_input &&
             !vision_fallback)) {
          prepared.push_back({{"type", "text"},
                              {"text",
                               "[attachment not visible to this model; use the "
                               "original file path]"}});
          changed = true;
        } else {
          prepared.push_back(part);
        }
        continue;
      }
      changed = true;
      const std::string path = JsonValue(part, "path", ""),
                        name = JsonValue(part, "name", "attachment"),
                        stored_mime = JsonValue(part, "mime", "");
      std::string delivery = "File reference", detail;
      Attachment attachment;
      const bool processed =
          JsonValue(part, "processed_route", "") == route && !route.empty();
      if (!processed && InspectAttachment(path, attachment, detail)) {
        const std::string fingerprint = AttachmentFingerprint(attachment);
        if (!fingerprint.empty() && delivered.contains(fingerprint)) {
          prepared.push_back(
              {{"type", "text"},
               {"text", "[File reference: " + path + "; earlier attachment]"}});
          continue;
        }
        if (attachment.bytes > remaining) {
          detail = "request attachment budget exceeded";
        } else {
          remaining -= attachment.bytes;
        }
      }
      if (!processed && detail.empty()) {
        if (attachment.mime == "application/octet-stream") {
          attachment.mime = stored_mime;
        }
        if (TextMime(attachment.mime)) {
          std::string text;
          if (ReadRegularFile(path, size_t{64} * 1024, text, detail, true) &&
              text.find('\0') == std::string::npos) {
            prepared.push_back(
                {{"type", "text"},
                 {"text",
                  "[Attached text: " + name + "]\n" +
                      Utf8Prefix(std::move(text), size_t{64} * 1024) +
                      (attachment.bytes > size_t{64} * 1024
                           ? "\n[truncated; read the original file for more]"
                           : "")}});
            delivery = "Text";
          }
        } else if (attachment.image &&
                   (capabilities.image_input || vision_fallback)) {
          std::string mime;
          std::string data = PreparedImage(attachment, mime, detail);
          if (!data.empty()) {
            json image = {{"url", std::move(data)}};
            if (!ImageDetail().empty()) image["detail"] = ImageDetail();
            prepared.push_back(
                {{"type", "image_url"}, {"image_url", std::move(image)}});
            delivery = capabilities.image_input ? "Image" : "Via vision model";
          }
        } else if (attachment.mime == "application/pdf" &&
                   capabilities.file_input) {
          std::string header;
          if (ReadRegularFile(path, 5, header, detail, true) &&
              header == "%PDF-") {
            std::string data = Base64File(
                attachment,
                static_cast<uintmax_t>(AttachmentLimitMb()) * 1024 * 1024,
                detail, "data:application/pdf;base64,");
            if (detail.empty()) {
              prepared.push_back(
                  {{"type", "file"},
                   {"file",
                    {{"filename", name}, {"file_data", std::move(data)}}}});
              delivery = "Document";
            }
          } else {
            detail = "invalid PDF signature";
          }
        } else if (IsAudioMime(attachment.mime) && capabilities.audio_input) {
          // OpenRouter takes raw base64 plus a format word, never a data
          // URI, and no audio URLs at all.
          std::string data = Base64File(
              attachment,
              static_cast<uintmax_t>(AttachmentLimitMb()) * 1024 * 1024, detail,
              "");
          if (detail.empty()) {
            prepared.push_back({{"type", "input_audio"},
                                {"input_audio",
                                 {{"data", std::move(data)},
                                  {"format", AudioFormat(attachment.mime)}}}});
            delivery = "Audio";
          }
        } else if (IsVideoMime(attachment.mime) && capabilities.video_input) {
          // Local files ride as base64 data URLs, mirroring image_url;
          // remote URLs stay provider-specific and are out of scope.
          std::string data = Base64File(
              attachment,
              static_cast<uintmax_t>(AttachmentLimitMb()) * 1024 * 1024, detail,
              "data:" + attachment.mime + ";base64,");
          if (detail.empty()) {
            prepared.push_back({{"type", "video_url"},
                                {"video_url", {{"url", std::move(data)}}}});
            delivery = "Video";
          }
        }
      }
      if (!detail.empty()) {
        if (!error.empty()) error += "\n";
        error += name + ": " + detail;
      }
      if (delivery == "File reference") {
        prepared.push_back(
            {{"type", "text"},
             {"text", "[File reference: " + path +
                          (processed ? "; earlier attachment"
                                     : "; contents not included") +
                          "]"}});
      } else if (detail.empty()) {
        // Successful first delivery: later identical parts in this request
        // reference it (see above) instead of duplicating payload.
        const std::string fingerprint = AttachmentFingerprint(attachment);
        if (!fingerprint.empty()) delivered[fingerprint] = name;
      }
      if (deliveries && !processed) {
        deliveries->push_back({{"message_index", index},
                               {"id", JsonValue(part, "id", "")},
                               {"name", name},
                               {"delivery", delivery},
                               {"path", path}});
      }
    }
    message["content"] = std::move(prepared);
  }
  return changed;
}

ToolResult AttachmentQueue::Add(const std::string& path,
                                std::string source_call_id) {
  Attachment attachment;
  std::string error;
  if (!InspectAttachment(path, attachment, error)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments, "error: " + error);
  }
  std::string result =
      "attached " + attachment.name + "; queued for the next request";
  std::lock_guard<std::mutex> lock(mutex_);
  // MCP servers queue images without a model call to budget against, so the
  // ceiling lives here rather than only on read_path.
  if (static_cast<int64_t>(pending_.size()) >= MaxPendingAttachments()) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: too many attachments pending for one step (" +
                           std::to_string(MaxPendingAttachments()) + ")");
  }
  attachment.source_call_id = std::move(source_call_id);
  pending_.push_back(std::move(attachment));
  return ToolSuccess(std::move(result));
}

std::vector<Attachment> AttachmentQueue::Take() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(pending_, {});
}

AttachmentQueue& Attachments() {
  static AttachmentQueue attachments;
  return attachments;
}

}  // namespace uagent
