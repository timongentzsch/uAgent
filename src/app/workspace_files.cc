// Copyright 2026 Timon Gentzsch

#include "include/app/workspace_files.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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
constexpr char kXlsxMime[] =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
// Pages one request may move: more than any deck holds.
constexpr size_t kMoves = 1000;

bool Shown(const fs::path& path) {
  return !HiddenPath(path.string()) && !SelfConfigurationPath(path.string());
}

// In a repository, the files git tracks or would add, as the agent's grep
// sees them; elsewhere a walk that enters no .git, hidden or symlinked folder.
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

// The start of an HTML page, where its meta tags are, as they are read:
// without its comments, and quoted one way.
std::string Head(const fs::path& page) {
  std::string head = ReadFile(page.string(), KiB(4)).value_or("");
  for (size_t at = 0; (at = head.find("<!--", at)) != std::string::npos;) {
    const size_t end = head.find("-->", at);
    head.erase(at, end == std::string::npos ? end : end - at + 3);
  }
  std::ranges::replace(head, '\'', '"');
  return head;
}

// What an HTML page's head says it is: a "sheet" of tables, a "doc" on paper
// (print flow), a "slide", a whiteboard's "board", a "page" with an
// artboard, or nothing.
std::string PageKind(const std::string& head) {
  if (!MetaTag(head, "sheet").empty()) return "sheet";
  if (MetaTag(head, "print").find("\"flow") != std::string::npos) return "doc";
  const std::string artboard = MetaTag(head, "artboard");
  if (artboard.empty()) return "";
  if (artboard.find(" slide\"") != std::string::npos) return "slide";
  return MetaTag(head, "board").empty() ? "page" : "board";
}

// Whether a page of `kind` is an artifact of its own, one file, in a folder
// that is `folder`: a sheet anywhere, a document but a design system's,
// which is its book. Neither makes its folder what it is.
bool Alone(const std::string& kind, const std::string& folder = "") {
  return kind == "sheet" || (kind == "doc" && folder != "system");
}

// The content of the meta tag named `name`, as written; empty when none.
std::string MetaContent(const std::string& head, const std::string& name) {
  const std::string tag = MetaTag(head, name);
  const size_t at = tag.find("content=\"");
  return at == std::string::npos
             ? std::string()
             : tag.substr(at + 9, tag.find('"', at + 9) - at - 9);
}

// The size a page's artboard names, "1440x900" or a width alone: {w, h},
// with 0 for what it leaves open.
json ArtboardSize(const std::string& head) {
  const std::string said = MetaContent(head, "artboard");
  int64_t size[2] = {0, 0};
  size_t digit = 0;
  for (int64_t& side : size) {
    while (digit < said.size() &&
           isdigit(static_cast<unsigned char>(said[digit])) && side < 100000) {
      side = side * 10 + (said[digit++] - '0');
    }
    if (digit >= said.size() || said[digit] != 'x') break;
    ++digit;
  }
  return size[0] > 0 ? json::array({size[0], size[1]}) : json(nullptr);
}

// Whether a slide's head leaves it out of what is presented and printed.
bool Skipped(const std::string& head) {
  std::istringstream words(MetaContent(head, "slide"));
  for (std::string word; words >> word;) {
    if (word == "skip") return true;
  }
  return false;
}

// How many tabs a sheet has: its tables.
int64_t SheetTabs(const fs::path& page) {
  const std::string text = ReadFile(page.string(), kFileViewBytes).value_or("");
  int64_t tabs = 0;
  for (size_t at = 0; (at = text.find("<table data-sheet", at)) != text.npos;
       ++at) {
    ++tabs;
  }
  return tabs;
}

// What a folder's files make it: a design "system" with a tokens.css, a
// "deck" when every page not alone is a slide, a whiteboard's "board" when
// every one is a board, a "design" when a page declares an artboard, or
// nothing.
struct FolderPages {
  size_t pages = 0, slides = 0, boards = 0;
  bool artboard = false, system = false;
  void Add(const std::string& kind) {
    if (Alone(kind)) return;
    ++pages;
    slides += kind == "slide";
    boards += kind == "board";
    artboard = artboard || !kind.empty();
  }
  const char* Kind() const {
    return system            ? "system"
           : !artboard       ? ""
           : slides == pages ? "deck"
           : boards == pages ? "board"
                             : "design";
  }
};
}  // namespace

