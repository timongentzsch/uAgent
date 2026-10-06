# Security

µAgent is a local, single-user coding agent. It confines the commands it runs
(see [Sandboxing](#sandboxing)) but is not a container: approval grants the
current user's permissions, and reads are unrestricted apart from the browser
profile and the web host's state. It sends prompts, selected files, tool
results and attachments to the configured endpoint; treat that endpoint as
trusted infrastructure. Use a container, VM or restricted account for
untrusted code.

## What the agent may do without asking

The approval mode decides who is asked; the sandbox decides what a command can
write. They are separate: no approval mode turns the sandbox off.

| Mode | Set with | Effect |
| --- | --- | --- |
| `ask` (default) | `/permissions ask` | a person approves each mutating, process, network, cost-bearing or untrusted MCP call, and each read outside the workspace |
| `auto` | `/permissions auto`, `UAGENT_APPROVAL=auto` | a reviewer model allows or denies those calls; what it cannot decide goes to a person, and is denied when nobody can answer |
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
- A **coordinator's thread** cannot be put in yolo: `/permissions yolo` is
  refused there and a saved or flagged yolo reads as auto. Its sandbox is
  always switched on. While that sandbox is enforced, its `run` and `scratch`
  commands and its file changes inside the workspace run without review;
  everything else goes through its approval mode.

## Trust boundaries

- Settings come, the earlier winning, from what one conversation chose for
  itself (its model and approval mode only), command-line flags, process
  `UAGENT_*` and `OPENROUTER_*` variables, what is saved for the project
  folder and what is saved for all conversations. Both saved scopes are kept
  by µAgent in `~/.uagent/config/settings.json`, never in the project. That
  file and artifact files are forced private. Project `.env` files are
  ignored.
- Workspace trust is about `.mcp.json` alone. User `~/.mcp.json` is trusted
  executable configuration. Project `.mcp.json` requires interactive trust or
  `--trust-project-config`; semantic edits revoke stored trust. A
  `.uagent/.config` an earlier version left in a project is imported into
  the saved settings once and archived, and only when its content was
  approved then or that flag vouches for it.
- Project instruction files, memories and skills enter model context without
  configuration trust because they grant no capability. An explicit
  `$skill-name` mention loads that skill before the first model call. Treat an
  untrusted checkout as prompt input and review requested actions before
  approving them.
- Only structured provider tool calls are executed. Text that resembles
  another harness's call syntax is recognized only so the malformed response
  can be suppressed and retried; it is never translated or executed.
- Tool, file, web, memory, MCP and summary text is evidence: it cannot expand
  user-approved scope. Schema validation, capability policy, path checks and
  approval enforce the boundary; the system prompt reinforces it. µAgent does
  not semantically detect every prompt injection.
- An MCP call skips approval only when its server is trusted and marks the
  tool read-only. MCP servers run with the user's permissions, outside the
  command sandbox.
- Auto mode sends the current request and a bounded action preview to the
  configured OpenRouter Decisions model.
- Paths are canonicalized to reduce symlink escapes, and path restrictions
  are checked again when the call runs. Writes are atomic.
- Requests, responses, attachments, tool output, scans, jobs, MCP data and
  logs are bounded. MCP server stderr goes to a rotating,
  size-bounded log.
- API redirects are rejected and bearer-auth transfers use HTTP(S) only.
- `web_fetch` makes credential-free direct HTTP(S) connections only to public
  IPv4 or IPv6 destinations. The resolved address is checked before every
  connection, including redirects; proxy variables are ignored so they cannot
  bypass that check.
- Shell commands and subagents run in managed process groups. Child processes
  receive a centralized secret-deny environment; only class-specific
  credentials are re-added. Approved `run` commands can opt in exact variables
  with `UAGENT_SHELL_ENV_ALLOW`; the allowlist never applies to MCP servers,
  delegated agents or `scratch`.
- A `run(tty=true)` activity keeps a writable PTY for the harness lifetime.
  `activity(operation=write, chars=...)` sends raw bytes with the original
  process's permissions, so treat every write, interrupt and resize as process
  control. Input to non-TTY activities is rejected. Detached persistent
  activities keep logs but no reattachable stdin.
- Automatic memory extraction processes at most one idle saved session. Its
  child receives the configured model credential and only the memory tool.
  Transcript text and event previews are redacted for known credential forms
  before model requests and memory writes; `UAGENT_MEMORY_REDACT_KEYWORDS`
  adds site-specific assignment keywords. Codex and Claude memories are
  read-only, untrusted evidence. Redaction is defense in depth, not a
  guarantee that every secret is recognized.
- Model, MCP and tool text is terminal-sanitized.

## Browser automation

There are two ways the agent drives a browser.

**The shared Chrome of the web host** (the `browser` tool, available while
`UAGENT_BROWSER_DATA` is set and the web host runs it). You and the agent see
and drive the same browser, and its profile keeps your saved logins and
cookies.

- Actions that change the page need approval like any mutating call.
- The agent can use a saved login without seeing it: `fill_saved` has Chrome
  fill the one it offers for the focused field and does not read what was
  filled. Screenshots draw password fields as dots;
  [the web guide](docs/WEB.md#isolation) lists the fields that are not
  masked.
- With the sandbox on, commands cannot read the profile or connect to its
  sockets, and the file tools refuse it in every mode. See
  [Sandboxing](#sandboxing).
- Whatever a page shows after sign-in can enter model context. Use a profile
  that holds only the accounts the agent should reach.

**Playwright** (`@playwright/cli` through an ordinary approved `run`
command). Pin its npm version when reproducible or offline execution matters.

- Playwright's isolated mode uses a separate profile.
- CDP attach can inspect and control authenticated tabs after Chrome's
  remote-debugging approval; close sensitive tabs or use isolated mode when
  that access is unnecessary.
- Playwright snapshots report form field values verbatim, including
  autofilled passwords, card numbers and one-time codes. µAgent's redaction
  covers its own transcripts, not page content a snapshot pulls into context.

## Sandboxing

Commands the agent runs (`run`, `scratch`) are confined by the OS:
`sandbox-exec` (Seatbelt) on macOS, Landlock on Linux. The sandbox is on by
default and is the same in every approval mode.

| To | Use |
| --- | --- |
| see what is enforced | `/status` names the mechanism; `/context` lists every writable root |
| let commands write another folder | `UAGENT_SANDBOX_WRITE=/path/a:/path/b` |
| deny commands the network | `UAGENT_SANDBOX_NET=0` |
| run one command unconfined | the agent calls `run(sandbox=false)`; a person must approve it |
| run every command unconfined | `UAGENT_SANDBOX=0` |

The three settings take effect at the next start of the conversation
(`/restart`). `--yolo` is not in this table: it stops the questions, not the
confinement.

**Writes** are what the sandbox restricts. A confined command may write:

- the workspace;
- `$TMPDIR`, `/tmp`, `/var/tmp` and `/dev`;
- the package caches: `~/.cache`, `~/.local/share/uv` and, on macOS,
  `~/Library/Caches`;
- `~/.uagent/terminals/logs`, because a detached job's log pump writes there;
- the roots `UAGENT_SANDBOX_WRITE` adds.

Everything else is refused. `~/.uagent` is never granted, and a requested
root is rejected, and reported at startup, when it is `/`, `~/.uagent` or one
of its ancestors, or lies inside `~/.uagent/config`. That keeps the saved
settings, trust store, collaborator records and detached-job records out of
reach.

**Reads** are not restricted on either platform, with one exception below: a
confined `cat ~/.uagent/config/settings.json` still pulls the file into model
context.

**The browser profile and the web host's state** are the exception. With the
sandbox on, commands cannot read the browser data directory
(`UAGENT_BROWSER_DATA`) or `~/.uagent/web` (which holds the paired devices'
tokens), or connect to their sockets. `run(sandbox=false)` and a disabled
sandbox lift that, as they lift the sandbox itself; yolo does not. The file
tools refuse both paths in every mode. On Linux, Landlock hides the files on
any supported kernel but the sockets only from Linux 7.1 (ABI 9). Keep the
browser data directory outside the sandbox's writable roots, e.g.
`~/.uagent/browser`. See [the web guide](docs/WEB.md#isolation).

**Network.** Outbound access is allowed by default because git, npm and pip
need it. `UAGENT_SANDBOX_NET=0` denies all IP traffic on macOS and TCP (bind
and connect) on Linux, where Landlock cannot express the rest.

**Inside the workspace**, a `.uagent/.config` and `.mcp.json`, and in a
repository its `.git/config` and `.git/hooks` (which your own git would run),
are carved out of the writable workspace on macOS only; sandboxed commands can
still commit, but not change git config. Landlock has no deny rule, so the
same guarantee on Linux would mean not granting the workspace. On Linux, a
command that changes `.mcp.json` still revokes project trust, so the next
launch asks again. A project's instruction files (`AGENTS.md`,
`.uagent/COORDINATOR.md`) are ordinary repository files that any session in
it may edit.

**Not confined by the sandbox:** the built-in file tools (approval and path
checks govern them; they reach `.mcp.json`, and the `uagent` tool the saved
settings, with mandatory human approval for each change), MCP servers, and
the µAgent process of a subagent (its commands are confined).

A session can run unconfined in two ways besides `UAGENT_SANDBOX=0`, both
reported:

- `run(sandbox=false)` always asks a person. Yolo and a remembered grant
  cannot give that approval, and headless or delegated runs have nobody to
  ask, so a delegated child cannot unconfine itself.
- On a Linux kernel without Landlock (before 5.13), the sandbox degrades: a
  startup warning, a `capability.changed` event, `sandbox.mode=degraded`, and
  commands run unconfined. A session that requested the sandbox explicitly, in
  the environment or a saved setting, refuses to run commands instead. So
  does one that set `UAGENT_SANDBOX_NET=0` on a kernel whose Landlock cannot
  restrict the network (before ABI 4).

On macOS, a sandbox profile larger than 64 KiB (very many writable roots)
refuses the command rather than running it under a truncated profile.

## Sensitive data

Sessions and debug logs may contain source, prompts, commands, output and
reasoning. They are private (owner-only files) but not encrypted. Default
retention is 30 days / 200 files for sessions, 14 days / 50 files for debug
traces, 14 days for mail, and 7 days for background and MCP logs and for
captured outputs and HTTP exchanges. A detached terminal's log is not pruned
by age alone; it goes when the record of its exited job expires. See
[docs/PERSISTENCE.md](docs/PERSISTENCE.md) for the detail, locations and
removal.

`uagent config export` prints everything saved, API keys included; treat its
output as a secret.

Do not attach secrets or enable debug logging unless disclosure is acceptable.
If a credential appears in a prompt, attachment, result or trace, delete the
affected artifacts and rotate the credential.

## Reporting

Use GitHub private vulnerability reporting. Include the version, reproduction,
impact and a suggested mitigation. Never put credentials or private traces in a
public issue.
