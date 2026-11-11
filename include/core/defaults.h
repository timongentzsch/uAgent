// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_DEFAULTS_H_
#define UAGENT_INCLUDE_CORE_DEFAULTS_H_
// Built-in names and lists; numbers live in limits.h, user settings in
// config_registry.h. Keep this header dependency free: it is included widely.

#include <string_view>

namespace uagent {

// Credential stores under the home folder that sandboxed commands cannot read
// and the file tools refuse; `sandbox.reveal` and `sandbox.hide` adjust them.
// ~/.ssh and ~/.config/gh stay out: git over ssh and gh need their login.
inline constexpr std::string_view kHiddenCredentialStores[] = {
    ".aws",                // AWS CLI and SDK credentials and config
    ".config/gcloud",      // Google Cloud CLI credentials and tokens
    ".azure",              // Azure CLI tokens
    ".kube",               // kubectl contexts and cluster credentials
    ".gnupg",              // GnuPG private keys
    ".netrc",              // curl, git and ftp passwords
    ".git-credentials",    // git's plain-text credential store
    ".docker/config.json"  // Docker registry logins
};

// Package-manager caches under the home folder that sandboxed commands may
// write. Of the data directory only uv's part: the rest holds keyrings.
inline constexpr std::string_view kPackageCaches[] = {
    ".cache",           // XDG cache: pip, uv, Go, Playwright and most others
    ".local/share/uv",  // uv's tools and Python installs
    ".npm",             // npm and npx, which start most stdio MCP servers
};
// On macOS, the native cache folder as well (uv keeps using ~/.cache there).
inline constexpr std::string_view kMacPackageCaches[] = {"Library/Caches"};
// Home-folder entries commands still read at a level that confines reads.
inline constexpr std::string_view kReadableHomePaths[] = {
    "Library/Caches", "Library/Developer", "Library/Python"};

// How a tool result's hint opens for the model, for the tests that pin it.
inline constexpr char kDetachedMarker[] = "[detached] pid ";
inline constexpr char kRunningActivityMarker[] = "[running] activity ";
// Appended by ArtifactHint().
inline constexpr char kCapturedLogMarker[] = "\n[captured log: ";
// A skill body: the skill tool's result and what `$skill` pushed.
inline constexpr char kSkillMarker[] = "[skill ";
inline constexpr char kExplicitSkillMarker[] = "[explicit skill instructions";

// Variables that never reach a child process, matched without regard to
// case; so is a name that holds a marker or looks like a credential.
inline constexpr std::string_view kHiddenEnvNames[] = {
    "uagent_providers", "uagent_session_budget", "uagent_internal_usage_file",
    "ssh_auth_sock",  // the agent that holds your ssh keys
    "xauthority"};    // the X display's login cookie
inline constexpr std::string_view kHiddenEnvMarkers[] = {
    "_access_key", "_private_key", "_cookie"};

inline constexpr char kSkillFile[] = "SKILL.md";
inline constexpr char kMcpConfigFile[] = ".mcp.json";

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_DEFAULTS_H_
