// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MEDIA_H_
#define UAGENT_INCLUDE_MEDIA_H_
// Local multimodal attachments for Chat Completions: images use image_url data
// URLs; PDFs/documents use file_data. No upload API or decoding dependency.

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "third_party/json.hpp"

extern char** environ;

namespace uagent {

using nlohmann::json;

struct Attachment {
  std::string path, name, mime;
  uintmax_t bytes = 0;
  bool image = false;
};

inline std::string ImageExtension(const std::string& mime) {
  if (mime == "image/png") return ".png";
  if (mime == "image/jpeg") return ".jpg";
  if (mime == "image/webp") return ".webp";
  if (mime == "image/gif") return ".gif";
  return "";
}

inline std::string ImageDetail() {
  std::string detail = EnvStr("UAGENT_IMAGE_DETAIL");
  return detail == "low" || detail == "high" || detail == "original" ||
                 detail == "auto"
             ? detail
             : "";
}

inline bool InspectAttachment(std::string path, Attachment& out,
                              std::string& error) {
  path = Unquote(path);
  std::error_code ec;
  std::filesystem::path file(path);
  uintmax_t bytes = std::filesystem::file_size(file, ec);
  if (ec) {
    error = "cannot read " + path;
    return false;
  }
  std::string ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  static const std::pair<const char*, const char*> kTypes[] = {
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
  for (const auto& [suffix, mime] : kTypes) {
    if (ext == suffix) {
      out = {file.string(), file.filename().string(), mime, bytes,
             std::string(mime).starts_with("image/")};
      return true;
    }
  }
  error = "unsupported attachment type `" + ext + "`";
  return false;
}

// Files the model asked to read. Drained into the next request as user content;
// attachment_content owns the byte limit, the tool's call budget the count.
class AttachmentQueue {
 public:
  std::string Add(const std::string& path) {
    Attachment attachment;
    std::string error;
    if (!InspectAttachment(path, attachment, error)) return "error: " + error;
    std::string result = "attached " + attachment.name + " (" +
                         attachment.mime + "); readable in your next step";
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(attachment));
    return result;
  }
  std::vector<Attachment> Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(pending_, {});
  }

 private:
  std::mutex mutex_;
  std::vector<Attachment> pending_;
};

inline AttachmentQueue g_attachments;

inline std::string Base64File(const Attachment& attachment, uintmax_t max_bytes,
                              std::string& error,
                              const std::string& prefix = "") {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  FILE* file = fopen(attachment.path.c_str(), "rb");
  if (!file) {
    error = "cannot open " + attachment.path;
    return "";
  }
  struct stat st{};
  if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode)) {
    error = "attachment is not a regular file: " + attachment.path;
    fclose(file);
    return "";
  }
  uintmax_t current_bytes = static_cast<uintmax_t>(st.st_size);
  if (current_bytes > max_bytes) {
    error = "attachment exceeds remaining byte limit: " + attachment.path;
    fclose(file);
    return "";
  }
  std::string out = prefix;
  out.reserve(prefix.size() + static_cast<size_t>((current_bytes + 2) / 3 * 4));
  unsigned char in[3];
  uintmax_t read_bytes = 0;
  while (!feof(file)) {
    size_t n = fread(in, 1, 3, file);
    if (!n) break;
    read_bytes += n;
    if (read_bytes > max_bytes) {
      error = "attachment grew beyond the byte limit while reading: " +
              attachment.path;
      fclose(file);
      return "";
    }
    out += kAlphabet[in[0] >> 2];
    out += kAlphabet[((in[0] & 3) << 4) | (n > 1 ? in[1] >> 4 : 0)];
    out +=
        n > 1 ? kAlphabet[((in[1] & 15) << 2) | (n > 2 ? in[2] >> 6 : 0)] : '=';
    out += n > 2 ? kAlphabet[in[2] & 63] : '=';
  }
  if (ferror(file)) {
    error = "failed to read " + attachment.path;
    fclose(file);
    return "";
  }
  fclose(file);
  return out;
}

