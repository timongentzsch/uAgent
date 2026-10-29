// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SANDBOX_H_
#define UAGENT_INCLUDE_CORE_SANDBOX_H_
// Pure policy composition for the OS sandbox: which roots a command may write
// to, whether it may reach the network, and how that renders into the two
// enforcement mechanisms. Nothing here touches the host -- probing and
// enforcement live in sandbox_posix.cc -- so every rule below is testable on
// either platform.
//
// Paths in and out are canonical absolute paths. Canonicalisation is the
// caller's job because it is the part that reads the filesystem, and because
// seatbelt matches the resolved path only: a profile naming /tmp confines
// nothing on macOS, where /tmp is a symlink to /private/tmp.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

struct SandboxPolicy {
  // Deduplicated and sorted, with any root nested inside another folded away,
  // so the rendered profile is a function of the set rather than of the order
  // the roots happened to be discovered in.
  std::vector<std::string> writable_roots;
  // Paths carved back out of a writable root. Seatbelt only: Landlock grants
  // rights per path with no deny form, so a Linux policy that needs a hole has
  // to not grant the parent in the first place.
  std::vector<std::string> denied_writes;
  // Paths inside a writable root that no write may reach, except through a
  // root nested inside them: the relaxed level opens the home folder and
  // withholds ~/.uagent, yet still grants its terminal logs and a workspace
  // kept there. Seatbelt denies them; Landlock grants the root around them.
  std::vector<std::string> withheld;
  // HiddenPaths(), filled in when a wrapper is built: never read, written or
  // connected to.
  std::vector<std::string> hidden;
  // The home folder's visible entries (~/Software, ~/Documents, ...) that no
  // command may read, and the folders inside them it still may: the
  // folder, and what commands use there. Reads elsewhere stay open.
  std::vector<std::string> unreadable;
  std::vector<std::string> readable;
  bool allow_network = true;
  // The readonly level: commands may write only temporary files, so nothing
  // they do outlasts them. Said to the model when a write is refused.
  bool read_only = false;
};

// What a policy is composed from. Supplied by the caller rather than read from
// the environment here, so that composition stays pure and one test can cover
// host shapes the test machine does not have.
struct SandboxInputs {
  std::string workspace;
  // ~/.uagent. Never writable, and neither is any ancestor of it: that is what
  // keeps the config, the trust store and the detached-process records out of
  // reach of a command that can otherwise write freely.
  std::string global_base;
  std::string tmpdir;  // $TMPDIR; empty when unset
  // macOS's own per-user temp folder, where its tools cache (xcrun, behind
  // /usr/bin/git) whatever $TMPDIR says; empty elsewhere.
  std::string user_tmpdir;
  // Package-manager caches (kPackageCaches in include/core/defaults.h).
  std::vector<std::string> tool_caches;
  // ~/.uagent/terminals/logs -- the one deliberate exception inside
  // global_base, because a detached process's own log pump writes there.
  std::string terminal_logs;
  std::vector<std::string> extra_roots;  // UAGENT_SANDBOX_WRITE, one per entry
  // The home folder, when the relaxed level makes it writable; empty otherwise.
  // global_base stays withheld from it.
  std::string open_home;
  bool allow_network = true;
  // The readonly level: only the temporary roots and the terminal logs are
  // offered, whatever else is listed here.
  bool read_only = false;
  // The folders closed to reads at a level that confines them (readonly,
  // default), empty otherwise: the person's own -- the home folder's
  // entries not named with a dot and the workspaces in ~/.uagent/spaces --
  // and the folder's neighbours, as other folders in /workspaces are.
  std::vector<std::string> closed_folders;
  // What stays readable inside them besides the folder and the writable
  // roots: what the environment points at, sandbox.read and
  // kReadableHomePaths.
  std::vector<std::string> read_roots;
  // A linked git worktree's own git directory and the shared repository's
  // objects, refs and logs: the writes a commit there needs.
  std::vector<std::string> git_roots;
  // The folder already is a repository (its .git is a directory), so its
  // config and hooks exist and must not be rewritten; before `git init` they
  // cannot be denied without denying the init itself.
  bool git_repository = false;
};

// The `sandbox.level` setting: what a command may do before the individual
// overrides (sandbox.network, sandbox.write, sandbox.hide, sandbox.reveal)
// apply. Not SandboxLevel, which is what the host can enforce.
// readonly is plan mode's and a read-only agent's: its commands may look but
// change nothing, the folder and the network included, whatever the overrides
// say.
enum class SandboxPreset { kReadOnly, kDefault, kRelaxed, kOff };

// readonly, default, relaxed and off. False for anything else, leaving
// *preset alone.
bool ParseSandboxPreset(std::string_view text, SandboxPreset* preset);

// The level a session's commands run at while its approval mode is yolo:
// default is raised to relaxed; readonly, relaxed and off stay as they are.
SandboxPreset YoloSandboxPreset(SandboxPreset configured);

struct SandboxPolicyResult {
  SandboxPolicy policy;
  // Roots dropped by the rules above, as they were offered, so the startup
  // notice can name what was asked for and not granted.
  std::vector<std::string> rejected;
};

SandboxPolicyResult BuildSandboxPolicy(const SandboxInputs& inputs);

// A profile beyond this is refused rather than truncated: a truncated profile
// is a weaker profile, and it would arrive without saying so.
inline constexpr size_t kSeatbeltProfileLimit = size_t{64} * 1024;

// SBPL for `sandbox-exec -p`. Empty when the policy exceeds the limit.
//
// Seatbelt takes the last matching rule, so the order is load-bearing:
// (allow default) keeps reads and exec unrestricted, (deny file-write*) then
// removes every write, and the per-root allows and their deny-overrides add
// back exactly what the policy names.
std::string SeatbeltProfile(const SandboxPolicy& policy);

