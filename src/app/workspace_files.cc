// Copyright 2026 Timon Gentzsch

#include "include/app/workspace_files.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/app/asset_store.h"
#include "include/app/launch.h"
#include "include/core/capture.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/library.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent::workspace_files {
namespace fs = std::filesystem;
namespace {
bool Shown(const fs::path& path) {
  return !HiddenPath(path.string()) && !SelfConfigurationPath(path.string());
}

// In a repository, the files git tracks or would add, so dependencies and
// build output stay out as they do from the agent's grep; elsewhere the
// folder is walked, entering neither .git nor a hidden or symlinked folder.
std::vector<std::string> Candidates(const fs::path& root, bool& more) {
  std::vector<std::string> paths;
  const CapturedProcess listed =
      HostGit(root.string(), {"ls-files", "--cached", "--others",
                              "--exclude-standard", "-z", "--", "."});
  // Past the capture's limit, what arrived is still a list, cut. A folder
  // git lists nothing in (one it ignores, say) is walked instead.
  if (!listed.output.empty() && (listed.Ok() || !listed.error.empty())) {
    const std::string& out = listed.output;
    for (size_t at = 0, end; (end = out.find('\0', at)) != std::string::npos;
         at = end + 1) {
      paths.push_back(out.substr(at, end - at));
    }
    more = more || !listed.error.empty();
    return paths;
  }
  std::deque<fs::path> folders = {root};
  int64_t scanned = 0;
  while (!folders.empty()) {
    std::error_code ec;
    fs::directory_iterator iterator(folders.front(), ec), end;
    folders.pop_front();
    for (; !ec && iterator != end; iterator.increment(ec)) {
      if (++scanned > kListDirScanEntries) {
        more = true;
        return paths;
      }
      const fs::path& path = iterator->path();
      std::error_code type_error;
      const fs::file_status status = iterator->symlink_status(type_error);
      if (fs::is_directory(status)) {
        if (path.filename() != ".git" && Shown(path)) folders.push_back(path);
      } else if (fs::is_regular_file(status)) {
        paths.push_back(path.lexically_relative(root).string());
      }
    }
  }
  return paths;
}

}  // namespace

// `relative` resolved under `root`, when it stays inside and may be shown.
std::optional<fs::path> Resolve(const fs::path& root,
                                const std::string& relative) {
  if (fs::path(relative).is_absolute()) return std::nullopt;
  fs::path target = CanonicalAccessPath((root / relative).string());
  if (!PathWithin(target, root) || !Shown(target)) return std::nullopt;
  return target;
}

namespace {
// At most this many designs a place lists, the newest.
constexpr size_t kPlaceDesigns = 50;

// The meta tag named `name` in a page's head, as written; empty when none.
std::string MetaTag(const std::string& head, const std::string& name) {
  const size_t at = head.find("name=\"" + name + "\"");
  const size_t open = at == std::string::npos ? at : head.rfind('<', at);
  const size_t close = at == std::string::npos ? at : head.find('>', at);
  return open == std::string::npos || close == std::string::npos
             ? std::string()
             : head.substr(open, close - open);
}

// What an HTML page's head says it is: a "doc" on paper (print flow), a
// "slide", a "page" with an artboard, or nothing.
std::string PageKind(const fs::path& page) {
  const std::string head = ReadFile(page.string(), KiB(4)).value_or("");
  if (MetaTag(head, "print").find("\"flow") != std::string::npos) return "doc";
  const std::string artboard = MetaTag(head, "artboard");
  if (artboard.empty()) return "";
  return artboard.find(" slide\"") == std::string::npos ? "page" : "slide";
}

// What a folder's files make it: a design "system" with a tokens.css, a
// "deck" when every page that is not a document is a slide, a "design" when
// a page declares an artboard, or nothing.
struct FolderPages {
  size_t pages = 0, slides = 0;
  bool artboard = false, system = false;
  void Add(const std::string& kind) {
    ++pages;
    slides += kind == "slide";
    artboard = artboard || !kind.empty();
  }
  const char* Kind() const {
    return system            ? "system"
           : !artboard       ? ""
           : slides == pages ? "deck"
                             : "design";
  }
};
}  // namespace

