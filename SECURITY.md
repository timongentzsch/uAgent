# Security

µAgent is a local, single-user coding agent. It confines the commands it runs
(see [Sandboxing](#sandboxing)) but is not a container: approval grants the
current user's permissions, and reads are unrestricted apart from the
[hidden stores](#sandboxing). It sends prompts, selected files, tool results
and attachments to the configured endpoint; treat that endpoint as trusted
infrastructure. Use a container, VM or restricted account for untrusted
code.

## What the agent may do without asking

The approval mode decides who is asked; the sandbox decides what a command
can write. They are separate: no approval mode turns the sandbox off.

| Mode | Set with | Effect |
| --- | --- | --- |
| `ask` (default) | `/permissions ask` | a person approves each mutating, process, network, cost-bearing or untrusted MCP call, and each read outside the workspace |
| `auto` | `/permissions auto`, `UAGENT_APPROVAL=auto` | a decision model allows or denies the calls the sandbox contains; what it cannot decide, every call that leaves the sandbox or the folder, and every `run` or `scratch` call while the sandbox is off or not enforced go to a person, and are denied when nobody can answer |
| `yolo` | `--yolo`, `/yolo`, `/permissions yolo` | those calls run unasked; commands stay sandboxed |

In every mode:

- Reads inside the workspace need no approval, apart from the files named
  below.
- An exact action you allowed for the session, or remembered for the
  repository, is not asked again. `/permissions rules` lists what is
  remembered and `/permissions forget N` removes one.
- Some operations always require a person, even under yolo, auto or a
  remembered rule, and are denied when no interactive client can answer, as
  in a delegated child:
  - writing µAgent's configuration through the file tools: the saved
    settings, the trust store, `~/.uagent/config/permissions.json`, your
    instruction files in `~/.uagent`, and any `.mcp.json`;
  - reading any of those through the file tools, except the trust store and
    the instruction files, which hold no credential;
  - the `uagent` tool's `configure` and `set_instructions` actions;
  - `run(sandbox=false)`.

Two kinds of session act for you unattended:

- A **subagent** approves its own tool calls; approving the delegation is the
  gate, and the approval says so. It runs its commands under the sandbox of
  the session that delegated to it, whatever its own configuration says.
- A **panel's thread** cannot be put in yolo: `/permissions yolo` is refused
  there and a saved or flagged yolo reads as auto. Its sandbox is always on.
  While that sandbox is enforced, its `run` and `scratch` commands and its
  file changes inside the workspace run without review; everything else goes
  through its approval mode.

## Trust boundaries

- Settings are never read from the project folder, and project `.env` files
  are ignored. Saved settings live in `~/.uagent/config/settings.json`
  ([Operations](docs/OPERATIONS.md#settings) has the scopes and their order).
  That file and artifact files are forced private.
- Workspace trust is about `.mcp.json` alone. User `~/.mcp.json` is trusted
  executable configuration. Project `.mcp.json` requires interactive trust or
  `--trust-project-config`; semantic edits revoke stored trust.
- Project instruction files, memories and skills enter model context without
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
- Requests, responses, attachments, tool output, scans, jobs, MCP data and
  logs are bounded. MCP server stderr goes to a rotating, size-bounded log.
- API redirects are rejected and bearer-auth transfers use HTTP(S) only.
- `web_fetch` makes credential-free direct HTTP(S) connections only to public
  IPv4 or IPv6 destinations. The resolved address is checked before every
  connection, including redirects; proxy variables are ignored so they cannot
  bypass that check.
- Shell commands and subagents run in managed process groups. Child processes
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
  before model requests and memory writes; `UAGENT_MEMORY_REDACT_KEYWORDS`
  adds site-specific assignment keywords. Codex and Claude memories are
  read-only, untrusted evidence. Redaction is defense in depth, not a
  guarantee that every secret is recognized.
- Model, MCP and tool text is terminal-sanitized.
- The web host's pairing, device authentication, Origin checks and file
  serving are described in [Web interface](docs/WEB.md#security-model).

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
  never what. Screenshots draw password fields as dots;
  [the browser guide](docs/BROWSER.md#isolation) lists the fields that are
  not masked.
- With the sandbox on, commands cannot read the profile or connect to its
  sockets, and the file tools refuse it in every mode.
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
Linux. The sandbox is on by default and is the same in every approval mode.

One setting chooses how far it goes, `sandbox.level` in the settings file or
`UAGENT_SANDBOX` in the environment:

| Level | Writes | Network | Hidden stores |
| --- | --- | --- | --- |
| `strict` | the roots below | none | yes |
| `default` | the roots below | allowed | yes |
| `relaxed` | the roots below and your home folder, except `~/.uagent` and the hidden stores | allowed | yes |
| `off` | unconfined | allowed | no |

`0` and `false` are accepted as `off`, `1` and `true` as `default`, as
`UAGENT_SANDBOX` was spelled while it was a switch. A saved
`sandbox.enabled` is renamed to `sandbox.level` (`false` becomes `off`,
`true` `default`) the first time the settings are saved.

Four more settings each adjust one thing and win over the level:

| To | Setting | Environment |
| --- | --- | --- |
| allow or deny the network whatever the level | `sandbox.network` | `UAGENT_SANDBOX_NET=0` or `1` |
| let commands write another folder | `sandbox.write` | `UAGENT_SANDBOX_WRITE=/path/a:/path/b` |
| hide more paths from commands and the file tools | `sandbox.hide` | `UAGENT_SANDBOX_HIDE=~/.config/mycloud:/path/b` |
| show a path that is hidden by default | `sandbox.reveal` | `UAGENT_SANDBOX_REVEAL=~/.aws` |

Paths in `sandbox.hide` and `sandbox.reveal` are absolute or start with
`~/`. All of these settings take effect at the next start of the
conversation (`/restart`). `/status` names the mechanism and `/context`
lists every writable root. The agent can call `run(sandbox=false)` for one
command, which a person must approve. `--yolo` stops the questions, not the
confinement.

**Writes** are what the sandbox restricts. A confined command may write:

- the workspace;
- `$TMPDIR`, `/tmp`, `/var/tmp` and `/dev`;
- the package caches: `~/.cache`, `~/.local/share/uv`, `~/.npm` and, on
  macOS, `~/Library/Caches` (`kPackageCaches` and `kMacPackageCaches` in
  `include/core/defaults.h`; grant another with `sandbox.write`);
- `~/.uagent/terminals/logs`, where a detached job's log pump writes;
- the roots `sandbox.write` adds;
- at the `relaxed` level, your home folder.

Everything else is refused. `~/.uagent` is never granted, and a requested
root is rejected, and reported at startup, when it is `/`, `~/.uagent` or
one of its ancestors, or lies inside `~/.uagent/config`. That keeps the
saved settings, trust store, collaborator records and detached-job records
out of reach. The `relaxed` level opens the home folder around `~/.uagent`:
its terminal logs, and a workspace under `~/.uagent/spaces`, stay writable;
nothing else in it is. On Linux, Landlock can only grant, so the home folder
is granted entry by entry as it is when a command starts: a new file made
directly in the home folder is not writable, while files in its folders and
existing files are. The startup note says so.

The workspace is held to the same rule. A session started in your home
directory, which holds `~/.uagent`, has no writable workspace below the
`relaxed` level: its commands can write only the other roots above, while
the file tools still work there. Startup says so, the model is told, and a
refused write names the roots. Start in a project folder instead, or in a
workspace: the web's **New → Workspace…** takes a name and keeps a folder
for it at `~/.uagent/spaces/<name>`, which is granted like any other
workspace.

**Reads** are not restricted on either platform, except for the hidden
stores below. A confined `cat ~/.ssh/config` still pulls the file into model
context.

**Hidden stores.** With the sandbox on, commands cannot read, write or
connect to the sockets of these paths (those that exist), and the file tools
refuse them in every mode:

- the browser data directory (`UAGENT_BROWSER_DATA`) and `~/.uagent/web`,
  which holds the paired devices' tokens;
- `~/.uagent/config`: the saved settings and API keys, the trust store and
  remembered approvals;
- other tools' credential stores: `~/.aws`, `~/.config/gcloud`, `~/.azure`,
  `~/.kube`, `~/.gnupg`, `~/.netrc` and `~/.docker/config.json`
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
and `~/.docker`. Files in any other folder, a project among them, are
unaffected. A session whose workspace is one of those folders is told so.

**Network.** Outbound access is allowed except at the `strict` level,
because git, npm and pip need it. Denying it (`strict`, or `sandbox.network`
off) blocks all IP traffic on macOS and TCP (bind and connect) on Linux,
where Landlock cannot express the rest.

**Inside the workspace**, a sandboxed command cannot change `.mcp.json` or,
in a repository, `.git/config` and `.git/hooks` (which your own git would
run); it can still commit. macOS denies the write. Landlock has no deny
rule, so on Linux the three are recorded before a sandboxed command runs and
put back when it ends (when it returns, or when a background or detached job
is collected), and the model and you are told:
`restored .git/hooks: sandboxed commands may not change it`. A project's
trust still follows `.mcp.json`'s content. This is a check at the end of a
command, not a wall: a hook added by a long-running command is live until it
ends, a change you make to those paths while a sandboxed command runs is
undone with it, and a detached job still running when the session ends is
not checked. A project's instruction files (`AGENTS.md`,
`.uagent/PANEL.md`) are ordinary repository files that any session in it may
edit.

**MCP servers.** A stdio server is spawned under the same policy as a
command, hidden stores included, with the workspace and the package caches
writable, so a server started with `npx` or `uvx` works. Its network follows
the level and `sandbox.network`. A server that needs more can get a root
through `sandbox.write`. An entry that sets `"sandbox": false` runs
unconfined, in the project's `.mcp.json` or in `~/.mcp.json`. The prompt that
asks you to trust a project's `.mcp.json` names the servers that would run
unconfined, and a changed entry asks again like any other change to the file.

**Not confined by the sandbox:** the built-in file tools (approval and path
checks govern them; they reach `.mcp.json`, and the `uagent` tool reaches the
saved settings, with mandatory human approval for each change), an MCP
server that opts out, and the µAgent process of a subagent (its commands are
confined).

Besides `off`, a session can run unconfined in two ways, both reported:

- `run(sandbox=false)` always asks a person. Yolo and a remembered grant
  cannot give that approval, and headless or delegated runs have nobody to
  ask, so a delegated child cannot unconfine itself.
- On a Linux kernel without Landlock (before 5.13), the sandbox degrades: a
  startup warning, a `capability.changed` event, `sandbox.mode=degraded`, and
  commands run unconfined. A session that requested the sandbox explicitly,
  in the environment or a saved setting, refuses to run commands instead. So
  does one that denies the network (`strict`, or `sandbox.network` off) on a
  kernel whose Landlock cannot restrict it (before ABI 4).

On macOS, a sandbox profile larger than 64 KiB (very many writable roots)
refuses the command rather than running it under a truncated profile.

## Sensitive data

Sessions and debug logs may contain source, prompts, commands, output and
reasoning. They are private (owner-only files) but not encrypted, and are
pruned by age and count; [Persistence](docs/PERSISTENCE.md) lists each kind,
its retention and how to remove it. `uagent config export` prints everything
saved, API keys included; treat its output as a secret.

Do not attach secrets or enable debug logging unless disclosure is
acceptable. If a credential appears in a prompt, attachment, result or trace,
delete the affected artifacts and rotate the credential.

## Reporting

Use GitHub private vulnerability reporting. Include the version,
reproduction, impact and a suggested mitigation. Never put credentials or
private traces in a public issue.
