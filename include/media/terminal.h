// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MEDIA_TERMINAL_H_
#define UAGENT_INCLUDE_MEDIA_TERMINAL_H_

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/child_env.h"
#include "include/core/env.h"
#include "include/core/platform.h"
#include "include/core/term.h"
#include "include/media/attachments.h"
#include "include/tools/tool.h"

namespace uagent {

enum class TerminalImageProtocol { kNone, kIterm, kItty };

inline TerminalImageProtocol DetectTerminalImageProtocol() {
  std::string forced = EnvStr("UAGENT_IMAGE_PROTOCOL");
  if (forced == "iterm") return TerminalImageProtocol::kIterm;
  if (forced == "kitty") return TerminalImageProtocol::kItty;
  if (forced == "ascii" || forced == "none") {
    return TerminalImageProtocol::kNone;
  }
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
  if (!g_tty || DetectTerminalImageProtocol() == TerminalImageProtocol::kNone) {
    return "";
  }
  return " Use show_image to display local images in the terminal.";
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
                                     "--size=" + std::to_string(columns) + "x",
                                     "--",
                                     path};
  std::vector<char*> args;
  args.reserve(values.size() + 1);
  for (std::string& value : values) args.push_back(value.data());
  args.push_back(nullptr);
  fflush(stdout);
  pid_t pid = 0;
  ChildEnvironment environment;
  if (posix_spawnp(&pid, "chafa", nullptr, nullptr, args.data(),
                   environment.Data())) {
    return false;
  }
  int status = 0;
  if (WaitPid(pid, &status) < 0) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline ToolResult ToolShowImage(const std::string& path, int64_t columns = 0) {
  // Persistent interactive mode captures stdout so it can insert output above
  // the composer. g_tty was resolved before that redirection and remains the
  // authoritative capability check.
  if (!g_tty) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: image display requires an interactive terminal");
  }
  Attachment attachment;
  std::string error;
  if (!InspectAttachment(path, attachment, error)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments, "error: " + error);
  }
  if (!attachment.image) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: not an image: " + path);
  }
  if (attachment.bytes == 0) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: image is empty: " + path);
  }
  int64_t available = std::max(int64_t{1}, TerminalColumns() - 1);
  int64_t max_columns = std::min(available, ImageMaxColumns());
  if (columns <= 0) columns = ImageColumns(available);
  columns = std::clamp(columns, int64_t{1}, max_columns);
  TerminalImageProtocol protocol = DetectTerminalImageProtocol();
  if (protocol == TerminalImageProtocol::kNone) {
    return ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: this terminal does not support native inline images");
  }
  if (protocol == TerminalImageProtocol::kItty &&
      attachment.mime != "image/png") {
    if (!ExecutableOnPath("chafa")) {
      return ToolFailure(
          ToolErrorCode::kUnavailable,
          "error: Kitty displays PNG directly; install Chafa for other "
          "formats");
    }
    if (!EmitChafaKittyImage(attachment.path, columns)) {
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "error: Chafa could not convert " + attachment.path +
                             " to Kitty format");
    }
    return ToolSuccess("displayed " + attachment.path + " inline via kitty");
  }
  int64_t limit_mb = TerminalImageLimitMb();
  uintmax_t limit = static_cast<uintmax_t>(limit_mb) * 1024 * 1024;
  std::string data = Base64File(attachment, limit, error);
  if (!error.empty()) {
    return ToolFailure(ToolErrorCode::kInternal, "error: " + error);
  }

  auto write_part = [](std::string_view part) {
    fwrite(part.data(), 1, part.size(), stdout);
  };
  if (protocol == TerminalImageProtocol::kIterm) {
    EmitItermImage(data, attachment.bytes, columns,
                   EnvStr("TERM_PROGRAM") == "iTerm.app", write_part);
  } else {
    EmitKittyPng(data, columns, write_part);
  }
  fflush(stdout);
  return ToolSuccess("displayed " + attachment.path + " inline via " +
                     TerminalImageProtocolName(protocol));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MEDIA_TERMINAL_H_
