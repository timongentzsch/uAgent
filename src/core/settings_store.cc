// Copyright 2026 Timon Gentzsch

#include "include/core/settings_store.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

#include "include/core/config.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr size_t kSettingsBytes = size_t{4} * 1024 * 1024;
constexpr char kImported[] = ".imported";
// In the document: the files taken over whose archiving is still owed.
constexpr char kOwed[] = "archive";

json EmptyDocument() {
  return {{"format", 1}, {"all", json::object()}, {"projects", json::object()}};
}

// The document as its parts, or why it is not one. A malformed document is
// refused, never read as empty: the next save would otherwise discard it.
bool Valid(const json& document) {
  if (!document.is_object() || JsonValue(document, "format", 0) != 1) {
    return false;
  }
  auto strings = [](const json& scope) {
    if (!scope.is_object()) return false;
    for (const auto& item : scope.items()) {
      if (!item.value().is_string()) return false;
    }
    return true;
  };
  const json* all = JsonObject(document, "all");
  const json* projects = JsonObject(document, "projects");
  if (!all || !projects || !strings(*all)) return false;
  if (const auto owed = document.find(kOwed); owed != document.end()) {
    if (!owed->is_array()) return false;
    for (const json& path : *owed) {
      if (!path.is_string()) return false;
    }
  }
  for (const auto& item : projects->items()) {
    if (!strings(item.value())) return false;
  }
  return true;
}

SettingValues ScopeValues(const json& scope) {
  SettingValues values;
  for (const auto& item : scope.items()) {
    values[item.key()] = item.value().get<std::string>();
  }
  return values;
}

std::string Invalid() {
  return "the saved settings cannot be read; fix or remove " + SettingsPath();
}

// A text config file's lines as a scope, read once and whole: what is
// compared with what was approved is what is saved.
bool LegacyValues(const std::string& path, json& values, std::string& error) {
  std::string text;
  if (!ReadRegularFile(path, kSettingsBytes, text, error)) {
    error = "cannot take over " + path + ": " + error;
    return false;
  }
  values = json::object();
  for (const auto& [key, value] : ParseEnvValues(text)) {
    if (!key.starts_with("UAGENT_INTERNAL_")) values[key] = value;
  }
  return true;
}

// The document and its stamp, read until both describe one version.
json ReadDocument(FileStamp& stamp) {
  json document;
  for (int attempt = 0; attempt < 3; ++attempt) {
    stamp = SnapshotFile(SettingsPath());
    document = json::parse(
        ReadFile(SettingsPath(), kSettingsBytes).value_or(""), nullptr, false);
    if (SnapshotFile(SettingsPath()) == stamp) return document;
  }
  stamp.size = -2;  // never equal to a real one: the next check reads again
  return document;
}

std::string PreferencePath() {
  return UagentDir(kConfigDir) + "/model-preference.json";
}

// What earlier versions kept in text files, still to be taken over. Each is
// taken once: the user's when there is no document yet, a project's when the
// document has no entry for its folder (an entry is kept even when it
// overrides nothing). A file taken over is archived after the document is
// saved; until that has worked the document says it is owed.
struct Legacy {
  bool user = false;     // ~/.uagent/.config and the remembered model
  json project;          // <folder>/.uagent/.config, as read
  bool archive = false;  // an archiving is owed
  std::string error;

  bool Any() const {
    return user || archive || !project.is_null() || !error.empty();
  }
};

std::string ProjectFile(const std::string& folder) {
  return folder + "/.uagent/.config";
}

Legacy FindLegacy(const json& document, const std::string& folder,
                  bool trusted) {
  Legacy legacy;
  std::error_code ec;
  const std::string user = UagentConfigPath();
  const bool user_file = std::filesystem::is_regular_file(user, ec);
  if (!PathExists(SettingsPath())) {
    legacy.user = user_file || PathExists(PreferencePath());
  }
  legacy.archive = !JsonValue(document, kOwed, json::array()).empty();
  const json* projects = JsonObject(document, "projects");
  if (folder.empty() || (projects && projects->contains(folder)) ||
      !std::filesystem::is_regular_file(ProjectFile(folder), ec)) {
    return legacy;
  }
  json values;
  if (!LegacyValues(ProjectFile(folder), values, legacy.error)) return legacy;
  // A project's file counted only once its content was approved, or when the
  // caller trusts the folder outright; the same holds for taking it over.
  const json approved = JsonValue(ReadTrustStore(), folder.c_str(), json());
  if (trusted || (JsonValue(approved, "format", 0) == 3 &&
                  JsonValue(approved, "config", json()) == values)) {
    legacy.project = std::move(values);
  }
  return legacy;
}

// Archives what is owed and can be: a file that is gone, or whose backup is
// already there, is owed no more, and a backup is never overwritten. True
// when the list changed.
bool ArchiveOwed(json& document) {
  json still = json::array();
  for (const json& entry : JsonValue(document, kOwed, json::array())) {
    const std::string path = entry.get<std::string>();
    std::error_code failed;
    if (PathExists(path) && !PathExists(path + kImported)) {
      std::filesystem::rename(path, path + kImported, failed);
      if (failed) still.push_back(path);
    }
  }
  const bool changed = still != JsonValue(document, kOwed, json::array());
  document.erase(kOwed);
  if (!still.empty()) document[kOwed] = std::move(still);
  return changed;
}

