// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
#define UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
// The host's settings: one private JSON document,
// {"format": 3, "all": {...}, "projects": {"<folder>": {...}}}.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/file_watch.h"

namespace uagent {

inline constexpr char kSettingsFile[] = "settings.json";

std::string SettingsPath();

struct SavedSettings {
  SettingValues all;
  // What `folder` overrides.
  SettingValues project;
  // The document's stamp when it was read.
  FileStamp stamp;
  // Set when the document cannot be read; nothing is saved over it then.
  std::string error;
  // What the document holds that was not taken; the rest applies.
  std::string warning;
  // Keys whose value was not taken: a reader must not run on their default.
  std::vector<std::string> invalid_keys;
};

// What is saved for all conversations and for those in `folder`.
SavedSettings ReadSettings(const std::string& folder);

// One scope as the document holds it, including values reading does not take.
SettingValues HeldSettings(const std::string& folder);

// `change` edits one scope's map under the document's lock; an error it
// returns leaves the document as it was. Returns the error, or empty.
std::string ChangeSettings(
    const std::string& folder,
    const std::function<std::string(SettingValues& scope)>& change);

// Everything saved, as the document spells it; `error` when not all of it.
json ExportSettings(std::string& error);

// The document's JSON Schema, written beside it for an editor.
json SettingsSchema();
inline constexpr char kSettingsSchemaFile[] = "settings.schema.json";

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