// The designs in `root` (see the artifact skill): decks and designs, a
// folder each; documents, each its own; and design systems, whose own pages
// are theirs. Each names up to four of its pages, for thumbnails.
json Designs(const fs::path& root) {
  struct Folder {
    std::vector<std::string> pages, docs;
    int64_t modified = 0;
    FolderPages kind;
  };
  const auto modified = [&](const fs::path& relative) {
    return SnapshotFile((root / relative).string()).modified_seconds * 1000;
  };
  std::map<std::string, Folder> folders;
  bool more = false;
  for (const std::string& path : Candidates(root, more)) {
    const fs::path relative(path);
    const bool tokens = relative.filename() == "tokens.css";
    if ((!tokens && relative.extension() != ".html") ||
        !Shown(root / relative)) {
      continue;
    }
    Folder& folder = folders[relative.parent_path().string()];
    folder.modified = std::max(folder.modified, modified(relative));
    if (tokens) {
      folder.kind.system = true;
      continue;
    }
    const std::string kind = PageKind(root / relative);
    if (kind == "doc") {
      folder.docs.push_back(path);
      continue;
    }
    folder.pages.push_back(path);
    folder.kind.Add(kind);
  }
  struct Found {
    int64_t modified;
    json item;
  };
  std::vector<Found> found;
  const auto item = [&](const std::string& kind, const std::string& dir,
                        std::vector<std::string> pages, int64_t changed) {
    std::ranges::sort(pages);
    const size_t count = pages.size();
    pages.resize(std::min<size_t>(count, 4));
    return Found{changed,
                 {{"kind", kind},
                  {"key", root.string() + "/" + dir},
                  {"name", dir.empty() ? root.filename().string() : dir},
                  {"path", dir},
                  {"place", root.string()},
                  {"count", count},
                  {"pages", std::move(pages)},
                  {"modified", changed}}};
  };
  for (auto& [dir, folder] : folders) {
    // A system's component previews and book are its own, not designs.
    const bool inside = std::ranges::any_of(folders, [&](const auto& other) {
      return other.second.kind.system && other.first != dir &&
             (other.first.empty() || dir.starts_with(other.first + "/"));
    });
    if (folder.kind.system) {
      folder.pages.insert(folder.pages.end(), folder.docs.begin(),
                          folder.docs.end());
      found.push_back(item("system", dir, folder.pages, folder.modified));
      continue;
    }
    if (inside) continue;
    if (folder.kind.artboard) {
      found.push_back(
          item(folder.kind.Kind(), dir, folder.pages, folder.modified));
    }
    for (const std::string& doc : folder.docs) {
      found.push_back(item("doc", doc, {doc}, modified(doc)));
    }
  }
  std::ranges::sort(found, [](const Found& a, const Found& b) {
    return a.modified > b.modified;
  });
  if (found.size() > kPlaceDesigns) found.resize(kPlaceDesigns);
  json items = json::array();
  for (Found& entry : found) items.push_back(std::move(entry.item));
  return items;
}

namespace {
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
    if (rows.size() == static_cast<size_t>(ListDirEntries())) {
      more = true;
      break;
    }
    rows.push_back(
        {{"name", entry.name}, {"dir", entry.dir}, {"size", entry.size}});
  }
  return json{{"entries", std::move(rows)}, {"more", more}};
}

}  // namespace

json Folders(const std::string& path) {
  const auto folder = CanonicalDirectory(ExpandHome(path));
  if (!folder || !Shown(*folder)) {
    return {{"error", "Choose an accessible folder."}, {"status", 404}};
  }
  auto listed = List(folder->root_path(), *folder);
  if (!listed) return {{"error", "Cannot list this folder."}, {"status", 403}};
  json entries = json::array();
  for (const json& entry : (*listed)["entries"]) {
    if (JsonValue(entry, "dir", false)) entries.push_back(entry);
  }
  (*listed)["entries"] = std::move(entries);
  (*listed)["path"] = folder->string();
  (*listed)["parent"] = folder->parent_path().string();
  return *listed;
}

namespace {
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

// Files under `root` that match `query`, nearest first.
json Find(const fs::path& root, const std::string& query) {
  const std::string wanted = AsciiLower(query);
  bool more = false;
  std::vector<std::string> matched;
  for (std::string& path : Candidates(root, more)) {
    if (Matches(wanted, AsciiLower(path))) matched.push_back(std::move(path));
  }
  const auto depth = [](const std::string& path) {
    return std::ranges::count(path, '/');
  };
  std::ranges::stable_sort(matched,
                           [&](const std::string& a, const std::string& b) {
                             return depth(a) < depth(b);
                           });
  json paths = json::array();
  for (std::string& path : matched) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(root / path, ec)) ||
        !Shown(root / path)) {
      continue;
    }
    if (paths.size() == kFileFindMatches) {
      more = true;
      break;
    }
    paths.push_back(std::move(path));
  }
  return {{"paths", std::move(paths)}, {"more", more}};
}

