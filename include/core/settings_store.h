// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
#define UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
// The host's settings: one private JSON document,
// {"format": 2, "all": {...}, "projects": {"<folder>": {...}}}. A scope names
// each setting as the registry's `key` does and holds its value in its own
// type: {"model": "x", "limits.maxSteps": 40, "sandbox.enabled": false}.
// "variables" in a scope holds what a value may refer to as $NAME, kept and
// never shown. The document may be edited by hand: what it holds that is no
// setting, or no value a setting takes, is reported and otherwise left
// alone. While a file taken over from an earlier version could not be
// archived yet, "archive" lists it.
//
// In the process a scope is a map from a setting's environment name to its
// value as text, which is also how the environment and the command line
// give one.

#include <functional>
#include <map>
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
  // What the document holds that was not taken: a name that is no setting, a
  // value its setting does not take. The rest applies.
  std::string warning;
};

// What is saved for all conversations and for the conversations in `folder`.
// The first read takes over what an earlier version kept in text files and
// leaves each as .config.imported: ~/.uagent/.config, and
// <folder>/.uagent/.config when its content was approved or the caller says
// the folder is `trusted`.
SavedSettings ReadSettings(const std::string& folder, bool trusted = false);

// One scope as the document holds it, also the values reading does not take:
// what a change to it is compared with, and what a correction replaces.
SettingValues HeldSettings(const std::string& folder);

// The one way a setting is saved: `change` edits one scope's map (all
// conversations, or `folder`'s when it is not empty) while the document is
// locked against every other writer, and the result is saved in one piece.
// Returning an error leaves the document as it was. Returns that error, or
// why the document could not be read or saved.
std::string ChangeSettings(
    const std::string& folder,
    const std::function<std::string(SettingValues& scope)>& change);

// Everything saved, scope by scope: what a backup holds, or another host is
// given.
struct AllSettings {
  SettingValues all;
  std::map<std::string, SettingValues> projects;
};
// Everything saved, as the document spells it. `error` says why it is not
// all there.
json ExportSettings(std::string& error);
// A document from a backup or another host, in this format or the one
// before, as scopes. False with why it is not one: unlike a document edited
// in place, one handed over is taken whole or not at all.
bool ParseSettings(const json& document, AllSettings& settings,
                   std::string& error);
// Replaces everything saved; the values are the caller's to check. Returns
// why it was not saved.
std::string ReplaceSettings(const AllSettings& settings);

// The JSON Schema of the document, for an editor: written beside it as
// settings.schema.json, which the document names.
json SettingsSchema();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SETTINGS_STORE_H_
