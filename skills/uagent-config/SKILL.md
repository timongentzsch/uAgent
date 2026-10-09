---
name: uagent-config
description: Configure, inspect, troubleshoot, or explain µAgent settings, providers, models, effort, approvals, tools, skills, memory, MCP, web search, limits, retention, and installation. Use for questions about UAGENT_* or OPENROUTER_* variables, /config and saved settings (~/.uagent/config/settings.json), legacy ~/.uagent/.config or project .uagent/.config files, .mcp.json, or changing µAgent behavior.
---

# Configure µAgent

Answer from the running binary first, then from the generated references beside
this file. Do not load every reference; pick the one the question needs.

## Route the question

| Question | Source |
| --- | --- |
| What is active right now, and why? | `uagent` action `inspect`, topic `status` or `config` |
| What does this setting default to, and when does a change apply? | `uagent` action `inspect`, topic `config`, `name` set |
| Which flags exist? | `uagent` action `inspect`, topic `cli`, or `references/cli.md` |
| Which slash commands exist? | `uagent` action `inspect`, topic `commands`, or `references/slash-commands.md` |
| Full setting catalogue | `references/configuration.md` |
| What is in the system prompt? | `references/system-prompt.md`; how instructions layer onto it: `docs/INSTRUCTIONS.md` in a source checkout |
| Which prompt is this session actually running? | `uagent` action `inspect`, topic `prompt` (base digest, active sections, overlay) |
| Which built-in tools and arguments exist? | `uagent` action `inspect`, topic `tools`, or `references/tools.md` |
| Which model routes are configured, and is a credential set? | `uagent` action `inspect`, topic `routes` |
| How is µAgent built, and why? | `references/architecture.md` when installed, else `docs/ARCHITECTURE.md` in a source checkout |
| Limits, recovery or failure triage | `docs/OPERATIONS.md` in a source checkout |
| Change a setting persistently | `references/self-configuration.md` |
| What is a folder's coordinator, and how is it opened or limited? | `references/cli.md` (Coordinator), the `UAGENT_COORDINATOR_*` rows of `references/configuration.md` |
| What instructions do sessions and coordinators read? | `uagent` action `inspect`, topic `instructions` |
| Change instructions | `uagent` action `set_instructions` with `audience` (sessions or coordinator), `scope` (user or project) and the whole `text`; the user approves the exact diff. The user can also run `/instructions edit` |

`uagent` reports the installed binary, so it beats both these references
and any recollection when the two disagree. The references are generated from
the same registries at build time and carry a `manifest.json` naming the
version they match.

## Workflow

1. Establish scope: one run (a flag or an exported variable), all
   conversations (`user`), the conversations in this project folder
   (`project`), or this conversation only (model and approval mode).
2. Read the live value before proposing a change; `uagent` reports the
   source of each active setting and whether a restart is required.
3. Persist a setting with `uagent` action `configure`, which changes only the
   named settings in the saved document and shows the user the exact diff.
   Never print secret values; report only whether they are set.
4. Prefer scope `user` for persistent settings. Use `project` only when
   project-specific behavior is intended; it is saved for that folder, not
   written into the repository, and needs no workspace trust. Web host
   settings (`UAGENT_WEB_*`, `UAGENT_BROWSER_DATA`) save at `user` only. Use
   process exports for a one-off command. The tool has no conversation
   scope: the user sets that with `/config conversation KEY=VALUE`,
   `/model` or `/permissions`.
5. Change only the settings needed for the requested outcome. Keep limits at
   defaults unless there is a measured reason to raise them.
6. Validate without a billable model call: `uagent --help`, `uagent --version`,
   and the `uagent` tool's `inspect`. Inspect `.mcp.json` as JSON when it
   changed. Explain that a real prompt is the end-to-end check and may incur
   provider usage.

## Rules

- Apply precedence correctly: the conversation's own choice (model and
  approval mode only), then command-line flags, then process environment,
  then what is saved for the project folder, then what is saved for all
  conversations, then built-in defaults.
- Treat `UAGENT_API_KEY`, `OPENROUTER_API_KEY`, keys embedded in
  `UAGENT_PROVIDERS`, web-search keys, and MCP credentials as secrets. The
  configuration reference marks them 🔒.
- Do not add secrets to a repository. Saved settings live in µAgent's own
  private `~/.uagent/config/settings.json`, never in a project.
- `/model`, `/effort` and `/variant` choose for the conversation they are
  typed in only. `/model NAME --default` also saves the model for new
  conversations (`UAGENT_MODEL`).
- Do not enable `--yolo`, `UAGENT_APPROVAL=yolo`, project trust, credential
  forwarding, or broader tool capabilities without making the authority change
  explicit.
- Do not invent settings. `uagent` enumerates every registered setting; if
  a requested behavior is absent from it, say so rather than guessing.
- Never write the saved settings with the file tools. `uagent
  action=configure` is offered only in a top-level session where a person can
  approve (a terminal or the web UI, not a headless run or a subagent). When
  it is absent, give the user the `/config user KEY=VALUE` (or `project`)
  command to run themselves; do not make the change on their behalf.
