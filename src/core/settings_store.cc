// Copyright 2026 Timon Gentzsch

#include "include/core/settings_store.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/private_store.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr size_t kSettingsBytes = size_t{4} * 1024 * 1024;
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
      if (int64_t value = 0; ParseInt64(text.c_str(), value)) {
        return value;
      }
      break;
    case ConfigType::kDouble:
      // A whole number is written as one: 2, not 2.0.
      if (int64_t whole = 0; ParseInt64(text.c_str(), whole)) {
        return whole;
      }
      if (double value = 0; ParseFiniteDouble(text.c_str(), value)) {
        return value;
      }
      break;
    case ConfigType::kBool:
      if (bool value = false; ParseBool(text, value)) {
        return value;
      }
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
  // A variable that is not text was never taken, so it is not in `values`:
  // it stays as written too.
  json variables = json::object();
  if (const json* held = JsonObject(prior, kVariables)) {
    for (const auto& [name, value] : held->items()) {
      if (!value.is_string()) variables[name] = value;
    }
  }
  for (const auto& [name, text] : values) {
    if (const ConfigDescriptor* descriptor = FindConfigDescriptor(name)) {
      // A value nobody changed stays as its author wrote it: writing it in
      // its setting's kind could turn one that was not taken into one that
      // is.
      const std::string key(descriptor->key);
      const json* held =
          prior.is_object() && prior.contains(key) ? &prior[key] : nullptr;
      scope[key] =
          held && Text(*held) == text ? *held : Typed(*descriptor, text);
    } else {
      variables[name] = text;
    }
  }
  if (!variables.empty()) {
    scope[kVariables] = std::move(variables);
  } else if (prior.is_object() && prior.contains(kVariables) &&
             !prior[kVariables].is_object()) {
    scope[kVariables] = prior[kVariables];
  }
  return scope;
}

// Whether `held` is of the kind `descriptor` holds. Text always may be: it is
// a reference, or the value spelled out.
bool KindFits(const ConfigDescriptor& descriptor, const json& held) {
  if (held.is_string()) return true;
  switch (descriptor.type) {
    case ConfigType::kInt:
    case ConfigType::kDouble:
      return held.is_number();
    case ConfigType::kBool:
      return held.is_boolean();
    case ConfigType::kString:
      return descriptor.sensitivity == Sensitivity::kCompositeSecret &&
             held.is_object();
  }
  return false;
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
    std::string text = Text(held);
    if (problems && !KindFits(*descriptor, held)) {
      report(key + " expects " +
             (descriptor->type == ConfigType::kBool     ? "true or false"
              : descriptor->type == ConfigType::kString ? "text"
                                                        : "a number"));
      continue;
    }
    // A reference is judged below by what it resolves to. The value is
    // taken as written: the check works on its own copy.
    if (std::string checked = text, problem;
        problems && text.find('$') == std::string::npos &&
        !ValidSettingValue(*descriptor, checked, problem, key)) {
      report(std::move(problem));
      continue;
    }
    values[std::string(descriptor->environment)] = std::move(text);
  }
  // Once every name of the scope is known: the environment's names count as
  // they do where settings are read, and one defined nowhere sets nothing.
  for (auto held = values.begin(); problems && held != values.end();) {
    const ConfigDescriptor* descriptor = FindConfigDescriptor(held->first);
    std::set<std::string> resolving;
    std::string problem;
    std::string resolved =
        descriptor && held->second.find('$') != std::string::npos
            ? ResolveEnvValue(held->first, values, resolving)
            : "";
    if (resolved.empty() ||
        ValidSettingValue(*descriptor, resolved, problem, descriptor->key)) {
      ++held;
      continue;
    }
    report(std::move(problem));
    held = values.erase(held);
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

// The document of the format before this one, which the last release still
// writes, as this one; or nothing when it is not one:
// {"format": 1, "all": {NAME: text}, "projects": {folder: {...}}}.
bool Upgrade(json& document) {
  json upgraded = EmptyDocument();
  const json* projects = JsonObject(document, "projects");
  if (!document.is_object() || JsonValue(document, "format", 0) != 1 ||
      !projects ||
      !FromNames(JsonValue(document, "all", json()), upgraded["all"])) {
    return false;
  }
  for (const auto& [folder, held] : projects->items()) {
    if (!FromNames(held, upgraded["projects"][folder])) return false;
  }
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

// Rewrites a saved document of the format before in this one, under the
// lock: another process may have done it since it was read.
std::string UpgradeSaved() {
  std::string error;
  PrivateJsonStore store(kSettingsFile, EmptyDocument(), kSettingsBytes, error);
  if (store.Ready() && Upgrade(store.Data())) store.Save(error);
  return error;
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

}  // namespace

std::string SettingsPath() {
  return UagentDir(kConfigDir) + "/" + kSettingsFile;
}

SavedSettings ReadSettings(const std::string& folder) {
  SavedSettings saved;
  json document = ReadDocument(saved.stamp);
  if (JsonValue(document, "format", 0) == 1) {
    saved.error = UpgradeSaved();
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
  // Once a run: a build newer than the schema beside the document replaces
  // it.
  static std::once_flag schema;
  std::call_once(schema, WriteSchema);
  return saved;
}

SettingValues HeldSettings(const std::string& folder) {
  FileStamp ignored;
  const json document = ReadDocument(ignored);
  if (!Valid(document)) return {};
  const json* scope = folder.empty()
                          ? JsonObject(document, "all")
                          : JsonObject(document["projects"], folder.c_str());
  return scope ? DecodeScope(*scope, "", nullptr) : SettingValues{};
}

json ExportSettings(std::string& error) {
  error = ReadSettings("").error;
  FileStamp ignored;
  json document = ReadDocument(ignored);
  return Valid(document) ? document : EmptyDocument();
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
  json document = EmptyDocument();
  document["all"] = EncodeScope(settings.all);
  for (const auto& [folder, scope] : settings.projects) {
    document["projects"][folder] = EncodeScope(scope);
  }
  store.Data() = std::move(document);
  store.Save(error);
  WriteSchema();
  return error;
}

std::string ChangeSettings(
    const std::string& folder,
    const std::function<std::string(SettingValues& scope)>& change) {
  // First: a document that cannot be read is not replaced by a new one.
  std::string error = ReadSettings(folder).error;
  if (!error.empty()) return error;
  PrivateJsonStore store(kSettingsFile, EmptyDocument(), kSettingsBytes, error);
  if (!store.Ready()) return error;
  json& document = store.Data();
  // An editor takes no lock: the document is read with its stamp, and what
  // an editor saves from here on is not overwritten.
  FileStamp read;
  if (json held = ReadDocument(read); !held.is_discarded()) {
    document = std::move(held);
  }
  if (!Valid(document)) return Invalid();
  json& scope = folder.empty() ? document["all"] : document["projects"][folder];
  if (!scope.is_object()) scope = json::object();
  // As held, also what reading would not take: a value someone typed into
  // the document is theirs to correct, not this save's to drop.
  SettingValues values = DecodeScope(scope, "", nullptr);
  error = change(values);
  if (!error.empty()) return error;
  scope = EncodeScope(values, scope);
  if (SnapshotFile(SettingsPath()) != read) {
    return SettingsPath() + " changed while this was being saved; try again";
  }
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
    } else if (descriptor.type == ConfigType::kDouble) {
      value["minimum"] = 0;
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
    if (value["type"] == "string" && !value.contains("enum")) {
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
              {"additionalProperties", {{"$ref", "#/definitions/scope"}}}}}}},
          {"definitions", {{"scope", scope}}}};
}

}  // namespace uagent
