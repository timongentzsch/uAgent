// Copyright 2026 Timon Gentzsch

#include "include/core/settings_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/fd.h"
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

constexpr int kFormat = 2;
constexpr char kVariables[] = "variables";
constexpr char kSchemaFile[] = "settings.schema.json";

json EmptyDocument() {
  return {{"$schema", std::string("./") + kSchemaFile},
          {"format", kFormat},
          {"all", json::object()},
          {"projects", json::object()}};
}

// The document's shape, in this format. A malformed document is refused,
// never read as empty: the next save would otherwise discard it. What a
// scope holds is not judged here: a name or value that cannot be taken is
// reported and the rest applies.
bool Valid(const json& document) {
  if (!document.is_object() || JsonValue(document, "format", 0) != kFormat) {
    return false;
  }
  const json* all = JsonObject(document, "all");
  const json* projects = JsonObject(document, "projects");
  if (!all || !projects) return false;
  if (const auto owed = document.find(kOwed); owed != document.end()) {
    if (!owed->is_array()) return false;
    for (const json& path : *owed) {
      if (!path.is_string()) return false;
    }
  }
  return std::ranges::all_of(projects->items(), [](const auto& item) {
    return item.value().is_object();
  });
}

// A setting's value as the document holds it: in the setting's own type
// where the text is of that type, and as the text otherwise (a reference to
// a variable, or a value reading will report).
json Typed(const ConfigDescriptor& descriptor, const std::string& text) {
  switch (descriptor.type) {
    case ConfigType::kInt:
      if (int64_t value = 0; ParseInt64(text.c_str(), value)) return value;
      break;
    case ConfigType::kDouble:
      // A whole number is written as one: 2, not 2.0.
      if (int64_t whole = 0; ParseInt64(text.c_str(), whole)) return whole;
      if (double value = 0; ParseFiniteDouble(text.c_str(), value)) {
        return value;
      }
      break;
    case ConfigType::kBool:
      if (bool value = false; ParseBool(text, value)) return value;
      break;
    case ConfigType::kString:
      // The providers are an object, and are held as one.
      if (descriptor.sensitivity == Sensitivity::kCompositeSecret) {
        json parsed = json::parse(text, nullptr, false);
        if (parsed.is_object()) return parsed;
      }
      break;
  }
  return text;
}

// The same value as text, which is how every other layer gives one.
std::string Text(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
  return JsonDump(value);
}

// A scope as the document holds it. What `prior` held that is no setting
// stays where its author put it.
json EncodeScope(const SettingValues& values, const json& prior = {}) {
  json scope = json::object();
  if (prior.is_object()) {
    for (const auto& [key, value] : prior.items()) {
      if (key != kVariables && !FindConfigKey(key)) scope[key] = value;
    }
  }
  json variables = json::object();
  for (const auto& [name, text] : values) {
    if (const ConfigDescriptor* descriptor = FindConfigDescriptor(name)) {
      scope[std::string(descriptor->key)] = Typed(*descriptor, text);
    } else {
      variables[name] = text;
    }
  }
  if (!variables.empty()) scope[kVariables] = std::move(variables);
  return scope;
}

// A scope of the document as settings. With `problems`, what cannot be taken
// is named there, each with `where` before it, and left out; without, every
// value of a setting is taken as it stands.
SettingValues DecodeScope(const json& scope, const std::string& where,
                          std::vector<std::string>* problems) {
  SettingValues values;
  const auto report = [&](std::string problem) {
    if (problems) problems->push_back(where + std::move(problem));
  };
  for (const auto& [key, held] : scope.items()) {
    if (key == kVariables) {
      if (!held.is_object()) {
        report("variables must map names to text");
        continue;
      }
      for (const auto& [name, text] : held.items()) {
        if (text.is_string()) {
          values[name] = text.get<std::string>();
        } else {
          report("variables." + name + " must be text");
        }
      }
      continue;
    }
    const ConfigDescriptor* descriptor = FindConfigKey(key);
    if (!descriptor) {
      report(key + " is not a setting");
      continue;
    }
    std::string text = Text(held), problem;
    // A reference is judged by what it resolves to, where it is read.
    if (problems && text.find('$') == std::string::npos &&
        !ValidSettingValue(*descriptor, text, problem, key)) {
      report(std::move(problem));
      continue;
    }
    values[std::string(descriptor->environment)] = std::move(text);
  }
  return values;
}

// A scope spelled the way the environment and the format before this one
// spell it, {NAME: text}, as this document holds one. False when it is not
// such a scope.
bool FromNames(const json& held, json& out) {
  if (!held.is_object()) return false;
  SettingValues values;
  for (const auto& [name, text] : held.items()) {
    if (!text.is_string()) return false;
    values[name] = text.get<std::string>();
  }
  out = EncodeScope(values);
  return true;
}

