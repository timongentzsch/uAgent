// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_DEFAULTS_H_
#define UAGENT_INCLUDE_CORE_DEFAULTS_H_
// Opinionated built-in names and lists: the strings and string lists uAgent
// ships a view on. The convention across the three constant homes:
//   - names and lists live here;
//   - numbers live in include/core/limits.h;
//   - settings a user can change live in include/core/config_registry.h.
// A list a user can extend names the setting that extends it, next to the
// list. Keep this header dependency free -- it is included widely.

#include <string_view>

namespace uagent {

// Credential stores that sandboxed commands cannot read and the file tools
// refuse, relative to the home folder (see HiddenPaths()). A path that does
// not exist is skipped. `sandbox.reveal` takes entries out of this list and
// `sandbox.hide` adds your own, e.g. sandbox.hide=~/.config/mycloud.
//
// Deliberately not listed: ~/.ssh and ~/.config/gh. Git over ssh and the
// GitHub CLI are expected agent work, and both need their login.
inline constexpr std::string_view kHiddenCredentialStores[] = {
    ".aws",                // AWS CLI and SDK credentials and config
    ".config/gcloud",      // Google Cloud CLI credentials and tokens
    ".azure",              // Azure CLI tokens
    ".kube",               // kubectl contexts and cluster credentials
    ".gnupg",              // GnuPG private keys
    ".netrc",              // curl, git and ftp passwords
    ".docker/config.json"  // Docker registry logins
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_DEFAULTS_H_
