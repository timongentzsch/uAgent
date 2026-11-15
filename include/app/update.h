// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_UPDATE_H_
#define UAGENT_INCLUDE_APP_UPDATE_H_
// The newest release on GitHub and what updating this install takes.

#include <string>

#include "include/core/json.h"

namespace uagent {

// This build and the release last checked for: `version`, `install` (how the
// installer says it was installed: release, source, checkout or container;
// empty when nothing says), `latest` with its `notes` and `url`,
// `available`, the `command` that updates it by hand, and `apply`: whether
// the update can be run from here, with `blocked` when the install allows
// it but `terminal` (the web terminal's switch) or its folder does not.
json UpdateState(bool terminal);

// Asks GitHub for the newest release and keeps the answer; the state, with
// `error` when it could not be read.
json CheckForUpdate(bool terminal);

// Runs the installer of release `version` over this install, replacing its
// files only; the host restarts itself afterwards. Returns the installer's
// output, and sets `error` when nothing was installed.
std::string ApplyUpdate(const std::string& version, bool terminal,
                        std::string& error);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_APP_UPDATE_H_
