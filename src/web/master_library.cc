// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/app/asset_store.h"
#include "include/app/workspace_files.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/private_store.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace {

namespace fs = std::filesystem;

int64_t Modified(const fs::path& path) {
  return SnapshotFile(path.string()).modified_seconds * 1000;
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

// Everything the Library lists, from every place a conversation works in and
// every workspace. Each item names its place; "" is every place's.
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
    if (!JsonValue(session, "host", "").empty()) {
      AddShared(id, host_.AssetPath(id), cwd, items);
      continue;
    }
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
    for (json& design : workspace_files::Designs(path)) {
      items.push_back(std::move(design));
    }
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

// Deletes one artifact the Library lists. Only what the place's listing
// names now goes, and never a place itself.
void Master::LibraryDelete(const Request& request, Response& response) {
  const json body = json::parse(request.body, nullptr, false);
  const std::string kind = JsonValue(body, "kind", "");
  const std::string key = JsonValue(body, "key", "");
  if (kind == "file") {
    const std::string error =
        host_.DeleteShared(JsonValue(body, "session", ""), key);
    if (error.empty()) {
      Reply(response, json::object());
    } else {
      Error(response, error, error == "not found" ? 404 : 500);
    }
    return;
  }
  const std::string place = JsonValue(body, "place", "");
  const json catalogue = host_.Catalogue();
  const auto names = [&](const char* list, const char* field) {
    return std::ranges::any_of(catalogue[list], [&](const json& each) {
      return JsonValue(each, "host", "").empty() &&
             JsonValue(each, field, "") == place;
    });
  };
  json listed = json::array();
  if (!place.empty() && (names("spaces", "path") || names("sessions", "cwd"))) {
    listed = workspace_files::Designs(place);
  }
  const auto found = std::ranges::find_if(listed, [&](const json& item) {
    return JsonValue(item, "kind", "") == kind &&
           JsonValue(item, "key", "") == key;
  });
  // A design at a place's root is the place: it stays.
  const std::string path =
      found == listed.end() ? "" : JsonValue(*found, "path", "");
  if (path.empty()) {
    Error(response, "not found", 404);
    return;
  }
  std::error_code ec;
  // A document and a sheet are one file; the others are their folder.
  if (kind == "doc" || kind == "sheet") {
    fs::remove(fs::path(place) / path, ec);
  } else {
    fs::remove_all(fs::path(place) / path, ec);
  }
  if (ec) {
    Error(response, "cannot delete: " + ec.message(), 500);
    return;
  }
  Reply(response, json::object());
}

}  // namespace uagent::web::master
