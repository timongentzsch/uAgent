// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_FILES_H_
#define UAGENT_INCLUDE_TOOLS_FILES_H_
// File inspection and editing tools, plus the durable memories the agent
// writes for itself. Every write goes through the atomic replace in
// core/fs.h, so a crash cannot leave a truncated file behind.

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {

inline std::string ToolReadFile(const std::string& path, int64_t offset,
                                int64_t limit) {
  if (limit == 0) limit = ReadFileLines();  // 0 = unset
  int64_t max_lines = ReadFileMaxLines();
  if (limit <= 0 || limit > max_lines) limit = max_lines;
  int64_t max_bytes = ReadFileBytes();
  if (offset < 1) offset = 1;
  std::ifstream f(path);
  if (!f) return "error: cannot open " + path;
  std::string line, out;
  int64_t total = 0, shown = 0, first = 0, last = 0;
  bool output_limited = false;
  while (shown < limit && std::getline(f, line)) {
    if (AbortRequested()) return "error: read cancelled";
    ++total;
    if (total >= offset) {
      if (out.size() + line.size() + 1 > static_cast<size_t>(max_bytes)) {
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
      if (AbortRequested()) return "error: read cancelled";
      total += static_cast<int64_t>(std::count(blk, blk + f.gcount(), '\n'));
      ended_nl = blk[f.gcount() - 1] == '\n';
    }
    if (!ended_nl) ++total;  // final line without trailing newline
  }
  if (total == 0) return "(empty file)";
  if (offset > total && !more) {
    return "error: offset " + std::to_string(offset) + " is beyond EOF (" +
           std::to_string(total) + " lines)";
  }
  std::string header = "[" + path + " lines " + std::to_string(first) + "-" +
                       std::to_string(last);
  if (output_limited) {
    header += "; output byte limit reached; more available";
  } else if (more && !ReadFileCountsTotal()) {
    header += "; more available";
  } else {
    header += " of " + std::to_string(total);
  }
  return header + "]\n" + out;
}

// Atomic write: temp file in the same directory, then rename — a disk-full or
// crash mid-write can never leave the target truncated. Keeps an existing
// file's permissions.
inline std::string ToolWriteFileMode(const std::string& path,
                                     const std::string& content,
                                     mode_t create_mode) {
  std::string error;
  if (!AtomicWriteFile(path, content, create_mode, /*preserve_mode=*/true,
                       error)) {
    return "error: " + error;
  }
  return "wrote " + std::to_string(content.size()) + " bytes to " + path;
}

inline std::string ToolWriteFile(const std::string& path,
                                 const std::string& content) {
  return ToolWriteFileMode(path, content, 0644);
}

inline std::string ToolWritePrivateFile(const std::string& path,
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

inline std::string ToolEditFile(const std::string& path,
                                const std::string& old_s,
                                const std::string& new_s) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "error: cannot open " + path;
  std::error_code size_ec;
  auto bytes = std::filesystem::file_size(path, size_ec);
  int64_t max_bytes = EditFileBytes();
  if (!size_ec && max_bytes > 0 && bytes > static_cast<uintmax_t>(max_bytes)) {
    return "error: " + path + " is too large to edit atomically (" +
           std::to_string(bytes) + " bytes; limit " +
           std::to_string(max_bytes) + ")";
  }
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  f.close();
  if (max_bytes > 0 && data.size() > static_cast<size_t>(max_bytes)) {
    return "error: " + path + " grew beyond the edit limit while reading";
  }
  std::string old_eff = old_s;
  int64_t n = CountOccurrences(data, old_eff);
  if (n == 0) {  // retry with read_file line-number prefixes stripped
    std::string stripped = StripLineNumbers(old_s);
    if (stripped != old_s) {
      old_eff = stripped;
      n = CountOccurrences(data, old_eff);
    }
  }
  if (n == 0) return "error: `old` not found in " + path;
  if (n > 1) {
    return "error: `old` matches " + std::to_string(n) + " times in " + path +
           "; it must match exactly once — add surrounding context";
  }
  if (max_bytes > 0 && new_s.size() > old_eff.size() &&
      new_s.size() - old_eff.size() >
          static_cast<size_t>(max_bytes) - data.size()) {
    return "error: edited output would exceed the edit byte limit";
  }
  data.replace(data.find(old_eff), old_eff.size(), new_s);
  std::string w =
      ToolWriteFile(path, data);  // atomic replace, keeps permissions
  if (w.starts_with("error:")) return w;
  return "edited " + path + " (replaced 1 occurrence, " +
         std::to_string(old_eff.size()) + " -> " +
         std::to_string(new_s.size()) + " chars)";
}

inline std::string ToolListDir(const std::string& path, int64_t offset = 0,
                               int64_t limit = 0) {
  std::string p = path.empty() ? "." : path;
  if (offset < 0) offset = 0;
  if (limit <= 0) limit = ListDirEntries();
  int64_t scan_cap = ListDirScanEntries();
  std::error_code ec;
  std::vector<std::string> names;
  for (auto& e : std::filesystem::directory_iterator(p, ec)) {
    if (static_cast<int64_t>(names.size()) >= scan_cap) {
      return "error: directory exceeds scan limit (" +
             std::to_string(scan_cap) + " entries)";
    }
    std::error_code ec2;
    names.push_back(e.path().filename().string() +
                    (e.is_directory(ec2) ? "/" : ""));
  }
  if (ec) return "error: cannot open directory " + p;
  std::sort(names.begin(), names.end());
  if (names.empty()) return "(empty directory)";
  if (offset >= static_cast<int64_t>(names.size())) {
    return "error: offset is beyond directory entries (" +
           std::to_string(names.size()) + ")";
  }
  size_t end = std::min(names.size(), static_cast<size_t>(offset + limit));
  std::string out = "[" + p + " entries " + std::to_string(offset + 1) + "-" +
                    std::to_string(end) + " of " +
                    std::to_string(names.size()) + "]\n";
  for (size_t i = static_cast<size_t>(offset); i < end; ++i) {
    out += names[i];
    out += '\n';
  }
  return out;
}

// Durable notes the agent writes for itself, reloaded with the project
// instructions at the start of every session. Global memories follow the user,
// project memories stay with the workspace that opted into ./.uagent — writing
// one is what creates that directory, and the call needs approval first.
inline std::string ToolMemory(const std::string& name, const std::string& scope,
                              const std::string& content) {
  namespace fs = std::filesystem;
  if (Trim(name).empty()) return "error: memory name must not be empty";
  if (scope != "project" && scope != "global") {
    return "error: scope must be \"project\" or \"global\"";
  }
  int64_t max_bytes = MemoryBytes();
  if (static_cast<int64_t>(content.size()) > max_bytes) {
    return "error: a memory is limited to " + std::to_string(max_bytes) +
           " bytes; keep it to the durable lesson";
  }
  std::error_code ec;
  std::string base = GlobalBase();
  if (scope == "project") {
    fs::path cwd = fs::current_path(ec);
    if (ec) return "error: cannot resolve the workspace: " + ec.message();
    base = (cwd / ".uagent").string();
  }
  std::string dir = MakePrivateDir(base, "memory");
  std::string file = dir + "/" + SafeFileComponent(name) + ".md";
  if (Trim(content).empty()) {
    return fs::remove(file, ec) ? "forgot " + file : "error: no such memory";
  }
  int64_t max_files = MaxMemories();
  if (!fs::exists(file, ec)) {
    int64_t count = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
      count += it->path().extension() == ".md";
    }
    if (count >= max_files) {
      return "error: " + scope + " memory is full (" +
             std::to_string(max_files) +
             "); delete or consolidate one before adding another";
    }
  }
  return ToolWritePrivateFile(file, content);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_FILES_H_