// The artifacts in `root` (see the artifact skill): decks, designs and
// whiteboards, a folder each; documents and sheets, each its own; and design
// systems, whose own pages are theirs. Each names up to four of its pages,
// for thumbnails.
json Designs(const fs::path& root) {
  struct Folder {
    // Each page's path and kind.
    std::vector<std::pair<std::string, std::string>> pages;
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
    if ((!tokens && relative.extension() != ".html") || !Resolve(root, path)) {
      continue;
    }
    Folder& folder = folders[relative.parent_path().string()];
    folder.modified = std::max(folder.modified, modified(relative));
    if (tokens) {
      folder.kind.system = true;
      continue;
    }
    const std::string kind = PageKind(Head(root / relative));
    folder.pages.emplace_back(path, kind);
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
    // Only ancestors can own a component. Unrelated folders never enter the
    // search, even when a workspace holds thousands of standalone artifacts.
    bool inside = false;
    for (fs::path parent = dir; !parent.empty() && !inside;) {
      parent = parent.parent_path();
      const auto owner = folders.find(parent.string());
      inside = owner != folders.end() && owner->second.kind.system;
    }
    const std::string type = folder.kind.Kind();
    std::vector<std::string> pages;
    for (const auto& [path, kind] : folder.pages) {
      if (!Alone(kind, type)) {
        pages.push_back(path);
        continue;
      }
      // A system's book and component previews are its own; a sheet is
      // its own wherever it lies, counted by its tabs.
      if (inside && kind != "sheet") continue;
      Found one = item(kind, path, {path}, modified(path));
      one.item["alone"] = true;
      found.push_back(std::move(one));
    }
    if (type == "system" || (!type.empty() && !inside)) {
      found.push_back(item(type, dir, std::move(pages), folder.modified));
    }
  }
  std::ranges::sort(found, [](const Found& a, const Found& b) {
    return a.modified > b.modified;
  });
  if (found.size() > kPlaceDesigns) found.resize(kPlaceDesigns);
  json items = json::array();
  for (Found& entry : found) {
    // Counting tabs opens the workbook. Read only sheets the Library returns.
    if (entry.item["kind"] == "sheet") {
      entry.item["count"] = SheetTabs(root / JsonValue(entry.item, "path", ""));
    }
    items.push_back(std::move(entry.item));
  }
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

// What tells one state of a file from the next without reading it: its size,
// inode and modification time.
std::string Stamp(const fs::path& file) {
  const FileStamp stamp = SnapshotFile(file.string());
  return std::to_string(stamp.size) + ":" + std::to_string(stamp.inode) + ":" +
         std::to_string(stamp.modified_seconds) + "." +
         std::to_string(stamp.modified_nanoseconds);
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
  std::string code;  // Git's index and working-tree status columns.
  std::string previous;

  char State(std::string_view scope = {}) const {
    if (scope.empty()) {
      if (code.find('D') != std::string::npos) return 'D';
      if (code.find('R') != std::string::npos) return 'R';
      return code.find_first_of("A?") == std::string::npos ? 'M' : 'A';
    }
    // An unresolved merge has no single staged version.
    if (code.find('U') != std::string::npos || code == "DD" || code == "AA") {
      return scope == "working" ? 'U' : ' ';
    }
    if (code == "??") return scope == "working" ? 'A' : ' ';
    return code[scope == "staged" ? 0 : 1];
  }
};
struct Changes {
  bool repository = false;
  std::vector<Change> list;
  bool more = false;
};
Changes ReadChanges(const fs::path& folder, const std::string& pathspec,
                    const std::string& rename_target = "") {
  Changes changes;
  const CapturedProcess prefix =
      HostGit(folder.string(), {"rev-parse", "--show-prefix"});
  if (!prefix.Ok()) return changes;
  const std::string from_top =
      prefix.output.substr(0, prefix.output.rfind('\n'));
  const CapturedProcess status = HostGit(
      folder.string(),
      {"--no-optional-locks", "status", "--porcelain", "-z", "--find-renames",
       rename_target.empty() ? "--untracked-files=all" : "--untracked-files=no",
       "--", pathspec});
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
    const std::string name = out.substr(at + 3, end - at - 3);
    at = end + 1;
    std::string previous;
    if (code.find_first_of("RC") != std::string::npos) {
      const size_t next = out.find('\0', at);
      if (next == std::string::npos) break;
      const std::string old = out.substr(at, next - at);
      if (!rename_target.empty() && name == from_top + rename_target &&
          old.starts_with(from_top) &&
          Shown(folder / old.substr(from_top.size()))) {
        previous = old.substr(from_top.size());
      }
      at = next + 1;
    }
    if (!name.starts_with(from_top)) continue;
    std::string path = name.substr(from_top.size());
    if ((!rename_target.empty() && rename_target != path) ||
        !Shown(folder / path)) {
      continue;
    }
    if (changes.list.size() == kFileFindMatches) {
      changes.more = true;
      break;
    }
    changes.list.push_back({std::move(path), code, std::move(previous)});
    if (!rename_target.empty()) break;
  }
  return changes;
}

