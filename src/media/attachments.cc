// Copyright 2026 Timon Gentzsch

#include "include/media/attachments.h"

#include <sys/stat.h>
#include <unistd.h>

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
  return {};
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
  if (!ReadRegularFile(path, 32, prefix, error_read, true)) {
    error = error_read;
    return false;
  }
  std::string mime = RasterMime(prefix);
  if (mime.empty()) {
    mime = AttachmentMime(path);
    if (mime.starts_with("image/")) {
      error = "invalid image signature: " + path;
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

namespace {
bool TextMime(const std::string& mime) {
  return mime.starts_with("text/") || mime == "application/json" ||
         mime == "application/xml";
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
#ifdef __APPLE__
  auto inspected = CaptureProcess({"/usr/bin/sips", "-g", "pixelWidth", "-g",
                                   "pixelHeight", attachment.path});
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
  auto inspected = CaptureProcess({"magick", "identify", "-ping", "-format",
                                   "%w %h", attachment.path + "[0]"});
  int width = 0, height = 0;
  std::istringstream(inspected.output) >> width >> height;
#endif
  if ((inspected.error.empty() && !inspected.Ok()) ||
      (inspected.Ok() && (width <= 0 || height <= 0))) {
    error = "image could not be decoded";
    return "";
  }
  std::string path = attachment.path, temporary;
  mime = attachment.mime;
  if (width > 2048 || height > 2048 || attachment.bytes > kImageBytes) {
    const std::string format = mime == "image/jpeg" ? "jpeg" : "png";
    Fd output(CreateTempFile(
        (std::filesystem::temp_directory_path() / "uagent-image-XXXXXX")
            .string(),
        temporary));
    if (!output) {
      error = "cannot prepare image";
      return "";
    }
#ifdef __APPLE__
    auto resized =
        CaptureProcess({"/usr/bin/sips", "-Z", "2048", "-s", "format", format,
                        "-s", "formatOptions", "85", path, "--out", temporary});
#else
    auto resized = CaptureProcess({"magick", "-limit", "memory", "128MiB",
                                   "-limit", "map", "256MiB", path + "[0]",
                                   "-auto-orient", "-resize", "2048x2048>",
                                   "-quality", "85", format + ":" + temporary});
#endif
    if (!resized.Ok()) {
      unlink(temporary.c_str());
      error =
          "image resizing failed; install ImageMagick on Linux or attach a "
          "smaller image";
      return "";
    }
    path = temporary;
    mime = "image/" + format;
  }
  Attachment prepared = attachment;
  prepared.path = path;
  std::string encoded =
      Base64File(prepared, kImageBytes, error, "data:" + mime + ";base64,");
  if (!temporary.empty()) unlink(temporary.c_str());
  if (!error.empty()) return "";
  std::lock_guard lock(mutex);
  cache[key] = {mime, encoded};
  size_t total = 0;
  for (const auto& item : cache) total += item.second.second.size();
  while (total > size_t{16} * 1024 * 1024 && !cache.empty()) {
    total -= cache.begin()->second.second.size();
    cache.erase(cache.begin());
  }
  return encoded;
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
