// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/app/asset_store.h"
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

// The meta tag named `name` in a page's head, as written; empty when none.
std::string MetaTag(const std::string& head, const std::string& name) {
  const size_t at = head.find("name=\"" + name + "\"");
  const size_t open = at == std::string::npos ? at : head.rfind('<', at);
  const size_t close = at == std::string::npos ? at : head.find('>', at);
  return open == std::string::npos || close == std::string::npos
             ? std::string()
             : head.substr(open, close - open);
}

// The designs in `root` (see the design skill): folders with a page that
// declares an artboard, a deck when every page is a slide; documents, each
// page on paper (print flow) its own; and design systems, folders with a
// tokens.css, whose own pages are theirs. Each names up to four of its
// pages, for thumbnails.
void AddDesigns(const fs::path& root, json& items) {
  struct Folder {
    std::vector<std::string> pages, docs;
    int64_t modified = 0;
    bool artboard = false, system = false;
    size_t slides = 0;
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
    const std::string head =
        ReadFile((root / relative).string(), KiB(4)).value_or("");
    if (MetaTag(head, "print").find("\"flow") != std::string::npos) {
      folder.docs.push_back(path);
      continue;
    }
    const std::string artboard = MetaTag(head, "artboard");
    folder.pages.push_back(path);
    folder.artboard = folder.artboard || !artboard.empty();
    folder.slides += artboard.find(" slide\"") != std::string::npos;
  }
  struct Found {
    int64_t modified;
    json item;
  };
  std::vector<Found> found;
  const auto item = [&](const char* kind, const std::string& dir,
                        std::vector<std::string> pages, int64_t modified) {
    std::ranges::sort(pages);
    const size_t count = pages.size();
    pages.resize(std::min<size_t>(count, 4));
    return Found{modified,
                 {{"kind", kind},
                  {"key", root.string() + "/" + dir},
                  {"name", dir.empty() ? root.filename().string() : dir},
                  {"path", dir},
                  {"place", root.string()},
                  {"count", count},
                  {"pages", std::move(pages)},
                  {"modified", modified}}};
  };
  for (auto& [dir, folder] : folders) {
    // A system's component previews and book are its own, not designs.
    const bool inside = std::ranges::any_of(folders, [&](const auto& other) {
      return other.second.system && other.first != dir &&
             (other.first.empty() || dir.starts_with(other.first + "/"));
    });
    if (folder.system) {
      folder.pages.insert(folder.pages.end(), folder.docs.begin(),
                          folder.docs.end());
      found.push_back(item("system", dir, folder.pages, folder.modified));
      continue;
    }
    if (inside) continue;
    if (folder.artboard) {
      const bool deck = folder.slides == folder.pages.size();
      found.push_back(
          item(deck ? "deck" : "design", dir, folder.pages, folder.modified));
    }
    for (const std::string& doc : folder.docs) {
      found.push_back(item("doc", doc, {doc}, Modified(root / doc)));
    }
  }
  std::ranges::sort(found, [](const Found& a, const Found& b) {
    return a.modified > b.modified;
  });
  if (found.size() > kPlaceDesigns) found.resize(kPlaceDesigns);
  for (Found& entry : found) items.push_back(std::move(entry.item));
}

// The files the agent shared in the conversation at `session_path`.
void AddShared(const std::string& session, const std::string& session_path,
               const std::string& place, json& items) {
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(session_path + kSessionAssetsSuffix, ec)) {
    if (entry.path().extension() != ".json") continue;
    const json record =
        ReadJsonFile(entry.path().string(), session::kAssetRecordBytes);
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
