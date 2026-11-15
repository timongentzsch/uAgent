// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SANDBOX_H_
#define UAGENT_INCLUDE_CORE_SANDBOX_H_
// Pure policy composition for the OS sandbox; nothing here touches the host.
// Paths in and out are canonical: seatbelt matches the resolved path only.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

struct SandboxPolicy {
  // Deduplicated and sorted, with any root nested inside another folded away.
  std::vector<std::string> writable_roots;
  // Carved back out of a writable root. Seatbelt only: Landlock cannot deny.
  std::vector<std::string> denied_writes;
  // No write may reach these, except through a root nested inside them.
  std::vector<std::string> withheld;
  // HiddenPaths(): never read, written or connected to.
  std::vector<std::string> hidden;
  // Home-folder entries no command may read, and the folders in them it may.
  std::vector<std::string> unreadable;
  std::vector<std::string> readable;
  bool allow_network = true;
  // The readonly level: commands may write only temporary files.
  bool read_only = false;
};

// What a policy is composed from, supplied by the caller.
struct SandboxInputs {
  std::string workspace;
  // ~/.uagent. Never writable, and neither is any ancestor of it.
  std::string global_base;
  std::string tmpdir;  // $TMPDIR; empty when unset
  // macOS's per-user temp folder, which its tools use whatever $TMPDIR says.
  std::string user_tmpdir;
  // Package-manager caches (kPackageCaches in include/core/defaults.h).
  std::vector<std::string> tool_caches;
  // ~/.uagent/terminals/logs: the one exception inside global_base.
  std::string terminal_logs;
  std::vector<std::string> extra_roots;  // UAGENT_SANDBOX_WRITE, one per entry
  // The home folder when the relaxed level makes it writable, else empty.
  std::string open_home;
  bool allow_network = true;
  // The readonly level: only temporary roots and the terminal logs are offered.
  bool read_only = false;
  // Folders closed to reads at a level that confines them (readonly, default).
  std::vector<std::string> closed_folders;
  // What stays readable inside them besides the folder and the writable roots.
  std::vector<std::string> read_roots;
  // A linked worktree's git directories: the writes a commit there needs.
  std::vector<std::string> git_roots;
  // The folder is a repository, so its config and hooks must not be rewritten.
  bool git_repository = false;
};

// The `sandbox.level` setting, before the individual overrides apply. Not
// SandboxLevel, which is what the host can enforce.
enum class SandboxPreset { kReadOnly, kDefault, kRelaxed, kOff };

// False for anything but readonly, default, relaxed and off; *preset is kept.
bool ParseSandboxPreset(std::string_view text, SandboxPreset* preset);

// The level under yolo: default is raised to relaxed, the others stay.
SandboxPreset YoloSandboxPreset(SandboxPreset configured);

struct SandboxPolicyResult {
  SandboxPolicy policy;
  // Roots the policy rules dropped, as they were offered.
  std::vector<std::string> rejected;
};

SandboxPolicyResult BuildSandboxPolicy(const SandboxInputs& inputs);

// A larger profile is refused: truncating it would weaken it.
inline constexpr size_t kSeatbeltProfileLimit = size_t{64} * 1024;

// SBPL for `sandbox-exec -p`; empty when the policy exceeds the limit.
std::string SeatbeltProfile(const SandboxPolicy& policy);

// The policy as one JSON object, for a child agent and the Linux trampoline.
json SandboxPolicyJson(const SandboxPolicy& policy);
// False leaves *policy unspecified; the trampoline treats that as fatal.
bool SandboxPolicyFromJson(const json& value, SandboxPolicy* policy);

// Whether `path` is `root` or beneath it; textual, so both must be canonical.
bool SandboxPathWithin(std::string_view path, std::string_view root);

// Whether `path` is a writable root or lies beneath one.
bool SandboxWritable(const SandboxPolicy& policy, std::string_view path);

// Whether `path` is in none of policy.unreadable, or in one of policy.readable.
bool SandboxReadable(const SandboxPolicy& policy, std::string_view path);

// What a confined command may write, in words, for whoever meets the limit.
std::string SandboxWritesNote(const SandboxPolicy& policy,
                              std::string_view workspace);

// What this host can enforce. Landlock ABI 1-3 confines writes, not the
// network.
enum class SandboxLevel { kUnavailable, kFilesystem, kFilesystemAndNetwork };

// Probes the host once and caches the answer.
SandboxLevel SandboxSupported();

// Linux trampoline: argv is the program, --sandbox-child, the policy JSON, `--`
// and the command. Every failure returns without executing the command.
int SandboxChildMain(int argc, char** argv);

// kDegraded and kRefused are both a host that cannot enforce: a setting nobody
// touched degrades loudly and keeps working, one somebody wrote down refuses.
enum class SandboxMode { kOff, kEnforced, kDegraded, kRefused };

struct SandboxStatus {
  SandboxMode mode = SandboxMode::kOff;
  SandboxLevel level = SandboxLevel::kUnavailable;
  SandboxPolicy policy;
  // Roots BuildSandboxPolicy refused.
  std::vector<std::string> rejected;
  // Why the mode is kDegraded or kRefused. Empty otherwise.
  std::string reason;
  // The sandbox.level in effect, as /status names it: "relaxed (yolo)".
  std::string preset;
};

// The status for the approval mode now: a mode switched mid-session holds from
// the next command. No status is rebuilt, so the reference stays valid.
const SandboxStatus& SandboxRuntime();

// That status as UAGENT_INTERNAL_SANDBOX's value, which a child runs under.
std::string SandboxInheritance();

// How this session confines its commands: mode, mechanism, roots, refusals.
json SandboxDiagnosticJson();

// SandboxWritesNote for `status` and this session's folder.
std::string SandboxNote(const SandboxStatus& status);

// SandboxNote when this session's commands cannot write its own folder.
std::string SandboxWorkspaceRefusal();

// The argv prefix for `<shell> -c <command>`; empty when nothing is enforced.
std::vector<std::string> SandboxWrapperArgv(const SandboxStatus& status);

// What sandboxed commands cannot read and the file tools refuse, canonical and
// existing; sandbox.hide adds to it and sandbox.reveal takes from it.
std::vector<std::string> HiddenPaths();

// The denied_writes this host cannot deny (all under Landlock, none under
// Seatbelt), which a caller guards with a SandboxWatch instead.
std::vector<std::string> SandboxUnenforcedWrites(const SandboxStatus& status);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SANDBOX_H_
