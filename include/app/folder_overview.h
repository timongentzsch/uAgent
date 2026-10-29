// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_FOLDER_OVERVIEW_H_
#define UAGENT_INCLUDE_APP_FOLDER_OVERVIEW_H_
// A folder at a glance: what its conversations spent and changed, and where
// its repository stands. The web's folder overview and /overview read it.

#include <string>

#include "include/core/json.h"

namespace uagent {
// {spend: {today, week}, changed: {files, added, removed}, git: {branch,
// uncommitted}}, from the turns its conversations finished in the last 7
// days and the files those of today changed; `git` only where the folder is
// a repository. Read from disk and git, never the network, and kept for a
// few seconds.
json FolderFacts(const std::string& folder);
// The same, with how many conversations there are, work and need you, and
// which need you, as lines of text.
std::string FolderOverviewText(const std::string& folder);
}  // namespace uagent
#endif
