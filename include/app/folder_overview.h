// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_FOLDER_OVERVIEW_H_
#define UAGENT_INCLUDE_APP_FOLDER_OVERVIEW_H_
// A folder at a glance: what its conversations spent and changed, and where
// its repository stands. The web's folder overview and /overview read it.

#include <string>

#include "include/core/json.h"

namespace uagent {
// {spend: {today, week}, changed: {files, added, removed}, git: {branch,
// uncommitted}}; `git` only for a repository. Read from disk and git, never
// the network, and cached for a few seconds.
json FolderFacts(const std::string& folder, const std::string& host = "");
json FolderGitFacts(const std::string& folder);
// The same as text, with the conversations that work and those that need you.
std::string FolderOverviewText(const std::string& folder);
}  // namespace uagent
#endif
