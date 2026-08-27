# Security

µAgent is a local coding agent, not an OS sandbox. It sends prompts, selected
files, tool results, and attachments to the configured endpoint. Treat that
model endpoint as trusted infrastructure; MCP descriptions and results remain
untrusted model evidence even when you trust a server to run.

## Trust boundaries

- Settings come from supported process `UAGENT_*` and `OPENROUTER_*` variables,
  a trusted project `.uagent/.config`, and `~/.uagent/.config`, in that order.
  Config and artifact files are forced private. Project `.env` files are
  ignored.
- User `~/.mcp.json` is trusted executable configuration. Project `.mcp.json`
  and `.uagent/.config` require interactive trust or
  `--trust-project-config`; semantic edits revoke stored trust.
- Project instruction files, memories, and selected skills enter model context
  without configuration trust because they grant no capability themselves.
  Explicit `$skill-name` mentions load that skill before the first model call;
  automatic selection still uses the skill tool. Treat an untrusted checkout
  as prompt input and review requested actions before approving them.
- Native mode executes only structured provider tool calls. The text protocol
  is enabled only after the active route explicitly rejects native tools, and
  then requires the entire assistant message to be valid call blocks. Unknown
  provider markup is rejected, never translated or executed. Tool output has
  fallback delimiters escaped before returning to model context.
- Tool, file, web, memory, MCP, and summary text can inform implementation but
  cannot expand user-approved scope. µAgent reinforces this in the system
  prompt, while schema validation, capability policy, path checks, and approval
  enforce the host boundary. It does not claim to semantically classify every
  possible prompt injection.
- Browser automation is an ordinary approved `run` command using the separately
  installed `@playwright/cli`; pin its npm version when reproducible or offline
  execution matters.
- Mutating, process, network, cost-bearing, and untrusted MCP tools require
  approval unless yolo mode is active. An MCP call skips approval only when its
  configured server is trusted and the server marks that tool read-only.
  External reads also prompt.
- Paths are canonicalized to reduce symlink escapes. Writes are atomic.
- Requests, responses, attachments, tool output, scans, jobs, turns, costs, MCP
  data, and logs are bounded.
- API redirects are rejected and bearer-auth transfers use HTTP(S) only.
- `web_fetch` makes credential-free direct HTTP(S) connections only to public
  IPv4 or IPv6 destinations. The actual resolved address is checked before
  every connection, including redirects; proxy environment variables are
  ignored so they cannot bypass that check.
- Shell commands and subagents run in managed process groups. Child processes
  receive a centralized secret-deny environment; only class-specific
  credentials are deliberately re-added. Approved `run` commands can opt in
  exact variables with `UAGENT_SHELL_ENV_ALLOW`; the allowlist never applies to
  MCP servers, delegated agents, or `scratch`.
- A `run(tty=true)` activity retains a writable PTY for the lifetime of the
  harness. `activity(operation=write, chars=...)` sends raw bytes with the
  permissions of the original process, so treat every write, interrupt, and
  resize as process control. Ordinary input to non-TTY activities is rejected.
  Detached persistent activities retain logs but no reattachable stdin channel.
- Automatic memory extraction processes at most one idle saved session. Its
  child receives the configured model credential but only the memory tool—no
  shell, filesystem, web, MCP, skill, or delegation tools. Transcript text and
  event previews undergo deterministic secret redaction. A private receipt
  reports the result; `~/.uagent/memory/events.jsonl` stores only action, key,
  time, source, and a redacted 160-character preview. Known credential forms
  are redacted before model requests and memory writes.
  `UAGENT_MEMORY_REDACT_KEYWORDS` adds literal site-specific assignment
  keywords but cannot disable the built-in set. Codex and Claude memories are
  read-only, untrusted evidence. This is defense in depth, not a guarantee that
  every secret can be recognized.
- Model, MCP, and tool text is terminal-sanitized.

Approval grants the current user's filesystem and network permissions. Use a
container, VM, or restricted account for untrusted code.

Playwright's isolated mode uses a separate profile. CDP attach can inspect and
control authenticated tabs after Chrome's remote-debugging approval; close
sensitive tabs or use isolated mode when that access is unnecessary. Its
snapshots report form field values verbatim — including password,
credit-card, and one-time-code inputs that autofill has populated — and expose
no flag to suppress them, so the redaction above covers µAgent's own
transcripts, not page content a snapshot pulls into context.

## Sensitive data

Sessions and debug logs may contain source, prompts, commands, output, and
reasoning. They are private but not encrypted. Default retention is 30 days /
200 session files, 14 days / 50 debug traces, and 7 days for bounded process
logs.

Do not attach secrets or enable debug logging unless disclosure is acceptable.
Delete affected artifacts and rotate a credential if it appears in a prompt,
attachment, result, or trace.

## Reporting

Use GitHub private vulnerability reporting when available. Include the version,
reproduction, impact, and suggested mitigation. Never put credentials or
private traces in a public issue.
