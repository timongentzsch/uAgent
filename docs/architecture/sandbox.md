# Sandbox

This page explains how `run` commands and stdio MCP servers are confined:
how a policy is composed, how Seatbelt and Landlock enforce it, and what
each cannot express. What a user may rely on is stated in
[Security](../../SECURITY.md#sandboxing); who is asked before a call runs is
in [Tools](tools.md).

## Responsibility

`src/core/sandbox.cc` composes a `SandboxPolicy` from plain inputs and
renders it; it never touches the host. `src/core/sandbox_posix.cc` collects
the inputs, probes what the host can enforce, picks the policy for the
current approval mode and holds the Linux trampoline. The tool executor
(`SandboxWrapperFor` in `src/tools/shell.cc`) and the MCP launcher
(`src/mcp/register.cc`) prepend the wrapper argv to every spawn. The file
tools are not confined by the OS; they apply the same hidden and closed
paths in their own checks.

## Processes and threads

- macOS: the command runs as `/usr/bin/sandbox-exec -p <profile> <shell> -c
  <command>`. The profile is SBPL text built per spawn.
- Linux: the command runs as `<self> --sandbox-child <policy JSON> --
  <shell> -c <command>`. `<self>` is `/proc/self/exe`, so the running inode
  is re-executed even after an upgrade replaced the file. `src/main.cc`
  dispatches `--sandbox-child` before any other startup;
  `SandboxChildMain` applies Landlock, sets `PR_SET_NO_NEW_PRIVS` and
  `execvp`s the command. Any failure exits 125 without executing it.
- An agent's own session runtime runs at `readonly` (`edits` none) or
  `default`.
- In a remote workspace the execution worker supplies the status, cached
  per approval mode; an unreadable answer refuses every command.

## Data and formats

`SandboxPolicy` (`include/core/sandbox.h`) holds canonical paths:
`writable_roots`, `denied_writes`, `withheld`, `hidden`, `unreadable`,
`readable`, plus `allow_network` and `read_only`. Seatbelt matches resolved
paths only, so every input goes through `CanonicalAccessPath` first.
`SandboxPolicyJson` is the wire form for the trampoline and for a child.

`BuildSandboxPolicy` composes it from `SandboxInputs` (defaults in
`include/core/defaults.h`, settings in [Settings](../interfaces/settings.md)):

| Level (`sandbox.level`) | Writable roots | Closed reads | Network |
| --- | --- | --- | --- |
| `readonly` | `$TMPDIR`, the macOS user temp folder, `/tmp`, `/var/tmp`, `/dev`, terminal logs | yes | denied |
| `default` | the above, the folder, `kPackageCaches` (and `kMacPackageCaches`), `sandbox.write`, a linked worktree's git directories | yes | `sandbox.network` |
| `relaxed` | the above and the home folder, with `~/.uagent` withheld | no | `sandbox.network` |
| `off` | no wrapper | no | open |

- A root is rejected (and reported at startup) when it is `/`, an ancestor
  of `~/.uagent`, or inside `~/.uagent/config`. Nested roots fold into
  their parent unless they lie inside a withheld path.
- `denied_writes` carves `.mcp.json` and, in a repository, `.git/config`
  and `.git/hooks` back out of the folder.
- Closed reads (`CollectReads`): the non-dot entries of the home folder, of
  `~/.uagent/spaces`, and of the folder's parent. Inside them stay
  readable: the folder, the writable roots, the skill folders,
  `kReadableHomePaths`, `sandbox.read`, every directory an environment
  variable names (a `bin` brings its parent unless that is the home
  folder), `~/go` when `GOPATH` is unset, and a linked worktree's git
  directories.
- `HiddenPaths()` is added at each spawn: the browser data directory,
  `~/.uagent/web`, `~/.uagent/config`, `kHiddenCredentialStores`, and for
  an execution worker its socket, owner lease and the account's
  `kWorkspaceDir`; plus `sandbox.hide`, minus `sandbox.reveal`. Only paths
  that exist now are listed, so a store created mid-session is hidden from
  the next command.

## Protocol

1. `SandboxRuntime()` returns one of three statuses, each built once on
   first use: the configured one, `RaisedStatus` for yolo (`default`
   becomes `relaxed`, preset text `relaxed (yolo)`), and `ReadOnlyStatus`
   for plan mode (`read_only`, no network, `readonly (plan)`). Explicit
   `readonly`, `relaxed` and `off` are not raised. A mode change applies
   from the next command, which reads `CurrentApprovalMode()`.
2. `SandboxWrapperFor` refuses when the status is `kRefused`, or when the
   wrapper comes back empty because the policy is too large. Otherwise it
   returns the argv; `kOff` and `kDegraded` spawn unwrapped.
3. Seatbelt: `(allow default)`, `(deny file-write*)`, one `allow` per root,
   `deny` for withheld and denied paths, `(deny network*)` when the network
   is off, `deny file-read-data` for closed folders with `allow` for the
   open ones inside, and last the hidden paths: `deny file-read*
   file-write*`, `deny network-outbound` to unix sockets under them, and
   `deny file-write-unlink` on every ancestor so none can be renamed away.
4. Landlock: the ruleset handles write rights (plus `REFER` from ABI 2 and
   `TRUNCATE` from ABI 3); `READ_FILE`, and `RESOLVE_UNIX` from ABI 9, are
   handled only when something is hidden or closed. Since Landlock can only
   grant, `AddRulesExcept` grants a root child by child around each hidden
   or withheld path. With the network off, `BIND_TCP` and `CONNECT_TCP` are
   handled and never granted.
5. On Linux, `SandboxUnenforcedWrites` returns the `denied_writes`; a
   `SandboxWatch` records them before the command and `Restore` puts them
   back when it ends, adding `restored <path>: sandboxed commands may not
   change it` to the result.

`run(sandbox=false)` skips the wrapper for one command. Its approval class
is `kMandatoryHuman` while the sandbox is not `off`, so yolo, a remembered
rule and a headless or delegated run all end in a denial. The argument is
offered only when there is a sandbox to leave.

`run(root=true)` is offered only to an interactive top-level session
(`BuildTools`). `RootCommand` wraps the command as `sudo -k -S -p <token>
-- bash -o pipefail -c …`, or through `ssh -o BatchMode=yes` with `host`.
A local root command runs unwrapped, since sudo cannot gain privileges
under the sandbox. When `sudo -n true` fails, the person is asked for the
password once per turn per host; the token is `RandomToken(16)`.
`AnswerRootPrompt` strips the token from output, writes the password on
its first appearance, closes stdin of a non-terminal command, and on a
second appearance wipes the password and marks it rejected. Output
containing a password of 4 or more characters shows `***`. Passwords are
wiped at turn end. Where `root` is offered, other commands get a
`BASH_FUNC_sudo%%` that fails and names `root=true`.

## Invariants and limits

- A session told to confine never spawns unconfined: every refusal returns
  before the spawn, and the trampoline exits 125 on any failed step.
- `kSeatbeltProfileLimit` (64 KiB) and `kSandboxPolicyArgBytes` (96 KiB,
  [Limits](../interfaces/limits.md)) refuse a policy that would otherwise be
  truncated.
- `SandboxWatch` guards at most 4 MiB and 1,000 entries per path; a larger
  path is not guarded.

## Failure modes

- A `sandbox.level` that is no level, or one the settings reader dropped:
  `kRefused`, every command refused. It never falls back to `default`.
- No `sandbox-exec`, or no Landlock (kernel before 5.13): `kDegraded` with a
  `capability.changed` event and a warning, commands unconfined; `kRefused`
  instead when `sandbox.level` was written down, in the environment or a
  saved setting.
- Network denied on Landlock below ABI 4: `kRefused`, so plan mode there
  runs no commands. Plan mode runs a command only under the enforced
  `readonly` status (`PlanModeAllows`); at `off` it refuses every command.

What each mechanism cannot express:

- Landlock has no deny rule: `.mcp.json`, `.git/config` and `.git/hooks`
  are restored after the command, not protected during it. A detached
  activity still running when the session ends is not checked.
- Landlock grants around hidden paths by the entries that exist at spawn.
  A running command cannot read a file created later directly in a folder
  on the way to a hidden path, nor write a new file directly in the home
  folder at `relaxed`.
- Landlock restricts TCP bind and connect only; UDP and other traffic stay
  open. Unix sockets under hidden paths are blocked only from ABI 9.
- Landlock lists names in a closed folder but refuses its files; Seatbelt
  refuses the listing and still resolves paths through it.
- Keychains are not hidden: access goes through `securityd`, not files.
- An MCP server keeps the status it was spawned with, and its reads are
  never closed (`register.cc` clears `unreadable`).

## Source and tests

| Path | What |
| --- | --- |
| `include/core/sandbox.h`, `src/core/sandbox.cc` | policy, levels, Seatbelt profile, JSON form |
| `src/core/sandbox_posix.cc` | inputs, host probe, per-mode status, Landlock, `SandboxChildMain` |
| `include/core/sandbox_watch.h`, `src/core/sandbox_watch.cc` | record and restore on Linux |
| `include/core/defaults.h` | hidden stores, package caches, readable home paths |
| `src/tools/shell.cc` | `SandboxWrapperFor`, `RootCommand`, `PasswordlessSudo` |
| `src/tools/registry_exec.cc` | `run` arguments and approval class |
| `src/agent/activity_supervisor.cc` | `AnswerRootPrompt` |
| `src/app/approvals.cc`, `src/app/session_worker.cc` | plan mode, an agent's level |
| `tests/unit/sandbox_test.cc` | composition, profile, notes, watch |
| `tests/integration_sandbox.py` | real commands under each level, yolo, sudo, refusals |
