// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/app/session_host.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace {

namespace fs = std::filesystem;

// The browser shows what the file tools may read: nothing hidden from them
// and none of µAgent's own configuration.
bool Shown(const fs::path& path) {
  return !HiddenPath(path.string()) && !SelfConfigurationPath(path.string());
}

// `relative` resolved under `root`, when it stays inside and may be shown.
std::optional<fs::path> Resolve(const fs::path& root,
                                const std::string& relative) {
  if (fs::path(relative).is_absolute()) return std::nullopt;
  fs::path target = CanonicalAccessPath((root / relative).string());
  if (!PathWithin(target, root) || !Shown(target)) return std::nullopt;
  return target;
}

// A folder's entries, folders first. A symlink shows as what it points to,
// and only when that is inside `root`.
std::optional<json> List(const fs::path& root, const fs::path& folder) {
  struct Entry {
    std::string name;
    fs::path path;
    bool dir;
    uintmax_t size;
  };
  std::vector<Entry> entries;
  bool more = false;
  std::error_code ec;
  fs::directory_iterator iterator(folder, ec), end;
  for (; !ec && iterator != end; iterator.increment(ec)) {
    if (entries.size() == static_cast<size_t>(kListDirScanEntries)) {
      more = true;
      break;
    }
    fs::path path = iterator->path();
    std::error_code type_error;
    fs::file_status status = iterator->symlink_status(type_error);
    if (fs::is_symlink(status)) {
      path = CanonicalAccessPath(path.string());
      if (!PathWithin(path, root)) continue;
      status = fs::status(path, type_error);
    }
    const bool directory = fs::is_directory(status);
    const uintmax_t size = directory ? 0 : fs::file_size(path, type_error);
    if (type_error || (!directory && !fs::is_regular_file(status))) continue;
    entries.push_back(
        {iterator->path().filename().string(), path, directory, size});
  }
  if (ec) return std::nullopt;
  std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
    return a.dir != b.dir ? a.dir : AsciiLower(a.name) < AsciiLower(b.name);
  });
  // Checked only for what is shown: the policy resolves every path it asks
  // about, too slow for a whole large folder.
  json rows = json::array();
  for (const Entry& entry : entries) {
    if (!Shown(entry.path)) continue;
    if (rows.size() == static_cast<size_t>(kListDirEntries)) {
      more = true;
      break;
    }
    rows.push_back(
        {{"name", entry.name}, {"dir", entry.dir}, {"size", entry.size}});
  }
  return json{{"entries", std::move(rows)}, {"more", more}};
}

// Whether `query`'s characters, spaces aside, appear in `path` in order.
bool Matches(const std::string& query, const std::string& path) {
  size_t at = 0;
  for (const char ch : query) {
    if (ch == ' ') continue;
    at = path.find(ch, at);
    if (at == std::string::npos) return false;
    ++at;
  }
  return true;
}

// Files under `root` that match `query`, nearest first. Neither .git nor a
// symlinked folder is entered.
json Find(const fs::path& root, const std::string& query) {
  const std::string wanted = AsciiLower(query);
  json paths = json::array();
  std::deque<fs::path> folders = {root};
  int64_t scanned = 0;
  bool more = false;
  while (!folders.empty() && !more) {
    std::error_code ec;
    fs::directory_iterator iterator(folders.front(), ec), end;
    folders.pop_front();
    for (; !ec && iterator != end; iterator.increment(ec)) {
      if (++scanned > kListDirScanEntries || paths.size() == kFileFindMatches) {
        more = true;
        break;
      }
      const fs::path& path = iterator->path();
      std::error_code type_error;
      const fs::file_status status = iterator->symlink_status(type_error);
      if (fs::is_directory(status)) {
        if (path.filename() != ".git" && Shown(path)) folders.push_back(path);
        continue;
      }
      std::string relative = path.lexically_relative(root).string();
      if (fs::is_regular_file(status) &&
          Matches(wanted, AsciiLower(relative)) && Shown(path)) {
        paths.push_back(std::move(relative));
      }
    }
  }
  return {{"paths", std::move(paths)}, {"more", more}};
}

}  // namespace

void Master::Files(const Request& request, Response& response) {
  std::string folder;
  {
    std::lock_guard lock(mutex_);
    folder = host_.Folder(request.matches[1].str());
  }
  if (folder.empty()) {
    Error(response, "unknown session", 404);
    return;
  }
  const int modes = request.has_param("list") + request.has_param("read") +
                    request.has_param("raw") + request.has_param("find");
  if (modes != 1) {
    Error(response, "name one of list, read, raw or find");
    return;
  }
  std::optional<fs::path> root = CanonicalDirectory(folder);
  if (!root) {
    Error(response, "not found", 404);
    return;
  }
  if (request.has_param("find")) {
    Reply(response, Find(*root, request.get_param_value("find")));
    return;
  }
  const std::string mode = request.has_param("list")   ? "list"
                           : request.has_param("read") ? "read"
                                                       : "raw";
  std::optional<fs::path> target =
      Resolve(*root, request.get_param_value(mode));
  std::optional<json> listing;
  if (target && mode == "list") listing = List(*root, *target);
  if (listing) {
    Reply(response, *listing);
    return;
  }
  // Read a byte past the text shown, so a cut is seen.
  const bool read = mode == "read";
  std::error_code ec;
  const uintmax_t size = target ? fs::file_size(*target, ec) : 0;
  std::string bytes, error;
  if (!target || mode == "list" || ec ||
      !ReadRegularFile(target->string(),
                       read ? kFileViewBytes + 1 : kUploadBytes, bytes, error,
                       read)) {
    Error(response, "not found", 404);
    return;
  }
  if (!read) {
    // Only an image opens in the browser; anything else, HTML included,
    // downloads, so no file in the folder runs on this origin.
    std::string mime = RasterMime(bytes);
    if (mime.empty()) mime = SvgMime(bytes);
    const bool image = !mime.empty();
    ServeFile(response, std::move(bytes), image ? mime : kOctetStreamMime,
              target->filename().string(),
              image && !request.has_param("download"));
    return;
  }
  if (!LikelyTextFile(*target)) {
    Error(response, "not a text file", 415);
    return;
  }
  const bool more = bytes.size() > kFileViewBytes;
  Reply(response, {{"text", Utf8Prefix(std::move(bytes), kFileViewBytes)},
                   {"bytes", size},
                   {"more", more}});
}

}  // namespace uagent::web::master
