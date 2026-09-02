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
  bool allow_network = true;
};

// What a policy is composed from. Supplied by the caller rather than read from
// the environment here, so that composition stays pure and one test can cover
// host shapes the test machine does not have.
struct SandboxInputs {
  std::string workspace;
  // ~/.uagent. Never writable, and neither is any ancestor of it: that is what
  // keeps the config, the trust store and the detached-job records out of
  // reach of a command that can otherwise write freely.
  std::string global_base;
  std::string tmpdir;  // $TMPDIR; empty when unset
  std::string cache_dir;
  std::string data_dir;  // ~/.local/share; empty where there is no such dir
  // ~/.uagent/terminals/logs -- the one deliberate exception inside
  // global_base, because a detached job's own log pump writes there.
  std::string terminal_logs;
  std::string extra_roots;  // raw UAGENT_SANDBOX_WRITE, colon-separated
  bool allow_network = true;
};

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

// Round-trip for the Linux self-reexec. The policy travels as separate argv
// words so that no quoting rule sits between the parent and the trampoline;
// denied_writes is not carried because Landlock cannot express it.
std::vector<std::string> EncodeSandboxPolicy(const SandboxPolicy& policy);
// Reads words written by EncodeSandboxPolicy. Reports how many it consumed so
// the caller can find the command that follows. False leaves *policy
// unspecified; the trampoline treats that as fatal rather than as an empty
// policy, which would be no confinement at all.
bool DecodeSandboxPolicy(const std::vector<std::string>& words,
                         SandboxPolicy* policy, size_t* consumed);

// True when `path` is `root` or lies beneath it. Both must be canonical and
// absolute; comparison is textual so that it cannot touch the filesystem.
bool SandboxPathWithin(std::string_view path, std::string_view root);

// What this host can actually enforce. Filesystem-only is the Landlock ABI 1-3
// case: writes confine, the network toggle does not, so a policy that denies
// the network on such a host is refusing rather than pretending.
enum class SandboxLevel { kUnavailable, kFilesystem, kFilesystemAndNetwork };

// Probes the host once and caches the answer. Cheap either way -- one syscall
// on Linux, one access() on macOS -- but every spawn asks, and the answer
// cannot change while the process runs.
SandboxLevel SandboxSupported();

// Linux trampoline: the argv word after the program name is --sandbox-child,
// followed by the words EncodeSandboxPolicy produced, then `--`, then the
// command to run. Applies the policy to itself and execs the command, so on
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
};

// Reads the configuration, composes the policy and probes the host once. Every
// spawn asks; none of the answers can change while the process runs.
const SandboxStatus& SandboxRuntime();

// The argv words to put in front of `<shell> -c <command>`. Empty when the
// status does not enforce, which is what makes an unsandboxed spawn identical
// to the one this release already ships.
std::vector<std::string> SandboxWrapperArgv(const SandboxStatus& status);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SANDBOX_H_