struct Change {
  std::string path;
  char state;
  bool untracked;
};
struct Changes {
  bool repository = false;
  std::vector<Change> list;
  bool more = false;
};
Changes ReadChanges(const fs::path& folder, const std::string& pathspec) {
  Changes changes;
  const CapturedProcess prefix =
      HostGit(folder.string(), {"rev-parse", "--show-prefix"});
  if (!prefix.Ok()) return changes;
  const std::string from_top = Trim(prefix.output);
  const CapturedProcess status = HostGit(
      folder.string(),
      {"status", "--porcelain", "-z", "--untracked-files=all", "--", pathspec});
  // Past the capture's limit, what arrived is still a list, cut.
  if (!status.error.empty() ? status.output.empty() : !status.Ok()) {
    return changes;
  }
  changes.repository = true;
  changes.more = !status.error.empty();
  const std::string out =
      status.output.substr(0, status.output.rfind('\0') + 1);
  for (size_t at = 0; at + 3 < out.size();) {
    const size_t end = std::min(out.find('\0', at), out.size());
    const std::string code = out.substr(at, 2);
    std::string path =
        out.substr(at + 3 + from_top.size(), end - at - 3 - from_top.size());
    at = end + 1;
    if (code.find_first_of("RC") != std::string::npos) {
      at = std::min(out.find('\0', at), out.size()) + 1;
    }
    if (!Shown(folder / path)) continue;
    if (changes.list.size() == kFileFindMatches) {
      changes.more = true;
      break;
    }
    const char state = code.find('D') != std::string::npos             ? 'D'
                       : code.find('R') != std::string::npos           ? 'R'
                       : code.find_first_of("A?") != std::string::npos ? 'A'
                                                                       : 'M';
    changes.list.push_back({std::move(path), state, code == "??"});
  }
  return changes;
}

// The change to `path` as a unified diff against HEAD, or an untracked file
// against nothing. No program the repository names is run: git would take
// an external diff or a textconv filter from its config.
std::optional<json> ReadDiff(const fs::path& folder, const Changes& changes,
                             const std::string& path) {
  const auto found = std::ranges::find(changes.list, path, &Change::path);
  if (found == changes.list.end()) return std::nullopt;
  std::vector<std::string> args = {"diff", "--no-ext-diff", "--no-textconv",
                                   "--no-color"};
  // A repository without commits has no HEAD to compare with.
  const bool head =
      HostGit(folder.string(), {"rev-parse", "-q", "--verify", "HEAD"}).Ok();
  const bool whole = found->untracked || !head;
  if (whole) {
    args.insert(args.end(), {"--no-index", "--", "/dev/null", path});
  } else {
    args.insert(args.end(), {"--relative", "HEAD", "--", ":(literal)" + path});
  }
  CapturedProcess diff = HostGit(folder.string(), args);
  // With --no-index, exit code 1 means the files differ. Past the capture's
  // limit, what arrived is the diff's start.
  const bool more = !diff.error.empty();
  if (more ? diff.output.empty() : diff.status < 0 || diff.status > whole) {
    return std::nullopt;
  }
  return json{{"text", Utf8Prefix(std::move(diff.output), kFileViewBytes)},
              {"more", more}};
}

}  // namespace

