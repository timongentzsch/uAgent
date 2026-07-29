# Security

µAgent is a local coding agent, not an OS sandbox. It sends prompts, selected
files, tool results, and attachments to the configured endpoint. Treat that
endpoint and every MCP server as trusted infrastructure.

## Trust boundaries

- Settings come from process `UAGENT_*` variables, a trusted project
  `.uagent/.config`, and `~/.uagent/.config`, in that order. Config and artifact
  files are forced private. Project `.env` files are ignored.
- User `~/.mcp.json` is trusted executable configuration. Project `.mcp.json`
  and `.uagent/.config` require interactive trust or
  `--trust-project-config`; semantic edits revoke stored trust.
- Project instruction files, memories, and skills enter model context without
  configuration trust because they grant no capability themselves. Treat an
  untrusted checkout as prompt input and review requested actions before
  approving them.
- The default Chrome integration executes `chrome-devtools-mcp@latest` through
  `npx`, so its code can change without a µAgent update. Disable it with
  `UAGENT_CHROME_DEVTOOLS=0` when reproducible or offline execution matters.
- Mutating, process, network, cost-bearing, and MCP tools require approval
  unless yolo mode is active. External reads also prompt.
- Paths are canonicalized to reduce symlink escapes. Writes are atomic.
- Requests, responses, attachments, tool output, scans, jobs, turns, costs, MCP
  data, and logs are bounded.
- API redirects are rejected and bearer-auth transfers use HTTP(S) only.
- Shell commands and subagents run in managed process groups. Child processes
  receive a centralized secret-deny environment; only class-specific
  credentials are deliberately re-added. Approved `run` commands can opt in
  exact variables with `UAGENT_SHELL_ENV_ALLOW`; the allowlist never applies to
  MCP servers, delegated agents, or `run_python`.
- Model, MCP, and tool text is terminal-sanitized.

Approval grants the current user's filesystem and network permissions. Use a
container, VM, or restricted account for untrusted code.

Chrome's isolated mode uses a temporary profile. User mode can inspect and
control authenticated tabs after Chrome's remote-debugging approval; close
sensitive tabs or use isolated mode when that access is unnecessary.

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