// The document of the format before this one as this one, or nothing when it
// is not one: {"format": 1, "all": {NAME: text}, "projects": {folder: {...}}}.
bool Upgrade(json& document) {
  const auto& scope = FromNames;
  json upgraded = EmptyDocument();
  const json* projects = JsonObject(document, "projects");
  if (!document.is_object() || JsonValue(document, "format", 0) != 1 ||
      !projects ||
      !scope(JsonValue(document, "all", json()), upgraded["all"])) {
    return false;
  }
  for (const auto& [folder, held] : projects->items()) {
    if (!scope(held, upgraded["projects"][folder])) return false;
  }
  if (document.contains(kOwed)) upgraded[kOwed] = document[kOwed];
  document = std::move(upgraded);
  return true;
}

// The schema beside the document, kept current with this build.
void WriteSchema() {
  const std::string path = UagentDir(kConfigDir) + "/" + kSchemaFile;
  const std::string text = JsonDump(SettingsSchema(), 2) + "\n";
  if (ReadFile(path, kSettingsBytes).value_or("") == text) return;
  std::string ignored;
  AtomicWriteFile(path, text, kPrivateFileMode, /*preserve_mode=*/false,
                  ignored);
}

std::string Joined(const std::vector<std::string>& problems) {
  std::string text;
  for (const std::string& problem : problems) {
    text += (text.empty() ? "" : "; ") + problem;
  }
  return text.empty() ? text : SettingsPath() + ": " + text;
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
  bool earlier = false;  // the document is in the format before this one
  std::string error;

  bool Any() const {
    return user || archive || earlier || !project.is_null() || !error.empty();
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
  legacy.earlier = JsonValue(document, "format", 0) == 1;
  // Taken once: not when the folder has an entry, nor while its file, taken
  // already, still waits to be archived.
  const json* projects = JsonObject(document, "projects");
  const json owed = JsonValue(document, kOwed, json::array());
  if (folder.empty() || (projects && projects->contains(folder)) ||
      std::find(owed.begin(), owed.end(), ProjectFile(folder)) != owed.end() ||
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

// Archives what is owed and can be. A file is given its backup name without
// ever replacing one (a link, which fails where the name is taken, then the
// old name removed), and is owed no more only when it is known to be gone or
// its backup known to be there, on disk. True when the list changed.
bool ArchiveOwed(json& document) {
  json still = json::array();
  for (const json& entry : JsonValue(document, kOwed, json::array())) {
    const std::string path = entry.get<std::string>();
    const std::string backup = path + kImported;
    const bool named = link(path.c_str(), backup.c_str()) == 0 ||
                       errno == EEXIST || errno == ENOENT;
    const bool removed =
        named && (unlink(path.c_str()) == 0 || errno == ENOENT);
    const Fd folder(open(std::filesystem::path(path).parent_path().c_str(),
                         O_RDONLY | O_DIRECTORY));
    const bool settled = folder ? fsync(folder.Get()) == 0 : errno == ENOENT;
    if (!(removed && settled)) still.push_back(path);
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
  const EnvValues values = DecodeScope(all, "", nullptr);
  auto resolved = [&](const char* key) {
    std::set<std::string> resolving;
    return ResolveEnvValue(key, values, resolving);
  };
  const std::string base = resolved("UAGENT_BASE_URL");
  if (resolved("UAGENT_MODEL").empty() &&
      (JsonValue(remembered, "route", false) || base.empty() ||
       StripTrailingSlashes(base) ==
           StripTrailingSlashes(JsonValue(remembered, "base_url", "")))) {
    all[std::string(Cfg("UAGENT_MODEL").key)] = selection;
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
  // Under the lock: another process may have upgraded or imported since.
  const bool upgraded = Upgrade(document);
  if (!Valid(document)) return Invalid();
  Legacy legacy = FindLegacy(document, folder, trusted);
  if (!legacy.error.empty()) return legacy.error;
  const std::string user = UagentConfigPath();
  std::error_code ec;
  if (legacy.user) {
    json names = json::object();
    if (std::filesystem::is_regular_file(user, ec) &&
        !LegacyValues(user, names, error)) {
      return error;
    }
    FromNames(names, document["all"]);
    if (!ImportModelPreference(document["all"], error)) return error;
  }
  const bool project = !legacy.project.is_null();
  if (project) FromNames(legacy.project, document["projects"][folder]);
  if (legacy.user || project) {
    json owed = JsonValue(document, kOwed, json::array());
    if (legacy.user) owed.push_back(user);
    if (project) owed.push_back(ProjectFile(folder));
    document[kOwed] = std::move(owed);
  }
  if ((upgraded || legacy.user || project) && !store.Save(error)) return error;
  if (legacy.user) std::remove(PreferencePath().c_str());
  if (ArchiveOwed(document)) store.Save(error);
  WriteSchema();
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
  std::vector<std::string> problems;
  saved.all = DecodeScope(document["all"], "", &problems);
  if (const json* project = JsonObject(document["projects"], folder.c_str())) {
    saved.project = DecodeScope(*project, "this project: ", &problems);
  }
  saved.warning = Joined(problems);
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

bool ParseSettings(const json& document, AllSettings& settings,
                   std::string& error) {
  json current = document;
  Upgrade(current);
  if (!Valid(current)) {
    error =
        "expected {\"format\": 2, \"all\": {setting: value}, "
        "\"projects\": {folder: {setting: value}}}";
    return false;
  }
  std::vector<std::string> problems;
  settings.all = DecodeScope(current["all"], "", &problems);
  for (const auto& [folder, scope] : current["projects"].items()) {
    settings.projects[folder] = DecodeScope(scope, folder + ": ", &problems);
  }
  if (problems.empty()) return true;
  error = problems.front();
  return false;
}

std::string ReplaceSettings(const AllSettings& settings) {
  std::string error = ReadSettings("").error;
  if (!error.empty()) return error;
  PrivateJsonStore store(kSettingsFile, EmptyDocument(), kSettingsBytes, error);
  if (!store.Ready()) return error;
  // What this host still owes stays its own; a document from elsewhere
  // names no file to move here.
  json document = EmptyDocument();
  document["all"] = EncodeScope(settings.all);
  for (const auto& [folder, scope] : settings.projects) {
    document["projects"][folder] = EncodeScope(scope);
  }
  if (store.Data().contains(kOwed)) document[kOwed] = store.Data()[kOwed];
  store.Data() = std::move(document);
  store.Save(error);
  WriteSchema();
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
  // As held, also what reading would not take: a value someone typed into
  // the document is theirs to correct, not this save's to drop.
  SettingValues values = DecodeScope(scope, "", nullptr);
  error = change(values);
  if (!error.empty()) return error;
  scope = EncodeScope(values, scope);
  store.Save(error);
  WriteSchema();
  return error;
}

json SettingsSchema() {
  json properties = json::object();
  for (const ConfigDescriptor& descriptor : kConfigRegistry) {
    json value = {{"type", ConfigTypeName(descriptor.type)}};
    if (descriptor.type == ConfigType::kInt) {
      if (descriptor.minimum != kConfigAnyMin) {
        value["minimum"] = descriptor.minimum;
      }
      if (descriptor.maximum != kConfigAnyMax) {
        value["maximum"] = descriptor.maximum;
      }
    }
    if (!descriptor.choices.empty()) {
      value["enum"] = json::array({""});
      for (std::string_view choice : descriptor.choices) {
        value["enum"].push_back(choice);
      }
    }
    if (descriptor.sensitivity == Sensitivity::kCompositeSecret) {
      value["type"] = "object";
    }
    std::visit([&](const auto& held) { value["default"] = held; },
               descriptor.default_value);
    json property = {
        {"description",
         std::string(descriptor.purpose.empty() ? descriptor.description
                                                : descriptor.purpose)},
        // Not a keyword of the schema: the same setting in the environment.
        {"x-env", descriptor.environment}};
    if (value["type"] == "string") {
      property.update(value);
    } else {
      // Any setting may name a variable instead of holding its value.
      property["anyOf"] = json::array(
          {std::move(value), {{"type", "string"}, {"pattern", "\\$"}}});
    }
    properties[std::string(descriptor.key)] = std::move(property);
  }
  properties[kVariables] = {
      {"type", "object"},
      {"description",
       "Values a setting may refer to as $NAME, such as an API key."},
      {"additionalProperties", {{"type", "string"}}}};
  const json scope = {{"type", "object"},
                      {"properties", std::move(properties)},
                      {"additionalProperties", false}};
  return {{"$schema", "http://json-schema.org/draft-07/schema#"},
          {"title", "uagent settings"},
          {"type", "object"},
          {"required", json::array({"format", "all", "projects"})},
          {"properties",
           {{"$schema", {{"type", "string"}}},
            {"format", {{"const", kFormat}}},
            {"all",
             {{"$ref", "#/definitions/scope"},
              {"description", "Settings for all conversations."}}},
            {"projects",
             {{"type", "object"},
              {"description",
               "Settings for the conversations in one folder, by its path."},
              {"additionalProperties", {{"$ref", "#/definitions/scope"}}}}},
            {kOwed, {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
          {"definitions", {{"scope", scope}}}};
}

}  // namespace uagent
