#pragma once
// Local multimodal attachments for Chat Completions: images use image_url data
// URLs; PDFs/documents use file_data. No upload API or decoding dependency.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "json.hpp"
#include "util.hpp"

using nlohmann::json;

struct Attachment {
    std::string path, name, mime;
    uintmax_t bytes = 0;
    bool image = false;
};

inline long attachment_limit_mb() {
    return std::max(1L, env_long("UAGENT_ATTACHMENT_MB", 10));
}

inline std::string image_detail() {
    std::string detail = env_str("UAGENT_IMAGE_DETAIL");
    return detail == "low" || detail == "high" || detail == "original" || detail == "auto"
               ? detail
               : "";
}

inline bool inspect_attachment(std::string path, Attachment& out, std::string& error) {
    path = unquote(path);
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
    static const std::pair<const char*, const char*> types[] = {
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
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".rtf", "application/rtf"},
        {".odt", "application/vnd.oasis.opendocument.text"},
        {".ppt", "application/vnd.ms-powerpoint"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".xls", "application/vnd.ms-excel"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    };
    for (const auto& [suffix, mime] : types)
        if (ext == suffix) {
            out = {file.string(), file.filename().string(), mime, bytes,
                   std::string(mime).rfind("image/", 0) == 0};
            return true;
        }
    error = "unsupported attachment type `" + ext + "`";
    return false;
}

inline std::string base64_file(const Attachment& attachment, uintmax_t max_bytes,
                               std::string& error,
                               const std::string& prefix = "") {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE* file = fopen(attachment.path.c_str(), "rb");
    if (!file) {
        error = "cannot open " + attachment.path;
        return "";
    }
    struct stat st {};
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
            error = "attachment grew beyond the byte limit while reading: " + attachment.path;
            fclose(file);
            return "";
        }
        out += alphabet[in[0] >> 2];
        out += alphabet[((in[0] & 3) << 4) | (n > 1 ? in[1] >> 4 : 0)];
        out += n > 1 ? alphabet[((in[1] & 15) << 2) | (n > 2 ? in[2] >> 6 : 0)] : '=';
        out += n > 2 ? alphabet[in[2] & 63] : '=';
    }
    if (ferror(file)) {
        error = "failed to read " + attachment.path;
        fclose(file);
        return "";
    }
    fclose(file);
    return out;
}

inline json attachment_content(const std::string& prompt,
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
    long limit_mb = attachment_limit_mb();
    uintmax_t limit = static_cast<uintmax_t>(limit_mb) * 1024 * 1024;
    if (bytes > limit) {
        error = "attachments total " + std::to_string(bytes / (1024 * 1024)) +
                " MB; limit is " + std::to_string(limit_mb) + " MB";
        return nullptr;
    }

    std::string text = prompt + "\n\nAttached:";
    for (const Attachment& attachment : attachments) text += " " + attachment.name;
    json content = json::array({{{"type", "text"}, {"text", text}}});
    for (const Attachment& attachment : attachments) {
        std::string data = base64_file(
            attachment, limit, error,
            "data:" + attachment.mime + ";base64,");
        if (!error.empty()) return nullptr;
        if (attachment.image) {
            json image = {{"url", std::move(data)}};
            std::string detail = image_detail();
            if (!detail.empty()) image["detail"] = detail;
            content.push_back({{"type", "image_url"}, {"image_url", std::move(image)}});
        } else {
            content.push_back(
                {{"type", "file"},
                 {"file", {{"filename", attachment.name}, {"file_data", std::move(data)}}}});
        }
    }
    return content;
}

enum class TerminalImageProtocol { none, iterm, kitty };

inline TerminalImageProtocol terminal_image_protocol() {
    std::string forced = env_str("UAGENT_IMAGE_PROTOCOL");
    if (forced == "iterm") return TerminalImageProtocol::iterm;
    if (forced == "kitty") return TerminalImageProtocol::kitty;
    if (forced == "none") return TerminalImageProtocol::none;
    if (getenv("TMUX")) return TerminalImageProtocol::none;
    std::string program = env_str("TERM_PROGRAM");
    std::string terminal = env_str("LC_TERMINAL");
    std::string term = env_str("TERM");
    if (program == "iTerm.app" || program == "WezTerm" || terminal == "iTerm2")
        return TerminalImageProtocol::iterm;
    if (getenv("KITTY_WINDOW_ID") || term.find("kitty") != std::string::npos)
        return TerminalImageProtocol::kitty;
    return TerminalImageProtocol::none;
}