inline bool Base64Decode(std::string_view input, std::string& output,
                         size_t max_bytes) {
  static constexpr signed char kInvalid = -1;
  static const auto kTable = [] {
    std::array<signed char, 256> values{};
    values.fill(kInvalid);
    constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) {
      values[static_cast<unsigned char>(kAlphabet[i])] =
          static_cast<signed char>(i);
    }
    return values;
  }();
  if (input.size() % 4 != 0 || input.size() / 4 * 3 > max_bytes + 2) {
    return false;
  }
  output.clear();
  output.reserve(std::min(max_bytes, input.size() / 4 * 3));
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

inline json AttachmentContent(const std::string& prompt,
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
    error = "attachments total " + std::to_string(bytes / (1024 * 1024)) +
            " MB; limit is " + std::to_string(limit_mb) + " MB";
    return nullptr;
  }

  std::string text = prompt + "\n\nAttached:";
  for (const Attachment& attachment : attachments) {
    text += " " + attachment.name;
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

enum class TerminalImageProtocol { kNone, kIterm, kItty };

inline TerminalImageProtocol DetectTerminalImageProtocol() {
  std::string forced = EnvStr("UAGENT_IMAGE_PROTOCOL");
  if (forced == "iterm") return TerminalImageProtocol::kIterm;
  if (forced == "kitty") return TerminalImageProtocol::kItty;
  if (forced == "ascii") return TerminalImageProtocol::kNone;
  if (forced == "none") return TerminalImageProtocol::kNone;
  if (!getenv("TMUX")) {  // tmux passes no native protocol through reliably
    std::string program = EnvStr("TERM_PROGRAM");
    std::string term = EnvStr("TERM");
    if (program == "iTerm.app" || program == "WezTerm" ||
        EnvStr("LC_TERMINAL") == "iTerm2") {
      return TerminalImageProtocol::kIterm;
    }
    if (getenv("KITTY_WINDOW_ID") || program == "ghostty" ||
        term.find("kitty") != std::string::npos ||
        term.find("ghostty") != std::string::npos) {
      return TerminalImageProtocol::kItty;
    }
  }
  return TerminalImageProtocol::kNone;
}

inline const char* TerminalImageProtocolName(TerminalImageProtocol protocol) {
  if (protocol == TerminalImageProtocol::kIterm) return "iterm";
  if (protocol == TerminalImageProtocol::kItty) return "kitty";
  return "none";
}

inline const char* TerminalImageInstruction() {
  if (!g_tty) return " Images unavailable.";  // before the PATH probe below
  TerminalImageProtocol protocol = DetectTerminalImageProtocol();
  if (protocol == TerminalImageProtocol::kNone) return " Images unavailable.";
  return " Images: show_image (native).";
}

template <class Emit>
inline void EmitItermImage(const std::string& data, uintmax_t bytes,
                           int64_t columns, bool multipart, Emit&& emit) {
  std::string options = "inline=1;size=" + std::to_string(bytes) +
                        ";width=" + std::to_string(columns) +
                        ";height=auto;preserveAspectRatio=1";
  if (!multipart) {
    std::string start = "\033]1337;File=" + options + ":";
    emit(std::string_view(start));
    emit(std::string_view(data));
    emit(std::string_view("\a\n"));
    return;
  }
  std::string start = "\033]1337;MultipartFile=" + options + "\a";
  emit(std::string_view(start));
  constexpr size_t kChunkBytes = 64 * 1024;
  for (size_t offset = 0; offset < data.size(); offset += kChunkBytes) {
    emit(std::string_view("\033]1337;FilePart="));
    emit(std::string_view(data).substr(
        offset, std::min(kChunkBytes, data.size() - offset)));
    emit(std::string_view("\a"));
  }
  emit(std::string_view("\033]1337;FileEnd\a\n"));
}

inline std::string ItermImageSequence(const std::string& data, uintmax_t bytes,
                                      int64_t columns, bool multipart) {
  std::string output;
  EmitItermImage(data, bytes, columns, multipart,
                 [&](std::string_view part) { output.append(part); });
  return output;
}

template <class Emit>
inline void EmitKittyPng(const std::string& data, int64_t columns,
                         Emit&& emit) {
  constexpr size_t kChunkBytes = 4096;
  for (size_t offset = 0; offset < data.size(); offset += kChunkBytes) {
    bool first = offset == 0;
    bool more = offset + kChunkBytes < data.size();
    std::string start = "\033_G";
    if (first) start += "a=T,f=100,c=" + std::to_string(columns) + ",q=2,";
    start += std::string("m=") + (more ? "1;" : "0;");
    emit(std::string_view(start));
    emit(std::string_view(data).substr(
        offset, std::min(kChunkBytes, data.size() - offset)));
    emit(std::string_view("\033\\"));
  }
  emit(std::string_view("\n"));
}

inline std::string KittyPngSequence(const std::string& data, int64_t columns) {
  std::string output;
  EmitKittyPng(data, columns,
               [&](std::string_view part) { output.append(part); });
  return output;
}

inline bool EmitChafaKittyImage(const std::string& path, int64_t columns) {
  std::vector<std::string> values = {"chafa",
                                     "--format=kitty",
                                     "--probe=off",
                                     "--animate=off",
                                     "--relative=off",
                                     "--polite=on",
                                     "--size=" + std::to_string(columns) + "x"};
  values.insert(values.end(), {"--", path});
  std::vector<char*> args;
  for (std::string& value : values) args.push_back(value.data());
  args.push_back(nullptr);
  fflush(stdout);
  pid_t pid = 0;
  if (posix_spawnp(&pid, "chafa", nullptr, nullptr, args.data(), environ) !=
      0) {
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline std::string ToolShowImage(const std::string& path, int64_t columns = 0) {
  if (!g_tty || !isatty(STDOUT_FILENO)) {
    return "error: image display requires an interactive terminal";
  }
  Attachment attachment;
  std::string error;
  if (!InspectAttachment(path, attachment, error)) return "error: " + error;
  if (!attachment.image) return "error: not an image: " + path;
  if (attachment.bytes == 0) return "error: image is empty: " + path;
  int64_t available = std::max(int64_t{1}, TerminalColumns() - 1);
  int64_t max_columns = std::min(available, ImageMaxColumns());
  if (columns <= 0) columns = ImageColumns(available);
  columns = std::clamp(columns, int64_t{1}, max_columns);
  TerminalImageProtocol protocol = DetectTerminalImageProtocol();
  if (protocol == TerminalImageProtocol::kNone) {
    return "error: this terminal does not support native inline images";
  }
  if (protocol == TerminalImageProtocol::kItty &&
      attachment.mime != "image/png") {
    if (!ExecutableOnPath("chafa")) {
      return "error: Kitty displays PNG directly; install Chafa for other "
             "formats";
    }
    if (!EmitChafaKittyImage(attachment.path, columns)) {
      return "error: Chafa could not convert " + attachment.path +
             " to Kitty format";
    }
    return "displayed " + attachment.path + " inline via kitty";
  }
  int64_t limit_mb = TerminalImageLimitMb();
  uintmax_t limit = static_cast<uintmax_t>(limit_mb) * 1024 * 1024;
  std::string data = Base64File(attachment, limit, error);
  if (!error.empty()) return "error: " + error;

  auto write_part = [](std::string_view part) {
    fwrite(part.data(), 1, part.size(), stdout);
  };
  if (protocol == TerminalImageProtocol::kIterm) {
    // iTerm 3.5 supports multipart; WezTerm implements the original File
    // form and handles it without requiring its CLI in PATH.
    EmitItermImage(data, attachment.bytes, columns,
                   EnvStr("TERM_PROGRAM") == "iTerm.app", write_part);
  } else {  // kitty; none returned above
    EmitKittyPng(data, columns, write_part);
  }
  fflush(stdout);
  return "displayed " + attachment.path + " inline via " +
         TerminalImageProtocolName(protocol);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MEDIA_H_