// The policy as one JSON object: how a child agent inherits it, and how the
// Linux trampoline receives it as a single argv word, so that no quoting rule
// sits between the parent and the trampoline.
json SandboxPolicyJson(const SandboxPolicy& policy);
// Reads what SandboxPolicyJson wrote. False leaves *policy unspecified; the
// trampoline treats that as fatal rather than as an empty policy, which would
// be no confinement at all.
bool SandboxPolicyFromJson(const json& value, SandboxPolicy* policy);

// True when `path` is `root` or lies beneath it. Both must be canonical and
// absolute; comparison is textual so that it cannot touch the filesystem.
bool SandboxPathWithin(std::string_view path, std::string_view root);

// True when a confined command may write `path`: it is a writable root or
// lies beneath one.
bool SandboxWritable(const SandboxPolicy& policy, std::string_view path);

// True when a confined command may read `path`: it lies in none of
// policy.unreadable, or in one of policy.readable.
bool SandboxReadable(const SandboxPolicy& policy, std::string_view path);

// What a confined command may write, in words, for a reader who met the limit
// as a bare "Permission denied": the model after a failed command, the person
// at startup. A folder the policy does not grant is named first, with what
// still works there, because every other sentence about the sandbox assumes
// the folder is writable; and with what does not, when it holds one of
// policy.hidden.
std::string SandboxWritesNote(const SandboxPolicy& policy,
                              std::string_view workspace);

// What this host can actually enforce. Filesystem-only is the Landlock ABI 1-3
// case: writes confine, the network toggle does not, so a policy that denies
// the network on such a host is refusing rather than pretending.
enum class SandboxLevel { kUnavailable, kFilesystem, kFilesystemAndNetwork };

// Probes the host once and caches the answer. Cheap either way -- one syscall
// on Linux, one access() on macOS -- but every spawn asks, and the answer
// cannot change while the process runs.
SandboxLevel SandboxSupported();

// Linux trampoline: the argv word after the program name is --sandbox-child,
// followed by SandboxPolicyJson as one word, then `--`, then the command to
// run. Applies the policy to itself and execs the command, so on
// success it never returns. Every failure -- a malformed argv, a rejected
// ruleset, a missing command -- returns without executing anything, because a
// command that ran here would be a command that ran unconfined.
int SandboxChildMain(int argc, char** argv);

// How this session's spawns are actually treated.
//
// kDegraded and kRefused are the same host -- one that cannot enforce -- split
// by who asked for the sandbox. A default that bricked every old kernel would
// be a bad default, so a setting nobody touched degrades loudly and keeps
// working; a setting somebody wrote down is a requirement, and quietly not
// meeting it would be the one outcome worse than refusing.
enum class SandboxMode { kOff, kEnforced, kDegraded, kRefused };

struct SandboxStatus {
  SandboxMode mode = SandboxMode::kOff;
  SandboxLevel level = SandboxLevel::kUnavailable;
  SandboxPolicy policy;
  // Roots BuildSandboxPolicy refused, so startup can name what was asked for
  // and not granted.
  std::vector<std::string> rejected;
  // Why the mode is kDegraded or kRefused. Empty otherwise.
  std::string reason;
  // The sandbox.level in effect, as /status names it: "default", or
  // "relaxed (yolo)" where yolo raised it, "readonly (plan)" in plan mode. A
  // child carries its parent's.
  std::string preset;
};

// Reads the configuration, composes the policy and probes the host once, and
// composes it once more the first time another approval mode asks: at the
// default level relaxed for yolo, and readonly for plan mode. Returns the one
// for the approval mode now: every spawn asks, so a mode switched mid-session
// holds from the next command. No status is ever rebuilt, so the reference
// stays good.
const SandboxStatus& SandboxRuntime();

// That status as the value of UAGENT_INTERNAL_SANDBOX, which makes a child
// agent run under exactly it, whatever its own approval mode.
std::string SandboxInheritance();

// One object describing how this session confines its commands: mode,
// mechanism, the writable roots, and anything asked for that was not granted.
// Shared by session_ready, /status and /context so all three cannot drift.
json SandboxDiagnosticJson();

// SandboxWritesNote for `status` and this session's folder. The hidden paths
// are counted on Linux only: seatbelt denies them by rule, so nothing goes
// stale.
std::string SandboxNote(const SandboxStatus& status);

// SandboxNote when this session's commands cannot write its own folder;
// empty otherwise. A home directory is the usual case: it holds ~/.uagent, so
// it is never granted.
std::string SandboxWorkspaceRefusal();

// The argv words to put in front of `<shell> -c <command>`, hiding
// HiddenPaths() too. Empty when the status does not enforce.
std::vector<std::string> SandboxWrapperArgv(const SandboxStatus& status);

// What sandboxed commands cannot read and the file tools refuse, canonical and
// existing: the browser profile, the web host's state, uAgent's settings, the
// cloud and gpg credential stores, ~/.netrc and Docker's login. sandbox.hide
// adds to that and sandbox.reveal takes from it. ~/.ssh and the GitHub CLI
// login stay readable: git over ssh and `gh` are agent work.
std::vector<std::string> HiddenPaths();

// The paths of status.policy.denied_writes that this host cannot deny, which a
// caller guards with a SandboxWatch instead: all of them where Landlock is the
// mechanism, none under Seatbelt.
std::vector<std::string> SandboxUnenforcedWrites(const SandboxStatus& status);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SANDBOX_H_