inline const char* terminal_image_protocol_name(TerminalImageProtocol protocol) {
    if (protocol == TerminalImageProtocol::iterm) return "iterm";
    if (protocol == TerminalImageProtocol::kitty) return "kitty";
    return "none";
}

template <class Emit>
inline void emit_iterm_image(const std::string& data, uintmax_t bytes,
                             long columns, bool multipart, Emit&& emit) {
    std::string options =
        "inline=1;size=" + std::to_string(bytes) + ";width=" +
        std::to_string(columns) + ";height=auto;preserveAspectRatio=1";
    if (!multipart) {
        std::string start = "\033]1337;File=" + options + ":";
        emit(std::string_view(start));
        emit(std::string_view(data));
        emit(std::string_view("\a\n"));
        return;
    }
    std::string start = "\033]1337;MultipartFile=" + options + "\a";
    emit(std::string_view(start));
    constexpr size_t chunk_bytes = 64 * 1024;
    for (size_t offset = 0; offset < data.size(); offset += chunk_bytes) {
        emit(std::string_view("\033]1337;FilePart="));
        emit(std::string_view(data).substr(
            offset, std::min(chunk_bytes, data.size() - offset)));
        emit(std::string_view("\a"));
    }
    emit(std::string_view("\033]1337;FileEnd\a\n"));
}

inline std::string iterm_image_sequence(const std::string& data, uintmax_t bytes,
                                        long columns, bool multipart) {
    std::string output;
    emit_iterm_image(data, bytes, columns, multipart,
                     [&](std::string_view part) { output.append(part); });
    return output;
}

template <class Emit>
inline void emit_kitty_png(const std::string& data, long columns, Emit&& emit) {
    constexpr size_t chunk_bytes = 4096;
    for (size_t offset = 0; offset < data.size(); offset += chunk_bytes) {
        bool first = offset == 0;
        bool more = offset + chunk_bytes < data.size();
        std::string start = "\033_G";
        if (first) start += "a=T,f=100,c=" + std::to_string(columns) + ",q=2,";
        start += std::string("m=") + (more ? "1;" : "0;");
        emit(std::string_view(start));
        emit(std::string_view(data).substr(
            offset, std::min(chunk_bytes, data.size() - offset)));
        emit(std::string_view("\033\\"));
    }
    emit(std::string_view("\n"));
}

inline std::string kitty_png_sequence(const std::string& data, long columns) {
    std::string output;
    emit_kitty_png(data, columns,
                   [&](std::string_view part) { output.append(part); });
    return output;
}

inline std::string tool_view_image(const std::string& path, long columns = 0) {
    if (!g_tty || !isatty(STDOUT_FILENO))
        return "error: inline image display requires an interactive terminal";
    Attachment attachment;
    std::string error;
    if (!inspect_attachment(path, attachment, error)) return "error: " + error;
    if (!attachment.image) return "error: not an image: " + path;
    if (attachment.bytes == 0) return "error: image is empty: " + path;
    long max_columns = std::max(1L, env_long("UAGENT_IMAGE_MAX_COLUMNS", 200));
    if (columns <= 0) columns = env_long("UAGENT_IMAGE_COLUMNS", 80);
    columns = std::clamp(columns, 1L, max_columns);
    long limit_mb = std::max(1L, env_long("UAGENT_TERMINAL_IMAGE_MB", 10));
    uintmax_t limit = static_cast<uintmax_t>(limit_mb) * 1024 * 1024;
    std::string data = base64_file(attachment, limit, error);
    if (!error.empty()) return "error: " + error;

    TerminalImageProtocol protocol = terminal_image_protocol();
    auto write_part = [](std::string_view part) {
        fwrite(part.data(), 1, part.size(), stdout);
    };
    if (protocol == TerminalImageProtocol::iterm) {
        // iTerm 3.5 supports multipart; WezTerm implements the original File
        // form and handles it without requiring its CLI in PATH.
        emit_iterm_image(data, attachment.bytes, columns,
                         env_str("TERM_PROGRAM") == "iTerm.app", write_part);
    } else if (protocol == TerminalImageProtocol::kitty) {
        if (attachment.mime != "image/png")
            return "error: direct Kitty display currently requires a PNG image";
        emit_kitty_png(data, columns, write_part);
    } else {
        std::string program = env_str("TERM_PROGRAM", env_str("TERM", "unknown"));
        return "error: terminal `" + program +
               "` has no supported inline-image protocol; use iTerm2, WezTerm, "
               "or Kitty";
    }
    fflush(stdout);
    return "displayed " + attachment.path + " inline via " +
           terminal_image_protocol_name(protocol);
}
