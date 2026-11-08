// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/private_store.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace {

namespace fs = std::filesystem;

// At most this many designs a place lists, the newest.
constexpr size_t kPlaceDesigns = 50;

int64_t Modified(const fs::path& path) {
  return SnapshotFile(path.string()).modified_seconds * 1000;
}

// The designs in `root`: folders with a page that declares an artboard (see
// the design skill), and design systems, folders with a tokens.css. Each
// names up to four of its pages, for thumbnails.
void AddDesigns(const fs::path& root, json& items) {
  struct Folder {
    std::vector<std::string> pages;
    int64_t modified = 0;
    bool artboard = false, system = false;
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
    folder.modified = std::max(folder.modified, Modified(root / relative));
    if (tokens) {
      folder.system = true;
      continue;
    }
    folder.pages.push_back(path);
    if (!folder.artboard) {
      const auto head = ReadFile((root / relative).string(), KiB(4));
      folder.artboard =
          head && head->find("name=\"artboard\"") != std::string::npos;
    }
  }
  std::vector<std::pair<std::string, Folder>> found;
  for (auto& [dir, folder] : folders) {
    // A system's component previews are its own, not designs.
    const bool inside = std::ranges::any_of(folders, [&](const auto& other) {
      return other.second.system && other.first != dir &&
             (other.first.empty() || dir.starts_with(other.first + "/"));
    });
    if (folder.system || (folder.artboard && !inside)) {
      found.emplace_back(dir, std::move(folder));
    }
  }
  std::ranges::sort(found, [](const auto& a, const auto& b) {
    return a.second.modified > b.second.modified;
  });
  if (found.size() > kPlaceDesigns) found.resize(kPlaceDesigns);
  for (auto& [dir, folder] : found) {
    std::ranges::sort(folder.pages);
    const size_t count = folder.pages.size();
    folder.pages.resize(std::min<size_t>(count, 4));
    items.push_back({{"kind", folder.system ? "system" : "design"},
                     {"key", root.string() + "/" + dir},
                     {"name", dir.empty() ? root.filename().string() : dir},
                     {"path", dir},
                     {"place", root.string()},
                     {"count", count},
                     {"pages", folder.pages},
                     {"modified", folder.modified}});
  }
}

// The files the agent shared in the conversation at `session_path`.
void AddShared(const std::string& session, const std::string& session_path,
               const std::string& place, json& items) {
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(session_path + kSessionAssetsSuffix, ec)) {
    if (entry.path().extension() != ".json") continue;
    const json record = ReadJsonFile(entry.path().string(), 1024);
    if (!JsonValue(record, "shared", false)) continue;
    items.push_back({{"kind", "file"},
                     {"key", entry.path().stem().string()},
                     {"name", JsonValue(record, "name", "file")},
                     {"bytes", JsonValue(record, "bytes", int64_t{0})},
                     {"session", session},
                     {"place", place},
                     {"modified", Modified(entry.path())}});
  }
}

}  // namespace

// Everything the Library lists, from every place a conversation works in
// and every workspace: memories and skills (from one control process),
// designs and design systems in the folders, and files shared in
// conversations. Each item names its place; "" is every place's.
void Master::Library(const Request&, Response& response) {
  const json catalogue = host_.Catalogue();
  std::map<std::string, json> places;
  for (const json& space : catalogue["spaces"]) {
    const std::string path = JsonValue(space, "path", "");
    places[path] = {{"path", path},
                    {"name", JsonValue(space, "name", "")},
                    {"workspace", true}};
  }
  json items = json::array();
  std::map<std::string, int64_t> latest;
  for (const json& session : catalogue["sessions"]) {
    const std::string cwd = JsonValue(session, "cwd", "");
    const std::string id = JsonValue(session, "id", "");
    if (cwd.empty()) continue;
    json& place = places[cwd];
    if (place.is_null()) {
      place = {{"path", cwd},
               {"name", fs::path(cwd).filename().string()},
               {"workspace", false}};
    }
    // The conversation a place's designs open in: its latest of its own.
    const int64_t updated = JsonValue(session, "updated", int64_t{0});
    if (JsonValue(session, "parent", "").empty() && updated >= latest[cwd]) {
      latest[cwd] = updated;
      place["session"] = id;
    }
    AddShared(id, host_.AssetPath(id), cwd, items);
  }
  json paths = json::array(), listed = json::array();
  for (auto& [path, place] : places) {
    paths.push_back(path);
    listed.push_back(std::move(place));
    AddDesigns(path, items);
  }
  json library = host_.Manage(
      {{"kind", "library"}, {"action", "list"}, {"places", paths}});
  if (library.contains("error")) {
    Error(response, JsonValue(library, "error", ""), 500);
    return;
  }
  for (json& item : library["items"]) items.push_back(std::move(item));
  Reply(response, {{"places", std::move(listed)},
                   {"items", std::move(items)},
                   {"limits", std::move(library["limits"])}});
}

}  // namespace uagent::web::master
