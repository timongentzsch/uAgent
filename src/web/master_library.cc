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
void Master::Library(const Request& request, Response& response) {
  // Load each SSH workspace separately. An unavailable execution host cannot
  // delay local files or the results from another host.
  if (request.has_param("session")) {
    const std::string id = request.get_param_value("session");
    const WorkspaceIdentity workspace = host_.WorkspaceOf(id);
    if (!workspace.Remote()) {
      Error(response, "unknown remote workspace", 404);
      return;
    }
    json result = WorkspaceFiles(workspace, id, {{"mode", "designs"}});
    if (result.contains("error")) {
      Error(response, JsonValue(result, "error", "Workspace unavailable"),
            JsonValue(result, "status", 503));
      return;
    }
    for (json& item : result["items"]) {
      item["place"] = workspace.Key();
      item["session"] = id;
    }
    Reply(response, result);
    return;
  }
  const json catalogue = host_.Catalogue();
  std::map<std::string, json> places;
  for (const json& space : catalogue["spaces"]) {
    const std::string path = JsonValue(space, "path", "");
    places[path] = {{"path", path},
                    {"name", JsonValue(space, "name", "")},
                    {"workspace", true}};
  }
  std::map<std::string, std::string> addresses;
  for (const json& host : remotes_->List()) {
    addresses[JsonValue(host, "id", "")] = JsonValue(host, "address", "");
  }
  json items = json::array();
  std::map<std::string, int64_t> latest;
  for (const json& session : catalogue["sessions"]) {
    const std::string cwd = JsonValue(session, "cwd", "");
    const std::string id = JsonValue(session, "id", "");
    if (cwd.empty()) continue;
    const WorkspaceIdentity workspace{JsonValue(session, "host", ""), cwd};
    const std::string key = workspace.Key();
    json& place = places[key];
    if (place.is_null()) {
      place = {{"path", cwd},
               {"name", fs::path(cwd).filename().string()},
               {"workspace", false}};
      if (workspace.Remote()) {
        place["host"] = workspace.host;
        place["key"] = key;
        const auto address = addresses.find(workspace.host);
        const std::string label =
            address == addresses.end() ? workspace.host : address->second;
        place["name"] = label + " · " + JsonValue(place, "name", "");
      }
    }
    // The conversation a place's designs open in: its latest of its own.
    const int64_t updated = JsonValue(session, "updated", int64_t{0});
    if (JsonValue(session, "parent", "").empty() && updated >= latest[key]) {
      latest[key] = updated;
      place["session"] = id;
    }
    if (workspace.Remote() && !place.contains("session")) place["session"] = id;
    // A conversation can be removed after the catalogue snapshot.
    const std::string session_path = host_.AssetPath(id);
    if (!session_path.empty()) AddShared(id, session_path, key, items);
  }
  json paths = json::array(), listed = json::array();
  for (auto& [key, place] : places) {
    if (!place.contains("host")) {
      paths.push_back(key);
      for (json& design : workspace_files::Designs(key)) {
        items.push_back(std::move(design));
      }
    }
    listed.push_back(std::move(place));
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
        host_.DeleteShared(JsonValue(body, "session_id", ""), key);
    if (error.empty()) {
      Reply(response, json::object());
    } else {
      Error(response, error, error == "not found" ? 404 : 500);
    }
    return;
  }
  const std::string place = JsonValue(body, "place", "");
  const std::string session = JsonValue(body, "session_id", "");
  WorkspaceIdentity workspace = host_.WorkspaceOf(session);
  if (session.empty()) {
    // A local place is named by its absolute path, with no conversation.
    const json catalogue = host_.Catalogue();
    const auto known = [&](const char* list, const char* field) {
      return std::ranges::any_of(catalogue[list], [&](const json& each) {
        return JsonValue(each, "host", "").empty() &&
               JsonValue(each, field, "") == place;
      });
    };
    if (known("spaces", "path") || known("sessions", "cwd")) {
      workspace.path = place;
    }
  }
  if (workspace.path.empty() || workspace.Key() != place) {
    Error(response, "not found", 404);
    return;
  }
  const json result =
      WorkspaceFiles(workspace, session,
                     {{"mode", "delete_design"}, {"kind", kind}, {"key", key}});
  if (result.contains("error")) {
    Error(response, JsonValue(result, "error", "cannot delete"),
          JsonValue(result, "status", 503));
    return;
  }
  Reply(response, json::object());
}

}  // namespace uagent::web::master
