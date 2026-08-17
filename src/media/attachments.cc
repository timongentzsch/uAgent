// Copyright 2026 Timon Gentzsch

#include "include/media/attachments.h"

#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/env.h"
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

std::string ImageDetail() {
  std::string detail = EnvStr("UAGENT_IMAGE_DETAIL");
  return detail == "low" || detail == "high" || detail == "original" ||
                 detail == "auto"
             ? detail
             : "";
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
  std::string ext = AsciiLower(file.extension().string());
  for (const auto& [suffix, mime] : kTypes) {
    if (ext == suffix) {
      out = {file.string(), file.filename().string(), mime, bytes,
             std::string_view(mime).starts_with("image/")};
      return true;
    }
  }
  error = "unsupported attachment type `" + ext + "`";
  return false;
}

std::string ImageInputError(const Attachment& attachment,
                            bool image_input_available,
                            bool image_fallback_available) {
  if (!attachment.image || image_input_available || image_fallback_available) {
    return "";
  }
  return "this model rejected image input for " + attachment.path +
         "; configure UAGENT_IMAGE_MODEL to analyze attached images with a "
         "vision-capable model";
}

const char* ModelImageInputInstruction(bool image_input_available,
                                       bool image_fallback_available) {
  if (image_input_available) return "";
  return !image_fallback_available
             ? " Image input unavailable; image attachments are provided "
               "only as file paths."
             : " Direct image input unavailable; attached images are "
               "analyzed by the configured vision model and delivered as "
               "textual evidence.";
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
  unsigned char in[3];
  uintmax_t read_bytes = 0;
  for (size_t n; (n = fread(in, 1, 3, file.get())) > 0;) {
    read_bytes += n;
    if (read_bytes > max_bytes) {
      error = "attachment grew beyond the byte limit while reading: " +
              attachment.path;
      return "";
    }
    out += kBase64Alphabet[in[0] >> 2];
    out += kBase64Alphabet[((in[0] & 3) << 4) | (n > 1 ? in[1] >> 4 : 0)];
    out += n > 1
               ? kBase64Alphabet[((in[1] & 15) << 2) | (n > 2 ? in[2] >> 6 : 0)]
               : '=';
    out += n > 2 ? kBase64Alphabet[in[2] & 63] : '=';
    if (n < 3) break;
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
                       std::string& error, bool image_input_available,
                       bool image_fallback_available) {
  uintmax_t bytes = 0;
  for (const Attachment& attachment : attachments) {
    error = ImageInputError(attachment, image_input_available,
                            image_fallback_available);
    if (!error.empty()) return nullptr;
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
    error = "attachments total " + std::to_string(bytes / (1024 * 1024)) +
            " MB; limit is " + std::to_string(limit_mb) + " MB";
    return nullptr;
  }

  std::string text = prompt + "\n\nAttached:";
  for (const Attachment& attachment : attachments) {
    text += " " + attachment.path;
  }
  json content = json::array({{{"type", "text"}, {"text", text}}});
  for (const Attachment& attachment : attachments) {
    std::string data = Base64File(attachment, limit, error,
                                  "data:" + attachment.mime + ";base64,");
    if (!error.empty()) return nullptr;
    if (attachment.image) {
      json image = {{"url", std::move(data)}};
      std::string detail = ImageDetail();
      if (!detail.empty()) image["detail"] = detail;
      content.push_back(
          {{"type", "image_url"}, {"image_url", std::move(image)}});
    } else {
      content.push_back(
          {{"type", "file"},
           {"file",
            {{"filename", attachment.name}, {"file_data", std::move(data)}}}});
    }
  }
  return content;
}

size_t StripImageContentParts(json& messages) {
  size_t rewritten = 0;
  for (json& message : messages) {
    if (!message.contains("content") || !message["content"].is_array()) {
      continue;
    }
    json kept = json::array();
    size_t dropped = 0;
    for (json& part : message["content"]) {
      if (JsonValue(part, "type", "") == "image_url") {
        ++dropped;
      } else {
        kept.push_back(std::move(part));
      }
    }
    if (!dropped) continue;
    message["content"] = std::move(kept);
    ++rewritten;
  }
  return rewritten;
}

ToolResult AttachmentQueue::Add(const std::string& path,
                                bool image_input_available,
                                bool image_fallback_available) {
  Attachment attachment;
  std::string error;
  if (!InspectAttachment(path, attachment, error)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments, "error: " + error);
  }
  if (!(error = ImageInputError(attachment, image_input_available,
                                image_fallback_available))
           .empty()) {
    return ToolFailure(ToolErrorCode::kUnavailable, "error: " + error);
  }
  std::string result = "attached " + attachment.name + " (" + attachment.mime +
                       "); readable in your next step";
  std::lock_guard<std::mutex> lock(mutex_);
  // MCP servers queue images without a model call to budget against, so the
  // ceiling lives here rather than only on the attach tool.
  if (static_cast<int64_t>(pending_.size()) >= MaxPendingAttachments()) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: too many attachments pending for one step (" +
                           std::to_string(MaxPendingAttachments()) + ")");
  }
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