// What a previewed page may load, by name: its pages, stylesheets, scripts,
// data, fonts and media. Anything else in the folder stays out of reach.
namespace {
std::string PreviewMime(const fs::path& path) {
  const std::string ext = AsciiLower(path.extension().string());
  for (const auto& [suffix, mime] : {std::pair{".html", "text/html"},
                                     {".htm", "text/html"},
                                     {".css", "text/css"},
                                     {".js", "text/javascript"},
                                     {".mjs", "text/javascript"},
                                     {".json", "application/json"},
                                     {".avif", "image/avif"},
                                     {".mp4", "video/mp4"},
                                     {".webm", "video/webm"},
                                     {".mov", "video/quicktime"},
                                     {".woff2", "font/woff2"},
                                     {".woff", "font/woff"},
                                     {".ttf", "font/ttf"},
                                     {".otf", "font/otf"}}) {
    if (ext == suffix) return mime;
  }
  const std::string mime = AttachmentMime(path.string());
  return mime.starts_with("image/") ? mime : std::string();
}

std::string DeckPage(const fs::path& folder, int width, int height) {
  std::vector<std::string> slides;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(folder, error)) {
    if (entry.is_regular_file(error) && entry.path().extension() == ".html") {
      slides.push_back(entry.path().filename().string());
    }
  }
  std::ranges::sort(slides);
  const std::string size =
      std::to_string(width) + "px " + std::to_string(height) + "px";
  std::string page =
      "<!doctype html><meta charset=\"utf-8\"><style>@page{size:" + size +
      ";margin:0}html,body{margin:0}iframe{display:block;border:0;width:" +
      std::to_string(width) + "px;height:" + std::to_string(height) +
      "px;break-after:page}</style>";
  for (const std::string& slide : slides) {
    page += "<iframe src=\"" + PercentEncode(slide) + "\"></iframe>";
  }
  return page;
}

}  // namespace
json Request(const fs::path& root, const json& request) {
  const std::string mode = JsonValue(request, "mode", "");
  const std::string path = JsonValue(request, "path", "");
  if (mode == "find") return Find(root, path);
  if (mode == "changes" || mode == "diff") {
    const Changes changes =
        ReadChanges(root, mode == "changes" ? "." : ":(literal)" + path);
    if (mode == "diff") {
      return ReadDiff(root, changes, path)
          .value_or(json{{"error", "not found"}, {"status", 404}});
    }
    json rows = json::array();
    for (const auto& change : changes.list) {
      rows.push_back(
          {{"path", change.path}, {"state", std::string(1, change.state)}});
    }
    return {{"changes", rows},
            {"more", changes.more},
            {"repository", changes.repository}};
  }
  const auto target = Resolve(root, path);
  if (!target) return {{"error", "not found"}, {"status", 404}};
  if (mode == "list") {
    auto listed = List(root, *target);
    if (!listed) return {{"error", "not found"}, {"status", 404}};
    // What the folder's pages say they are, so a viewer knows a deck from a
    // design and a document from a page.
    FolderPages folder;
    for (json& entry : (*listed)["entries"]) {
      const fs::path name(JsonValue(entry, "name", ""));
      if (JsonValue(entry, "dir", false)) continue;
      folder.system = folder.system || name == "tokens.css";
      if (name.extension() != ".html") continue;
      const std::string kind = PageKind(*target / name);
      if (!kind.empty()) entry["kind"] = kind;
      if (kind != "doc") folder.Add(kind);
    }
    (*listed)["kind"] = folder.Kind();
    return *listed;
  }
  if (mode == "deck") {
    std::error_code error;
    if (!fs::is_directory(*target, error)) {
      return {{"error", "not found"}, {"status", 404}};
    }
    return {{"text", DeckPage(*target, JsonValue(request, "width", 1920),
                              JsonValue(request, "height", 1080))}};
  }
  if (mode == "stat") {
    std::error_code error;
    return {{"directory", fs::is_directory(*target, error)},
            {"regular", fs::is_regular_file(*target, error)}};
  }
  if (mode == "write") {
    if (!LikelyTextFile(*target) || !fs::is_regular_file(*target)) {
      return {{"error", "not found"}, {"status", 404}};
    }
    const std::string text = JsonValue(request, "text", "");
    if (text.size() > kFileViewBytes) {
      return {{"error", "too large to save here"}, {"status", 413}};
    }
    std::string previous, error;
    bool exists = false;
    json refused = CheckRevision(request, *target, kFileViewBytes, "file",
                                 previous, exists);
    if (!refused.is_null()) {
      refused["status"] = refused.contains("conflict") ? 409 : 500;
      return refused;
    }
    if (!AtomicWriteFile(target->string(), text, 0644, true, error)) {
      return {{"error", error}, {"status", 500}};
    }
    return {{"revision", DocumentRevision(target->string(), text)}};
  }
  std::string bytes, error;
  const bool read = mode == "read";
  if (!ReadRegularFile(target->string(),
                       read ? kFileViewBytes + 1 : session::kUploadBytes, bytes,
                       error, read)) {
    return {{"error", "not found"}, {"status", 404}};
  }
  if (read) {
    if (!LikelyTextFile(*target)) {
      return {{"error", "not a text file"}, {"status", 415}};
    }
    const bool more = bytes.size() > kFileViewBytes;
    std::error_code ec;
    return {
        {"text", Utf8Prefix(bytes, kFileViewBytes)},
        {"bytes", fs::file_size(*target, ec)},
        {"more", more},
        {"revision", more ? "" : DocumentRevision(target->string(), bytes)}};
  }
  if (mode != "raw" && mode != "preview") {
    return {{"error", "unknown file operation"}};
  }
  const std::string mime = mode == "preview" ? PreviewMime(*target)
                                             : AttachmentMime(target->string());
  if (mode == "preview" && mime.empty()) {
    return {{"error", "not found"}, {"status", 404}};
  }
  return {{"data", HexEncode(bytes)},
          {"mime", mime},
          {"name", target->filename().string()}};
}
}  // namespace uagent::workspace_files