// The model an older /model remembered for every later run. A bare model
// name belongs to the endpoint it was chosen on, and whatever sets a model
// already decides.
bool ImportModelPreference(json& all, std::string& error) {
  if (!PathExists(PreferencePath())) return true;
  std::string text;
  if (!ReadRegularFile(PreferencePath(), kSettingsBytes, text, error)) {
    error = "cannot take over " + PreferencePath() + ": " + error;
    return false;
  }
  const json remembered = json::parse(text, nullptr, false);
  const std::string selection = JsonValue(remembered, "selection", "");
  if (JsonValue(remembered, "format", 0) != 1 || selection.empty() ||
      selection.find_first_of("\r\n") != std::string::npos) {
    return true;
  }
  const EnvValues values = ScopeValues(all);
  auto resolved = [&](const char* key) {
    std::set<std::string> resolving;
    return ResolveEnvValue(key, values, resolving);
  };
  const std::string base = resolved("UAGENT_BASE_URL");
  if (resolved("UAGENT_MODEL").empty() &&
      (JsonValue(remembered, "route", false) || base.empty() ||
       StripTrailingSlashes(base) ==
           StripTrailingSlashes(JsonValue(remembered, "base_url", "")))) {
    all["UAGENT_MODEL"] = selection;
  }
  return true;
}

// The document is saved, naming the files it took, before one is archived:
// an interruption or a folder that cannot be written leaves the archiving
// owed, to be tried again, and never a value changed since to be overwritten.
// Returns why nothing could be taken over: the files then stay, and nothing is
// saved until they can be.
std::string Import(const std::string& folder, bool trusted) {
  std::string error;
  PrivateJsonStore store(kSettingsFile, EmptyDocument(), kSettingsBytes, error);
  if (!store.Ready()) return error;
  json& document = store.Data();
  if (!Valid(document)) return Invalid();
  // Under the lock: another process may have imported since.
  Legacy legacy = FindLegacy(document, folder, trusted);
  if (!legacy.error.empty()) return legacy.error;
  const std::string user = UagentConfigPath();
  std::error_code ec;
  if (legacy.user) {
    if (std::filesystem::is_regular_file(user, ec) &&
        !LegacyValues(user, document["all"], error)) {
      return error;
    }
    if (!ImportModelPreference(document["all"], error)) return error;
  }
  const bool project = !legacy.project.is_null();
  if (project) document["projects"][folder] = std::move(legacy.project);
  if (legacy.user || project) {
    json owed = JsonValue(document, kOwed, json::array());
    if (legacy.user) owed.push_back(user);
    if (project) owed.push_back(ProjectFile(folder));
    document[kOwed] = std::move(owed);
    if (!store.Save(error)) return error;
  }
  if (legacy.user) std::remove(PreferencePath().c_str());
  if (ArchiveOwed(document)) store.Save(error);
  return error;
}

}  // namespace

std::string SettingsPath() {
  return UagentDir(kConfigDir) + "/" + kSettingsFile;
}

SavedSettings ReadSettings(const std::string& folder, bool trusted) {
  SavedSettings saved;
  json document = ReadDocument(saved.stamp);
  if (FindLegacy(document, folder, trusted).Any()) {
    saved.error = Import(folder, trusted);
    document = ReadDocument(saved.stamp);
  }
  if (!Valid(document)) {
    if (saved.error.empty() && PathExists(SettingsPath())) {
      saved.error = Invalid();
    }
    return saved;
  }
  saved.all = ScopeValues(document["all"]);
  if (const json* project = JsonObject(document["projects"], folder.c_str())) {
    saved.project = ScopeValues(*project);
  }
  return saved;
}

json ExportSettings(std::string& error) {
  SavedSettings read = ReadSettings("");  // takes over what is still owed
  error = read.error;
  FileStamp ignored;
  json document = ReadDocument(ignored);
  if (!Valid(document)) return EmptyDocument();
  document.erase(kOwed);  // this host's own bookkeeping
  return document;
}

std::string ReplaceSettings(json document) {
  if (!Valid(document)) {
    return "expected {\"format\": 1, \"all\": {name: value}, \"projects\": "
           "{folder: {name: value}}}";
  }
  std::string error = ReadSettings("").error;
  if (!error.empty()) return error;
  PrivateJsonStore store(kSettingsFile, EmptyDocument(), kSettingsBytes, error);
  if (!store.Ready()) return error;
  // What this host still owes stays its own; a document from elsewhere
  // names no file to move here.
  const json owed = JsonValue(store.Data(), kOwed, json::array());
  document.erase(kOwed);
  if (!owed.empty()) document[kOwed] = owed;
  store.Data() = std::move(document);
  store.Save(error);
  return error;
}

std::string ChangeSettings(
    const std::string& folder,
    const std::function<std::string(SettingValues& scope)>& change) {
  // First, so that a document this creates does not pass for an import done;
  // while one is still owed, nothing is saved.
  std::string error = ReadSettings(folder).error;
  if (!error.empty()) return error;
  PrivateJsonStore store(kSettingsFile, EmptyDocument(), kSettingsBytes, error);
  if (!store.Ready()) return error;
  json& document = store.Data();
  if (!Valid(document)) return Invalid();
  json& scope = folder.empty() ? document["all"] : document["projects"][folder];
  if (!scope.is_object()) scope = json::object();
  SettingValues values = ScopeValues(scope);
  error = change(values);
  if (!error.empty()) return error;
  scope = values;
  store.Save(error);
  return error;
}

}  // namespace uagent
