// Copyright 2026 Timon Gentzsch

#include "include/core/settings_store.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "include/core/config.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr size_t kSettingsBytes = size_t{4} * 1024 * 1024;
constexpr char kImported[] = ".imported";

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

// A text config file's lines as a scope.
json LegacyValues(const std::string& path) {
  json values = json::object();
  for (const auto& [key, value] : ReadEnvValues(path)) {
    if (!key.starts_with("UAGENT_INTERNAL_")) values[key] = value;
  }
  return values;
}

json ReadDocument() {
  return json::parse(ReadFile(SettingsPath(), kSettingsBytes).value_or(""),
                     nullptr, false);
}

// What earlier versions kept in text files, still to be taken over. Each is
// taken once: the user's file when there is no document yet, a project's
// when the document has no entry for its folder (an entry is kept even when
// it overrides nothing, so a file that could not be archived is not taken
// twice).
struct Legacy {
  std::string user;     // ~/.uagent/.config to import
  std::string project;  // <folder>/.uagent/.config to import
  std::string archive;  // imported, but its archiving was interrupted

  bool Any() const {
    return !user.empty() || !project.empty() || !archive.empty();
  }
};

Legacy FindLegacy(const json& document, const std::string& folder,
                  bool trusted) {
  Legacy legacy;
  std::error_code ec;
  const std::string user = UagentConfigPath();
  if (std::filesystem::is_regular_file(user, ec)) {
    if (!PathExists(SettingsPath())) {
      legacy.user = user;
    } else if (!PathExists(user + kImported)) {
      legacy.archive = user;
    }
  }
  const std::string project = folder + "/.uagent/.config";
  const json* projects = JsonObject(document, "projects");
  if (folder.empty() || (projects && projects->contains(folder)) ||
      !std::filesystem::is_regular_file(project, ec)) {
    return legacy;
  }
  // A project's file counted only once its content was approved, or when the
  // caller trusts the folder outright; the same holds for taking it over.
  const json approved = JsonValue(ReadTrustStore(), folder.c_str(), json());
  if (trusted ||
      (JsonValue(approved, "format", 0) == 3 &&
       JsonValue(approved, "config", json()) == LegacyValues(project))) {
    legacy.project = project;
  }
  return legacy;
}

void Archive(const std::string& path) {
  std::error_code ignored;
  if (!path.empty()) std::filesystem::rename(path, path + kImported, ignored);
}

// The document is saved before a file is archived, so an interruption leaves
// the file to be archived later, and never a value changed since to be
// overwritten.
void Import(const std::string& folder, bool trusted) {
  std::string error;
  PrivateJsonStore store(kSettingsFile, nullptr, kSettingsBytes, error);
  if (!store.Ready()) return;
  json& document = store.Data();
  const bool created = document.is_null();
  if (created) document = EmptyDocument();
  if (!Valid(document)) return;
  // Under the lock: another process may have imported since.
  const Legacy legacy = FindLegacy(document, folder, trusted);
  const std::string preference =
      UagentDir(kConfigDir) + "/model-preference.json";
  if (!legacy.user.empty()) {
    json& all = document["all"];
    all = LegacyValues(legacy.user);
    // The model an older /model remembered for every later run. A bare model
    // name belongs to the endpoint it was chosen on.
    const json remembered = json::parse(
        ReadFile(preference, kSettingsBytes).value_or(""), nullptr, false);
    const std::string selection = JsonValue(remembered, "selection", "");
    const std::string base = JsonValue(all, "UAGENT_BASE_URL", "");
    if (!selection.empty() && !all.contains("UAGENT_MODEL") &&
        (JsonValue(remembered, "route", false) || base.empty() ||
         StripTrailingSlashes(base) ==
             StripTrailingSlashes(JsonValue(remembered, "base_url", "")))) {
      all["UAGENT_MODEL"] = selection;
    }
  }
  if (!legacy.project.empty()) {
    document["projects"][folder] = LegacyValues(legacy.project);
  }
  if ((created || !legacy.project.empty()) && !store.Save(error)) return;
  if (!legacy.user.empty()) std::remove(preference.c_str());
  Archive(legacy.user);
  Archive(legacy.archive);
  Archive(legacy.project);
}

std::string Invalid() {
  return "the saved settings cannot be read; fix or remove " + SettingsPath();
}

}  // namespace

std::string SettingsPath() {
  return UagentDir(kConfigDir) + "/" + kSettingsFile;
}

SavedSettings ReadSettings(const std::string& folder, bool trusted) {
  json document = ReadDocument();
  if (FindLegacy(document, folder, trusted).Any()) {
    Import(folder, trusted);
    document = ReadDocument();
  }
  SavedSettings saved;
  saved.stamp = SnapshotFile(SettingsPath());
  if (!Valid(document)) {
    if (PathExists(SettingsPath())) saved.error = Invalid();
    return saved;
  }
  saved.all = ScopeValues(document["all"]);
  if (const json* project = JsonObject(document["projects"], folder.c_str())) {
    saved.project = ScopeValues(*project);
  }
  return saved;
}

std::string ChangeSettings(
    const std::string& folder,
    const std::function<std::string(SettingValues& scope)>& change) {
  // First, so that a document this creates does not pass for an import done.
  (void)ReadSettings(folder);
  std::string error;
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
