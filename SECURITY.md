# Security

µAgent is a local, single-user coding agent. It confines the commands it runs
(see [Sandboxing](#sandboxing)) but is not a container: approval grants the
current user's permissions. Below the `relaxed` level, commands cannot read
your other folders or the [hidden stores](#sandboxing). It sends prompts,
selected files, tool results and attachments to the configured endpoint;
treat that endpoint as trusted infrastructure. Use a container, VM or
restricted account for untrusted code.

![What each part can reach: paired devices reach the web host; only the session runtime holds the model key and talks to the endpoint; commands and MCP servers run in the OS sandbox, write only the folder, temporary files and caches, and cannot reach your other folders, ~/.uagent, credential stores, the browser profile or secrets in the environment; the runtime acts in Chrome but gets no field values back](docs/images/boundaries.svg)

## What the agent may do without asking

The approval mode decides who is asked; the sandbox decides what a command
can reach. No approval mode turns the sandbox off, but yolo raises the
`default` level to `relaxed` and plan mode lowers it to `readonly` (see
[Sandboxing](#sandboxing)).

| Mode | Set with | Effect |
| --- | --- | --- |
| `ask` (default) | `/permissions ask` | a person approves each mutating, process, network, cost-bearing or untrusted MCP call, and each read outside the folder |
| `auto` | `/permissions auto`, `UAGENT_APPROVAL=auto` | a decision model allows or denies the calls the sandbox contains; what it cannot decide, every call that leaves the sandbox or the folder, and every `run` or `scratch` call while the sandbox is off or not enforced go to a person, and are denied when nobody can answer |
| `yolo` | `--yolo`, `/yolo`, `/permissions yolo` | those calls run unasked; commands stay sandboxed, at the `relaxed` level where the level is `default` |
| `plan` | `/plan`, `/permissions plan` | only reads run: commands at the `readonly` level and calls on the conversation's plan file; every other change is refused, and reads outside the folder ask as in `ask` |

In every mode:

- Reads inside the folder need no approval, apart from the files named
  below.
- A call a rule you kept covers is not asked again: a rule for the session
  or for the repository names a tool and what it covers (a simple command's
  prefix, a folder, a host, or the exact call). A command that chains,
  substitutes or redirects matches no prefix, and shells, interpreters,
  `sudo` and `rm` are only ever allowed exactly. `/permissions rules` lists
  the rules and `/permissions forget N` removes one.
- Some operations always require a person, even under yolo, auto or a
  remembered rule, and are denied when no interactive client can answer, as
  in a delegated child:
  - writing µAgent's configuration through the file tools: the saved
    settings, the trust store, `~/.uagent/config/permissions.json`, your
    instruction files in `~/.uagent`, and any `.mcp.json`;
  - reading any of those through the file tools, except the trust store and
    the instruction files, which hold no credential;
  - reading, through the file tools, one of your folders that the sandbox
    closes to commands (see **Reads** below);
  - the `uagent_configure` and `uagent_instructions` tools;
  - `run(sandbox=false)`.

An **agent**, a conversation another one started, acts for you unattended.
It takes its parent's approval mode, auto at most, unless a mode is chosen
for its own conversation (`/permissions`, `--yolo`, or the web's approval
control); the saved setting does not reach it. One started with `edits` none
runs at the `readonly` level with no tool that edits, whatever its settings
say, and so do the agents it starts. Any other runs at the `default` level,
or `relaxed` while it is in yolo. While that sandbox is enforced and it is
in auto, its `run` and `scratch` commands and its file changes inside the
folder run without review; everything else goes through its approval mode.
What would ask a person goes to its parent first, which decides or passes it
to you, and to you after five minutes. Once you write to the agent yourself,
what its turn still asks comes to you directly: in a turn your message
started, or one you wrote into while it ran. What always needs a person
never goes to the parent.

## Trust boundaries

- Settings are never read from the folder, and `.env` files in it
  are ignored. Saved settings live in `~/.uagent/config/settings.json`
  ([Operations](docs/OPERATIONS.md#settings) has the scopes and their order).
  That file and artifact files are forced private.
- Folder trust is about `.mcp.json` alone. User `~/.mcp.json` is trusted
  executable configuration. The folder's `.mcp.json` requires interactive trust or
  `--trust-project-config`; semantic edits revoke stored trust.
- Folder instruction files, memories and skills enter model context without
  configuration trust because they grant no capability. An explicit
  `$skill-name` mention loads that skill before the first model call. Treat
  an untrusted checkout as prompt input and review requested actions before
  approving them.
- Only structured provider tool calls are executed. Text that resembles
  another harness's call syntax is recognized only so the malformed response
  can be suppressed and retried; it is never translated or executed.
- Tool, file, web, memory, MCP and summary text is evidence: it cannot expand
  user-approved scope. Schema validation, capability policy, path checks and
  approval enforce the boundary; the system prompt reinforces it. µAgent does
  not semantically detect every prompt injection.
- An MCP call skips approval only when its server is trusted and marks the
  tool read-only. MCP servers run under the command sandbox unless their
  entry sets `"sandbox": false`.
- Auto mode sends the current request, the tool name, the folder and a
  bounded action preview to the decision model of an endpoint that declares
  `decisions` (OpenRouter by default). It asks for providers that retain no
  data and identifies the conversation by a hash. It allows only above the
  model's allow bar with no risk question above its risk bar. It never judges
  a call outside the sandbox or the folder, or a command while the sandbox is
  off, degraded or refused; without such an endpoint it asks.
- Paths are canonicalized to reduce symlink escapes, and path restrictions
  are checked again when the call runs. Writes are atomic.
- Requests, responses, attachments, tool output, scans, activities, MCP data and
  logs are bounded. MCP server stderr goes to a rotating, size-bounded log.
- API redirects are rejected and bearer-auth transfers use HTTP(S) only.
- `web_fetch` makes credential-free direct HTTP(S) connections only to public
  IPv4 or IPv6 destinations. The resolved address is checked before every
  connection, including redirects; proxy variables are ignored so they cannot
  bypass that check.
- Shell commands run in managed process groups. Child processes
  receive a centralized secret-deny environment; only class-specific
  credentials are re-added. Approved `run` commands can opt in exact
  variables with `UAGENT_SHELL_ENV_ALLOW`; the allowlist never applies to MCP
  servers, delegated agents or `scratch`. `UAGENT_SANDBOX_HIDE_ENV` adds
  names that no child inherits, whatever the allowlist says.
- A `run(tty=true)` activity keeps a writable PTY for the harness lifetime.
  `activity(operation=write, chars=...)` sends raw bytes with the original
  process's permissions, so treat every write, interrupt and resize as
  process control. Input to non-TTY activities is rejected. Detached
  persistent activities keep logs but no reattachable stdin.
- Automatic memory extraction processes at most one idle saved session. Its
  child receives the configured model credential and only the memory tool.
  Transcript text and event previews are redacted for known credential forms
  before model requests and memory writes. Codex and Claude memories are
  read-only, untrusted evidence. Redaction is defense in depth, not a
  guarantee that every secret is recognized: it finds keyword assignments,
  bearer tokens, private keys and known vendor token prefixes, not a bare
  token or a password named in prose. A conversation run with `--no-memory`
  is never extracted.
- Model, MCP and tool text is terminal-sanitized.
- The web host's pairing, device authentication, Origin checks and file
  serving are described in [Web interface](docs/WEB.md#security-model).
- The web host's terminal gives a paired device your own shell, unsandboxed:
  pairing is the boundary, and `UAGENT_WEB_TERMINAL=off` removes it. The
  shell gets the same secret-deny environment as the agent's commands, so
  "Add to chat" cannot carry µAgent's credentials into a model's context,
  and the agent never reads a terminal.

## Browser automation

The agent drives a browser in one of two ways.

**The shared Chrome of the web host** (the `browser` tool, available while
`UAGENT_BROWSER_DATA` is set and the web host runs it). You and the agent see
and drive the same browser, and its profile keeps your saved logins and
cookies.

- Actions that change the page need approval like any mutating call.
- The agent can use a saved login without seeing it: `fill_saved` has Chrome
  fill the one it offers for the focused field and does not read what was
  filled. The page text the agent gets says whether a field holds something,
  never what, the request list it reads has no bodies, and it cannot type
  into a password field. Screenshots draw password fields as dots;
  [the browser guide](docs/BROWSER.md#isolation) lists the fields that are
  not masked.
- With the sandbox on, commands cannot read the profile or connect to its
  sockets, and the file tools refuse it in every mode. The
  [managed policy](deploy/chrome-policy.json) blocks Chrome's password pages
  and `passwords.google.com`, where a saved password could be shown.
- Whatever a page shows after sign-in can enter model context. Use a profile
  that holds only the accounts the agent should reach.

**Playwright** (`@playwright/cli` through an ordinary approved `run`
command). Pin its npm version when reproducible or offline execution
matters.

- Playwright's isolated mode uses a separate profile.
- CDP attach can inspect and control authenticated tabs after Chrome's
  remote-debugging approval; close sensitive tabs or use isolated mode when
  that access is unnecessary.
- Playwright snapshots report form field values verbatim, including
  autofilled passwords, card numbers and one-time codes. µAgent's redaction
  covers its own transcripts, not page content a snapshot pulls into context.

## Sandboxing

The OS confines the commands the agent runs (`run`, `scratch`) and the stdio
MCP servers it starts: `sandbox-exec` (Seatbelt) on macOS, Landlock on
Linux. The sandbox is on by default, in every approval mode.

One setting chooses how far it goes, `sandbox.level` in the settings file or
`UAGENT_SANDBOX` in the environment:

| Level | Writes | Reads | Network | Hidden stores |
| --- | --- | --- | --- | --- |
| `readonly` | temporary files and the terminal logs only, never the folder | not your other folders | none | yes |
| `default` | the roots below | not your other folders | allowed | yes |
| `relaxed` | the roots below and your home folder, except `~/.uagent` and the hidden stores | open | allowed | yes |
| `off` | unconfined | open | allowed | no |

**Yolo raises `default` to `relaxed`.** While a session's approval mode is
yolo and its level is `default`, its commands run at `relaxed`: they read
anywhere and write the home folder, except `~/.uagent` and the hidden
stores. The browser profile, µAgent's settings and the credential stores
stay out of reach. The change holds from the next command after `/yolo`,
`/permissions` or the web's control, and `/status` names the level in
effect (`relaxed (yolo)`). `readonly`, `relaxed` and `off`, set explicitly,
stay as they are. Plan mode runs commands at `readonly (plan)` whatever the
level, except `off`, where it runs none.

A value that is no level (`strict` included, which is gone) is not read as
`default`: commands are refused until it is fixed, so a typo cannot open the
network a stricter level would have closed.

Five more settings each adjust one thing and win over the level:

| To | Setting | Environment |
| --- | --- | --- |
| allow or deny the network whatever the level | `sandbox.network` | `UAGENT_SANDBOX_NET=0` or `1` |
| let commands write another folder | `sandbox.write` | `UAGENT_SANDBOX_WRITE=/path/a:/path/b` |
| let commands and the file tools read another of your folders | `sandbox.read` | `UAGENT_SANDBOX_READ=~/Software/other` |
| hide more paths from commands and the file tools | `sandbox.hide` | `UAGENT_SANDBOX_HIDE=~/.config/mycloud:/path/b` |
| show a path that is hidden by default | `sandbox.reveal` | `UAGENT_SANDBOX_REVEAL=~/.aws` |

Paths in `sandbox.read`, `sandbox.hide` and `sandbox.reveal` are absolute or start with
`~/`. All of these settings take effect at the next start of the
conversation (`/restart`). `/status` names the mechanism and `/context`
lists every writable root. The agent can call `run(sandbox=false)` for one
command, which a person must approve. `--yolo` stops the questions and
raises `default` to `relaxed`; it never turns the sandbox off.

**Writes** are what the sandbox restricts. A confined command may write:

- the folder;
- `$TMPDIR`, `/tmp`, `/var/tmp` and `/dev`;
- the package caches: `~/.cache`, `~/.local/share/uv`, `~/.npm` and, on
  macOS, `~/Library/Caches` (`kPackageCaches` and `kMacPackageCaches` in
  `include/core/defaults.h`; grant another with `sandbox.write`);
- `~/.uagent/terminals/logs`, where a detached activity's log pump writes;
- the roots `sandbox.write` adds;
- in a linked git worktree, the worktree's own git directory and the shared
  repository's `objects`, `refs` and `logs`, so a commit works; the shared
  `config` and `hooks` stay unwritable on macOS, and on Linux are only as
  protected as the folder's own `.git/config` is;
- at the `relaxed` level, your home folder.

Everything else is refused. `~/.uagent` is never granted, and a requested
root is rejected, and reported at startup, when it is `/`, `~/.uagent` or
one of its ancestors, or lies inside `~/.uagent/config`. That keeps the
saved settings, trust store, collaborator records and detached-activity records
out of reach. The `relaxed` level opens the home folder around `~/.uagent`:
its terminal logs, and a workspace under `~/.uagent/spaces`, stay writable;
nothing else in it is. On Linux, Landlock can only grant, so the home folder
is granted entry by entry as it is when a command starts: a new file made
directly in the home folder is not writable, while files in its folders and
existing files are. The startup note says so.

The folder is held to the same rule. A session started in your home
directory, which holds `~/.uagent`, has no writable folder below the
`relaxed` level: its commands can write only the other roots above, while
the file tools still work there. Startup says so, the model is told, and a
refused write names the roots. Start in a folder instead, or in a
workspace: the web's **New → Workspace…** takes a name and keeps a folder
for it at `~/.uagent/spaces/<name>`, which is granted like any other
folder.

**Reads** are closed in your own folders, below the `relaxed` level: the
visible entries of the home folder (`~/Software`, `~/Documents`, `~/Desktop`,
`~/Library`…), the workspaces in `~/.uagent/spaces`, and the folder's
neighbours, the other entries of the folder that holds it, as other
folders in the container's `/workspaces` are. A neighbour that holds the
home folder is closed only by the home folder's own entries. A model that climbs out of its folder into a sibling
repository answers from code that is not the one it was asked about, and
your documents are not its business. Inside those folders a command still
reads:

- the folder;
- the writable roots above;
- `~/Library/Caches`, `~/Library/Developer` and `~/Library/Python`
  (`kReadableHomePaths` in `include/core/defaults.h`);
- the folders its environment points at: every `PATH` entry, the parent of
  one named `bin` (the `bin` itself when that parent is the home folder), and
  any variable that names a folder (`GOPATH`, `ANDROID_HOME`…), so a
  toolchain installed in `~/go` or `~/fvm` keeps working; `~/go` is open
  while `GOPATH` is unset;
- a linked git worktree's git directory and the repository's shared one,
  when the repository itself is in a closed folder (only those directories,
  not the main checkout), after checking that they belong to each other;
- the skill folders;
- the folders `sandbox.read` adds.

Everything else stays readable: dot-folders such as `~/.config` and
`~/.gitconfig`, which tools need, and everything outside the home folder. A
confined `cat ~/.ssh/config` still pulls the file into model context. A
refused command is told which folder closed and that `sandbox.read` opens
one. A stdio MCP server you configured reads as before: it lives wherever
you keep it. The file tools ask a person before reading or changing a
closed folder (a failed edit shows lines of the file), even under auto, and
refuse it where nobody can answer, as in a headless run. Yolo at the `default`
level closes none. `/status`
lists the folders opened for reading. On macOS a closed folder cannot be listed; on Linux, Landlock
lists its names but refuses its files.

**Hidden stores.** With the sandbox on, commands cannot read, write or
connect to the sockets of these paths (those that exist), and the file tools
refuse them in every mode:

- the browser data directory (`UAGENT_BROWSER_DATA`) and `~/.uagent/web`,
  which holds the paired devices' tokens;
- `~/.uagent/config`: the saved settings and API keys, the trust store and
  remembered approvals;
- other tools' credential stores: `~/.aws`, `~/.config/gcloud`, `~/.azure`,
  `~/.kube`, `~/.gnupg`, `~/.netrc`, `~/.git-credentials` and
  `~/.docker/config.json`
  (`kHiddenCredentialStores` in
  [`include/core/defaults.h`](include/core/defaults.h)).

`~/.ssh` and the GitHub CLI login (`~/.config/gh`) stay readable: git over
ssh and `gh` are expected agent work and need them. Add your own store with
`sandbox.hide` (`sandbox.hide=~/.config/mycloud`); `sandbox.reveal` takes a
default out of the list (`sandbox.reveal=~/.aws`), for the file tools too.
macOS keychains are not listed: keychain access goes through the `securityd`
service, so hiding the files in `~/Library/Keychains` would not stop a
command asking it for a password (the files themselves are encrypted).

`run(sandbox=false)` and `off` lift the hiding along with the sandbox; yolo
does not. On Linux, Landlock hides the files on any supported kernel but the
sockets only from Linux 7.1 (ABI 9). Keep the browser data directory outside
the sandbox's writable roots, for example `~/.uagent/browser`; see
[the browser guide](docs/BROWSER.md#isolation).

On Linux this has one side effect. Because Landlock can only grant, reads are
granted folder by folder around a hidden path, as the folders are when a
command starts. A command that is still running (a server, a watcher) cannot
read a file created or replaced afterwards directly in a folder on the way to
a hidden path: `/`, `/home`, your home directory, `~/.uagent`, `~/.config`
and `~/.docker`. Files in any other folder, a repository among them, are
unaffected. A session whose folder is one of those folders is told so.

**Network.** Outbound access is allowed, because git, npm and pip need it.
Denying it (`sandbox.network` off, or the `readonly` level) blocks all IP
traffic on macOS and TCP (bind and connect) on Linux, where Landlock cannot
express the rest.

**Inside the folder**, a sandboxed command cannot change `.mcp.json` or,
in a repository, `.git/config` and `.git/hooks` (which your own git would
run); it can still commit. macOS denies the write. Landlock has no deny
rule, so on Linux the three are recorded before a sandboxed command runs and
put back when it ends (when it returns, or when a background or detached activity
is collected), and the model and you are told:
`restored .git/hooks: sandboxed commands may not change it`. The folder's
trust still follows `.mcp.json`'s content. This is a check at the end of a
command, not a wall: a hook added by a long-running command is live until it
ends, a change you make to those paths while a sandboxed command runs is
undone with it, and a detached activity still running when the session ends is
not checked. A folder's instruction file (`AGENTS.md`) is an ordinary
repository file that any session in it may edit.

**MCP servers.** A stdio server is spawned under the same policy as a
command, hidden stores included, with the folder and the package caches
writable, so a server started with `npx` or `uvx` works. Its network follows
the level and `sandbox.network`. A server that needs more can get a root
through `sandbox.write`. A server keeps the sandbox it was started with:
one started under yolo keeps `relaxed` when the mode changes, and one
started before keeps `default`. An entry that sets `"sandbox": false` runs
unconfined, in the folder's `.mcp.json` or in `~/.mcp.json`. The prompt that
asks you to trust a folder's `.mcp.json` names the servers that would run
unconfined, and a changed entry asks again like any other change to the file.

**Not confined by the sandbox:** the built-in file tools (approval and path
checks govern them; they reach `.mcp.json`, and the `uagent` tool reaches the
saved settings, with mandatory human approval for each change), an MCP
server that opts out, and the µAgent process of a memory extraction (its
commands are confined).

Besides `off`, a session can run unconfined in two ways, both reported:

- `run(sandbox=false)` always asks a person. Yolo and a remembered grant
  cannot give that approval, and headless or delegated runs have nobody to
  ask, so a delegated child cannot unconfine itself.
- On a Linux kernel without Landlock (before 5.13), the sandbox degrades: a
  startup warning, a `capability.changed` event, `sandbox.mode=degraded`, and
  commands run unconfined. A session that requested the sandbox explicitly,
  in the environment or a saved setting, refuses to run commands instead. So
  does one that denies the network (`sandbox.network` off) on a
  kernel whose Landlock cannot restrict it (before ABI 4).

On macOS, a sandbox profile larger than 64 KiB, and on Linux a policy larger
than 96 KiB (very many writable roots or closed folders), refuses the
command rather than running it under a truncated one.

## Sensitive data

Sessions and debug logs may contain source, prompts, commands, output and
reasoning. They are private (owner-only files) but not encrypted, and are
pruned by age and count; [Persistence](docs/PERSISTENCE.md) lists each kind,
its retention and how to remove it. `config/settings.json` holds saved API
keys in clear text; treat it as a secret.

Do not attach secrets or enable debug logging unless disclosure is
acceptable. If a credential appears in a prompt, attachment, result or trace,
delete the affected artifacts and rotate the credential.

## Reporting

Use GitHub private vulnerability reporting. Include the version,
reproduction, impact and a suggested mitigation. Never put credentials or
private traces in a public issue.
