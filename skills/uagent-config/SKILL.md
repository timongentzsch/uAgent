---
name: uagent-config
description: Configure, inspect, troubleshoot, or explain µAgent settings, providers, models, effort, approvals, tools, skills, memory, MCP, web search, limits, retention, and installation. Use for questions about UAGENT_* or OPENROUTER_* variables, /config and saved settings (~/.uagent/config/settings.json), .mcp.json, or changing µAgent behavior.
---

# Configure µAgent

Answer from the running binary, never from memory: it is the installed
release, and it knows more than any copy of its documentation.

## Route the question

| Question | Source |
| --- | --- |
| What is active right now, and why? | `uagent`, topic `status` or `config` |
| What does this setting default to, and when does a change apply? | `uagent`, topic `config`, `name` set to its key or environment name |
| Which settings exist? | `uagent`, topic `config`, no `name`: every setting with its key, environment name, type, default, purpose and where it is set |
| Which flags exist? | `uagent --help`, or `uagent` topic `cli` |
| Which slash commands exist? | `uagent` topic `commands`, or `/help` |
| What is in the system prompt? | `uagent --show-system-prompt` (`--json` adds the sources); how instructions layer onto it: `docs/INSTRUCTIONS.md` in a source checkout |
| Which prompt is this session actually running? | `uagent` topic `prompt` (base digest, active sections, overlay) |
| Which built-in tools and arguments exist? | `uagent` topic `tools` |
| Which model routes are configured, and is a credential set? | `uagent` topic `routes` |
| How is µAgent built, and why? | `references/architecture.md` when installed, else `docs/ARCHITECTURE.md` in a source checkout |
| Limits, recovery or failure triage | `docs/OPERATIONS.md` in a source checkout |
| Change a setting persistently | `references/self-configuration.md` |
| What is a folder's panel, and how is it opened or limited? | `uagent panel --help`, and the `UAGENT_PANEL_*` settings from `uagent` topic `config` |
| What instructions do sessions and panels read? | `uagent` topic `instructions` |
| Change instructions | `uagent_instructions` with `audience` (sessions or panel), `scope` (user or project) and the whole `text`; the user approves the exact diff. The user can also run `/instructions edit` |

Do not load every reference; pick the one the question needs. Only
`self-configuration.md` and `architecture.md` are written by hand; every other
fact comes from the binary.

## Workflow

1. Establish scope: one run (a flag or an exported variable), all
   conversations (`user`), the conversations in this folder
   (`project`), or this conversation only (model and approval mode).
2. Read the live value before proposing a change; `uagent` reports the
   source of each active setting and whether a restart is required.
3. Persist a setting with `uagent_configure`, which changes only the
   named settings in the saved document and shows the user the exact diff.
   Never print secret values; report only whether they are set.
4. Prefer scope `user` for persistent settings. Use `project` only when
   folder-specific behavior is intended; it is saved for that folder, not
   written into the repository, and needs no folder trust. Web host
   settings (`UAGENT_WEB_*`, `UAGENT_BROWSER_DATA`) save at `user` only. Use
   process exports for a one-off command. The tool has no conversation
   scope: the user sets that with `/config conversation KEY=VALUE`,
   `/model` or `/permissions`.
5. Change only the settings needed for the requested outcome. Keep limits at
   defaults unless there is a measured reason to raise them.
6. Validate without a billable model call: `uagent --help`, `uagent --version`,
   and the `uagent` tool. Inspect `.mcp.json` as JSON when it
   changed. Explain that a real prompt is the end-to-end check and may incur
   provider usage.

## Rules

- Apply precedence correctly: the conversation's own choice (model and
  approval mode only), then command-line flags, then process environment,
  then what is saved for the folder, then what is saved for all
  conversations, then built-in defaults.
- Treat `UAGENT_API_KEY`, `OPENROUTER_API_KEY`, keys embedded in
  `UAGENT_PROVIDERS`, web-search keys, and MCP credentials as secrets. `uagent`
  reports them as set or unset and never shows the value.
- Do not add secrets to a repository. Saved settings live in µAgent's own
  private `~/.uagent/config/settings.json`, never in a folder.
- `/model` and `/effort` choose for the conversation they are
  typed in only. `/model NAME --default` also saves the model for new
  conversations (`UAGENT_MODEL`).
- Do not enable `--yolo`, `UAGENT_APPROVAL=yolo`, folder trust, credential
  forwarding, or broader tool capabilities without making the authority change
  explicit.
- Do not invent settings. `uagent` enumerates every registered setting; if
  a requested behavior is absent from it, say so rather than guessing.
- Never write the saved settings with the file tools. `uagent_configure`
  is offered only in a session where a person can approve (a terminal or
  the web UI, not a headless run). When
  it is absent, give the user the `/config user KEY=VALUE` (or `project`)
  command to run themselves; do not make the change on their behalf.
