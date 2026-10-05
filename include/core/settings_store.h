// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
#define UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
// The host's settings: one private JSON document it alone reads and writes,
// {"format": 1, "all": {...}, "projects": {"<folder>": {...}}}. Each scope is
// one map of names to values as they were given. A name the registry knows
// is a setting; any other is a binding a setting's value may refer to as
// $NAME, kept and never shown. While a file taken over from an earlier
// version could not be archived yet, "archive" lists it.

#include <functional>
#include <string>

#include "include/core/config.h"
#include "include/core/file_watch.h"

namespace uagent {

using SettingValues = EnvValues;

inline constexpr char kSettingsFile[] = "settings.json";

std::string SettingsPath();

struct SavedSettings {
  SettingValues all;
  // What `folder` overrides; empty for a folder that overrides nothing.
  SettingValues project;
  // The document's stamp when it was read, for a later "did it change".
  FileStamp stamp;
  // Why the saved settings are not all here: the document cannot be read, or
  // an earlier version's file could not be taken over. Nothing is saved over
  // either until it is put right.
  std::string error;
};

// What is saved for all conversations and for the conversations in `folder`.
// The first read takes over what an earlier version kept in text files and
// leaves each as .config.imported: ~/.uagent/.config, and
// <folder>/.uagent/.config when its content was approved or the caller says
// the folder is `trusted`.
SavedSettings ReadSettings(const std::string& folder, bool trusted = false);

// The one way a setting is saved: `change` edits one scope's map (all
// conversations, or `folder`'s when it is not empty) while the document is
// locked against every other writer, and the result is saved in one piece.
// Returning an error leaves the document as it was. Returns that error, or
// why the document could not be read or saved.
std::string ChangeSettings(
    const std::string& folder,
    const std::function<std::string(SettingValues& scope)>& change);

// Everything saved, as one document: a backup, or what another host is
// given. `error` says why it is not all there.
json ExportSettings(std::string& error);
// Replaces everything saved with `document`, whose shape is checked here and
// whose values are the caller's to check. Returns why it was not saved.
std::string ReplaceSettings(json document);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
