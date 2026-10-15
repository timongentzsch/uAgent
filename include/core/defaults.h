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

// Package-manager caches sandboxed commands and MCP servers may write,
// relative to the home folder. They are per-user, so they sit outside every
// other root; a missing one is created so the tool can fill it. Of the data
// directory only uv's part: the rest holds keyrings, autostart entries and
// other programs' state. Another cache is granted with `sandbox.write`.
inline constexpr std::string_view kPackageCaches[] = {
    ".cache",           // XDG cache: pip, uv, Go, Playwright and most others
    ".local/share/uv",  // uv's tools and Python installs
    ".npm",             // npm and npx, which start most stdio MCP servers
};
// On macOS, the native cache folder as well (uv keeps using ~/.cache there).
inline constexpr std::string_view kMacPackageCaches[] = {"Library/Caches"};

// Markers a tool result carries for the model, which the transcript view,
// compaction and the memory extractor later find by their opening. Each is
// written in one place and read in another; one constant keeps the two from
// drifting, and the tests pin that the reader accepts what the writer emits.
inline constexpr char kDetachedMarker[] = "[detached] pid ";
inline constexpr char kStartedSubagentMarker[] = "[started] subagent id ";
inline constexpr char kRunningActivityMarker[] = "[running] activity ";
// Appended by ArtifactHint(); StripArtifactHint() removes it.
inline constexpr char kCapturedLogMarker[] = "\n[captured log: ";
// The last line of a subagent's result (ChildAgentResumeNote).
inline constexpr char kCollaboratorMarker[] = "\n[collaborator ";
// A skill body: the skill tool's result and what `$skill` pushed.
inline constexpr char kSkillMarker[] = "[skill ";
inline constexpr char kExplicitSkillMarker[] = "[explicit skill instructions";

// File names uAgent reads or writes in more than one place.
inline constexpr char kSkillFile[] = "SKILL.md";
inline constexpr char kMcpConfigFile[] = ".mcp.json";
// A panel's instructions. COORDINATOR.md is what they were called before.
inline constexpr char kPanelInstructionFile[] = "PANEL.md";
inline constexpr char kLegacyPanelInstructionFile[] = "COORDINATOR.md";

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_DEFAULTS_H_
