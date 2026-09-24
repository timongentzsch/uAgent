# Security

µAgent is a local, single-user coding agent. It confines the commands it runs
(see [Sandboxing](#sandboxing)) but is not a container: approval grants the
current user's permissions, and reads are unrestricted. It sends prompts,
selected files, tool results and attachments to the configured endpoint; treat
that endpoint as trusted infrastructure. Use a container, VM or restricted
account for untrusted code.

## Trust boundaries

- Settings come from process `UAGENT_*` and `OPENROUTER_*` variables, a
  trusted project `.uagent/.config` and `~/.uagent/.config`, in that order.
  Config and artifact files are forced private. Project `.env` files are
  ignored.
- User `~/.mcp.json` is trusted executable configuration. Project `.mcp.json`
  and `.uagent/.config` require interactive trust or `--trust-project-config`;
  semantic edits revoke stored trust.
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
- Mutating, process, network, cost-bearing and untrusted MCP tools require
  approval unless YOLO or an exact remembered rule allows them. An MCP call
  skips approval only when its server is trusted and marks the tool read-only.
  Reads outside the workspace also prompt. Auto mode sends the current request
  and a bounded action preview to the configured OpenRouter Decisions model;
  it denies unattended calls when review fails or asks.
- Some operations always require a person, even under YOLO, auto mode or a
  remembered rule, and are denied when no interactive client can answer, as
  in a delegated child: writing µAgent's configuration (user and project
  `.uagent/.config`, the trust store, `~/.uagent/config/permissions.json`,
  `system-prompt.json`, `.mcp.json`) or reading its credential-bearing files
  through file tools, the `uagent` tool's `configure` action, and
  `run(sandbox=false)`.
- Paths are canonicalized to reduce symlink escapes. Writes are atomic.
- Requests, responses, attachments, tool output, scans, jobs, turns, costs,
  MCP data and logs are bounded. MCP server stderr goes to a rotating,
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
  adds site-specific assignment keywords. `~/.uagent/memory/events.jsonl`
  stores only action, key, time, source and a redacted 160-character preview.
  Codex and Claude memories are read-only, untrusted evidence. Redaction is
  defense in depth, not a guarantee that every secret is recognized.
- Model, MCP and tool text is terminal-sanitized.

## Browser automation

Browser automation runs `@playwright/cli` through an ordinary approved `run`
command; pin its npm version when reproducible or offline execution matters.
Playwright's isolated mode uses a separate profile. CDP attach can inspect and
control authenticated tabs after Chrome's remote-debugging approval; close
sensitive tabs or use isolated mode when that access is unnecessary.
Playwright snapshots report form field values verbatim, including autofilled
passwords, card numbers and one-time codes. µAgent's redaction covers its own
transcripts, not page content a snapshot pulls into context.

## Sandboxing

Commands the agent runs are confined by the OS: `sandbox-exec` on macOS,
Landlock on Linux. It is on by default; `UAGENT_SANDBOX=0` turns it off.

The sandbox restricts **writes**. A confined command may write the workspace,
`$TMPDIR` and `/tmp`, the package caches and the roots `UAGENT_SANDBOX_WRITE`
adds. Everything else is refused, including `~/.uagent` and each of its
ancestors, which keeps the config, trust store, collaborator records and
detached-job records out of reach. `~/.uagent/terminals/logs` is the one
exception, because a detached job's log pump writes there. `/status` names the
mechanism; `/context` lists every writable root. A requested root that is
refused is reported at startup.

Reads are **not** restricted on either platform: a confined
`cat ~/.uagent/.config` still pulls the file into model context. Outbound
network access is allowed by default because git, npm and pip need it;
`UAGENT_SANDBOX_NET=0` denies all IP traffic on macOS and outbound TCP on
Linux, where Landlock cannot express the rest.

A workspace's `.uagent/.config`, `.uagent/system-prompt.json` and `.mcp.json`
are carved out of the writable workspace on macOS only. Landlock has no deny
rule, so the same guarantee on Linux would mean not granting the workspace.
The built-in file tools and the `uagent` tool still reach these files, with
mandatory human approval for each change.

A session can run unconfined in two ways, both reported:

- `run(sandbox=false)` always asks a person. `--yolo`, a remembered grant and
  headless or delegated runs answer no, so a delegated child cannot unconfine
  itself.
- On a Linux kernel without Landlock, the sandbox degrades: a startup warning,
  a `capability_changed` event, `sandbox.mode=degraded`, and commands run
  unconfined. A session that requested the sandbox explicitly, in the
  environment or a config file, refuses to run commands instead.

## Sensitive data

Sessions and debug logs may contain source, prompts, commands, output and
reasoning. They are private but not encrypted. Default retention is 30 days /
200 files for sessions, 14 days / 50 files for debug traces, and 7 days for
background, MCP and terminal logs.

Do not attach secrets or enable debug logging unless disclosure is acceptable.
If a credential appears in a prompt, attachment, result or trace, delete the
affected artifacts and rotate the credential.

## Reporting

Use GitHub private vulnerability reporting. Include the version, reproduction,
impact and a suggested mitigation. Never put credentials or private traces in a
public issue.