// A path's diff against HEAD (default), against the index (working), or
// between HEAD and the index (staged, including before the first commit).
std::optional<json> ReadDiff(const fs::path& folder, const std::string& path,
                             const std::string& scope) {
  const Changes changes = ReadChanges(folder, ":(literal)" + path);
  const auto found = std::ranges::find_if(changes.list, [&](const Change& c) {
    return c.path == path && c.State(scope) != ' ';
  });
  if (found == changes.list.end()) return std::nullopt;
  std::vector<std::string> args = {"--no-optional-locks", "diff",
                                   "--no-ext-diff", "--no-textconv",
                                   "--no-color"};
  const bool whole =
      found->code == "??" ||
      (scope.empty() &&
       !HostGit(folder.string(), {"rev-parse", "-q", "--verify", "HEAD"}).Ok());
  if (whole) {
    args.insert(args.end(), {"--no-index", "--", "/dev/null", path});
  } else {
    args.emplace_back("--relative");
    if (scope == "staged") args.emplace_back("--cached");
    if (scope.empty()) args.emplace_back("HEAD");
    args.insert(args.end(), {"--find-renames", "--", ":(literal)" + path});
    // Restricting status to the destination reports a rename as an addition.
    // Ask for the tracked paths only when that column could be a rename.
    if (!scope.empty() && found->State(scope) == 'A') {
      const Changes renamed = ReadChanges(folder, ".", path);
      for (const auto& c : renamed.list) {
        if (c.State(scope) == 'R' && !c.previous.empty()) {
          args.push_back(":(literal)" + c.previous);
        }
      }
    }
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
                                     {".csv", "text/csv"},
                                     {".xlsx", kXlsxMime},
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

// A folder's pages printed in name order, each at its own artboard's size
// (the asked size where it names none): a deck's slides, or a design's
// pages. Documents and sheets are artifacts of their own, and a skipped
// slide is left out.
std::string DeckPage(const fs::path& root, const fs::path& folder, int width,
                     int height, bool review) {
  const auto px = [](int64_t n) { return std::to_string(n) + "px"; };
  // Each page by its file's name, and the named @page of its size.
  std::vector<std::pair<std::string, std::string>> pages;
  std::map<std::string, std::string> sizes;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(folder, error)) {
    if (!entry.is_regular_file(error) || entry.path().extension() != ".html") {
      continue;
    }
    // Only include a frame the workspace's preview route can actually serve.
    const auto target =
        Resolve(root, entry.path().lexically_relative(root).string());
    if (!target) continue;
    const std::string head = Head(*target);
    if (Alone(PageKind(head)) || Skipped(head)) continue;
    const json size = ArtboardSize(head);
    const auto side = [&](size_t at, int asked) {
      const int64_t said = size.is_array() ? size[at].get<int64_t>() : 0;
      return said > 0 ? std::min<int64_t>(said, 8000) : asked;
    };
    const int64_t w = side(0, width), h = side(1, height);
    const std::string name = "p" + std::to_string(w) + "x" + std::to_string(h);
    sizes[name] = "{size:" + px(w) + " " + px(h) + ";margin:0}." + name +
                  "{page:" + name + ";width:" + px(w) + ";height:" + px(h) +
                  "}";
    pages.emplace_back(entry.path().filename().string(), name);
  }
  std::ranges::sort(pages);
  std::string page =
      "<!doctype html><meta charset=\"utf-8\"><style>@page{size:" + px(width) +
      " " + px(height) +
      ";margin:0}html,body{margin:0}iframe{display:block;"
      "border:0;break-after:page}";
  for (const auto& [name, rules] : sizes) page += "@page " + name + rules;
  page += "</style>";
  // In a review Chrome instantiates just the slides in the requested range.
  // Printing instantiates the same ordered frames, all at once.
  if (review) page += "<template id=slides>";
  for (const auto& [file, name] : pages) {
    page += "<iframe class=\"" + name + "\" src=\"" + PercentEncode(file) +
            "\"></iframe>";
  }
  if (review) page += "</template>";
  return page;
}

// A person's reordering of a deck: pages of one folder renamed, copied
// (`copy`) or deleted (an empty `to`). Everything is checked first and each
// page passes through a temporary name, so two can swap.
json Move(const fs::path& folder, const json& moves) {
  const auto page = [](const std::string& name) {
    return name.ends_with(".html") && name.front() != '.' &&
           name.find_first_of(std::string_view("/\\\0", 3)) ==
               std::string::npos;
  };
  json invalid = {{"error", "invalid request"}, {"status", 400}};
  if (!moves.is_array() || moves.empty() || moves.size() > kMoves) {
    return invalid;
  }
  std::set<std::string> leaving, arriving;
  std::error_code error;
  for (const json& move : moves) {
    const std::string from = JsonValue(move, "from", ""),
                      to = JsonValue(move, "to", "");
    const bool copy = JsonValue(move, "copy", false);
    if (!page(from) || (to.empty() ? copy : !page(to)) ||
        (!copy && !leaving.insert(from).second) ||
        (!to.empty() && !arriving.insert(to).second)) {
      return invalid;
    }
    if (!fs::is_regular_file(fs::symlink_status(folder / from, error))) {
      return {{"error", "not found"}, {"status", 404}};
    }
  }
  for (const std::string& name : arriving) {
    if (!leaving.contains(name) &&
        fs::exists(fs::symlink_status(folder / name, error))) {
      return {{"error", name + " is already there"}, {"status", 409}};
    }
  }
  const std::string stem = ".move-" + RandomToken(kNameBytes) + "-";
  const auto staged = [&](size_t index) {
    return folder / (stem + std::to_string(index));
  };
  error.clear();
  // Copies first: their page may be renamed by the same request.
  for (const bool copies : {true, false}) {
    for (size_t i = 0; i < moves.size() && !error; ++i) {
      if (JsonValue(moves[i], "copy", false) != copies) continue;
      const fs::path from = folder / JsonValue(moves[i], "from", "");
      if (copies) {
        fs::copy_file(from, staged(i), error);
      } else {
        fs::rename(from, staged(i), error);
      }
    }
  }
  for (size_t i = 0; i < moves.size(); ++i) {
    const std::string to = JsonValue(moves[i], "to", "");
    std::error_code step;
    if (error && !JsonValue(moves[i], "copy", false)) {
      // Nothing has its new name yet: what was taken goes back.
      fs::rename(staged(i), folder / JsonValue(moves[i], "from", ""), step);
    } else if (error || to.empty()) {
      fs::remove(staged(i), step);
    } else {
      fs::rename(staged(i), folder / to, step);
      if (step) return {{"error", step.message()}, {"status", 500}};
    }
  }
  if (error) return {{"error", error.message()}, {"status", 500}};
  return {{"moved", moves.size()}};
}

}  // namespace
json Request(const fs::path& root, const json& request) {
  const std::string mode = JsonValue(request, "mode", "");
  const std::string path = JsonValue(request, "path", "");
  if (mode == "designs") return {{"items", Designs(root)}};
  if (mode == "delete_design") {
    const std::string kind = JsonValue(request, "kind", "");
    const std::string key = JsonValue(request, "key", "");
    for (const json& item : Designs(root)) {
      if (item["kind"] != kind || item["key"] != key) continue;
      const std::string relative = JsonValue(item, "path", "");
      const auto target = Resolve(root, relative);
      // A root design is the workspace itself and cannot be deleted.
      if (!target || *target == root || relative.empty()) break;
      std::error_code error;
      fs::remove_all(root / relative, error);
      return error ? json{{"error", "cannot delete: " + error.message()},
                          {"status", 500}}
                   : json::object();
    }
    return {{"error", "not found"}, {"status", 404}};
  }
  if (mode == "find") return Find(root, path);
  if (mode == "changes" || mode == "diff") {
    if (mode == "diff") {
      const std::string scope = JsonValue(request, "scope", "");
      if (!scope.empty() && scope != "working" && scope != "staged") {
        return {{"error", "unknown diff scope"}, {"status", 400}};
      }
      return ReadDiff(root, path, scope)
          .value_or(json{{"error", "not found"}, {"status", 404}});
    }
    const Changes changes = ReadChanges(root, ".");
    json rows = json::array();
    for (const auto& change : changes.list) {
      json row = {{"path", change.path},
                  {"state", std::string(1, change.State())}};
      for (const std::string scope : {"working", "staged"}) {
        const char state = change.State(scope);
        if (state != ' ') row[scope] = std::string(1, state);
      }
      rows.push_back(std::move(row));
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
    // What the folder is and what each page is shown as, so a viewer knows
    // a deck from a design and a document from a page: a document or a sheet
    // (an Excel or CSV file too) as itself, any other as its folder, or a
    // design.
    FolderPages folder;
    std::vector<std::pair<json*, std::string>> kinds;
    for (json& entry : (*listed)["entries"]) {
      const fs::path name(JsonValue(entry, "name", ""));
      if (JsonValue(entry, "dir", false)) continue;
      folder.system = folder.system || name == "tokens.css";
      const std::string ext = AsciiLower(name.extension().string());
      if (ext == ".xlsx" || ext == ".csv") kinds.emplace_back(&entry, "sheet");
      if (name.extension() != ".html") continue;
      const std::string head = Head(*target / name);
      const std::string kind = PageKind(head);
      kinds.emplace_back(&entry, kind);
      folder.Add(kind);
      // Its size, so a viewer gives it its room before it has drawn.
      if (json size = ArtboardSize(head); !size.is_null()) {
        entry["artboard"] = std::move(size);
      }
      if (Skipped(head)) entry["skip"] = true;
      // A slide that opens a part of a deck names it.
      if (std::string section = MetaContent(head, "section");
          !section.empty()) {
        entry["section"] = Utf8Prefix(std::move(section), 80);
      }
      if (kind == "sheet") entry["count"] = SheetTabs(*target / name);
    }
    const std::string type = folder.Kind();
    for (auto& [entry, kind] : kinds) {
      (*entry)["kind"] = kind == "doc" || kind == "sheet" ? kind
                         : type.empty()                   ? "design"
                                                          : type;
      if (Alone(kind, type)) (*entry)["alone"] = true;
    }
    (*listed)["kind"] = type;
    return *listed;
  }
  // A folder's files by name, each with its stamp: what a viewer asks often
  // to see what changed, so nothing in them is read.
  if (mode == "stamps") {
    auto listed = List(root, *target);
    if (!listed) return {{"error", "not found"}, {"status", 404}};
    json stamps = json::object();
    for (const json& entry : (*listed)["entries"]) {
      if (JsonValue(entry, "dir", false)) continue;
      const std::string name = JsonValue(entry, "name", "");
      stamps[name] = Stamp(*target / name);
    }
    return {{"stamps", std::move(stamps)}};
  }
  if (mode == "deck") {
    std::error_code error;
    if (!fs::is_directory(*target, error)) {
      return {{"error", "not found"}, {"status", 404}};
    }
    return {{"text", DeckPage(root, *target, JsonValue(request, "width", 1920),
                              JsonValue(request, "height", 1080),
                              JsonValue(request, "review", false))}};
  }
  if (mode == "stat") {
    std::error_code error;
    return {{"directory", fs::is_directory(*target, error)},
            {"regular", fs::is_regular_file(*target, error)}};
  }
  if (mode == "move") {
    std::error_code error;
    if (!fs::is_directory(*target, error)) {
      return {{"error", "not found"}, {"status", 404}};
    }
    return Move(*target, request.value("moves", json()));
  }
  if (mode == "write") {
    // With `create`, a page that is not there yet (a sheet saved from an
    // Excel file); else an edit of a text file that is.
    const bool create = JsonValue(request, "create", false);
    if (create ? target->extension() != ".html"
               : !LikelyTextFile(*target) || !fs::is_regular_file(*target)) {
      return {{"error", "not found"}, {"status", 404}};
    }
    const std::string text = JsonValue(request, "text", "");
    if (text.size() > kFileViewBytes) {
      return {{"error", "too large to save here"}, {"status", 413}};
    }
    std::string previous, error;
    bool exists = false;
    const FolderLock lock(*target);
    json refused = create ? json()
                          : CheckRevision(request, *target, kFileViewBytes,
                                          "file", previous, exists);
    if (!refused.is_null()) {
      refused["status"] = refused.contains("conflict") ? 409 : 500;
      return refused;
    }
    // A new file is linked into place, which a file already there refuses.
    if (!AtomicWriteFile(target->string(), text, kSharedFileMode, true, error,
                         !create)) {
      std::error_code ec;
      if (create && fs::exists(fs::symlink_status(*target, ec))) {
        return {{"error", target->filename().string() + " is already there"},
                {"status", 409}};
      }
      return {{"error", error}, {"status", 500}};
    }
    return {{"revision", DocumentRevision(target->string(), text)},
            {"stamp", Stamp(*target)}};
  }
  std::string bytes, error;
  const bool read = mode == "read";
  if (!ReadRegularFile(target->string(),
                       read ? kFileViewBytes + 1 : session::kTransferBytes,
                       bytes, error, read)) {
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
