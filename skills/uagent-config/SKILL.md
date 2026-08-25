---
name: uagent-config
description: Configure, inspect, troubleshoot, or explain µAgent settings, providers, models, effort, approvals, tools, skills, memory, MCP, web search, limits, retention, and installation. Use for questions about UAGENT_* or OPENROUTER_* variables, ~/.uagent/.config, trusted project .uagent/.config, .mcp.json, or changing µAgent behavior.
---

# Configure µAgent

Answer from the running binary first, then from the generated references beside
this file. Do not load every reference; pick the one the question needs.

## Route the question

| Question | Source |
| --- | --- |
| What is active right now, and why? | `uagent_info` topic `status` or `config` |
| What does this setting default to, and when does a change apply? | `uagent_info` topic `config`, `name` set |
| Which flags exist? | `uagent_info` topic `cli`, or `references/cli.md` |
| Which slash commands exist? | `uagent_info` topic `commands`, or `references/slash-commands.md` |
| Full setting catalogue | `references/configuration.md` |
| How is µAgent built, and why? | `references/architecture.md` when installed, else `docs/ARCHITECTURE.md` in a source checkout |
| Change a setting persistently | `references/self-configuration.md` |

`uagent_info` reports the installed binary, so it beats both these references
and any recollection when the two disagree. The references are generated from
the same registries at build time and carry a `manifest.json` naming the
version they match.

## Workflow

1. Establish scope: one command, the global user config, or a trusted project.
2. Read the live value before proposing a change; `uagent_info` reports the
   source of each active setting and whether a restart is required.
3. Inspect the relevant existing file and preserve unrelated settings. Never
   print secret values; report only whether they are set.
4. Prefer `~/.uagent/.config` for persistent user settings. Use
   `./.uagent/.config` only when project-specific behavior is intended, and
   explain that the project must be trusted. Use process exports for a one-off
   command.
5. Change only the settings needed for the requested outcome. Keep limits at
   defaults unless there is a measured reason to raise them.
6. Validate without a billable model call: `uagent --help`, `uagent --version`,
   and `uagent_info`. Inspect `.mcp.json` as JSON when it changed. Explain that
   a real prompt is the end-to-end check and may incur provider usage.

## Rules

- Apply precedence correctly: command-line flags, then process environment,
  then a trusted project config, then the global config, then built-in
  defaults. `UAGENT_CONFIG_FILE` replaces both config-file locations.
- Treat `UAGENT_API_KEY`, `OPENROUTER_API_KEY`, keys embedded in
  `UAGENT_PROVIDERS`, web-search keys, and MCP credentials as secrets. The
  configuration reference marks them 🔒.
- Do not add secrets to a repository. Keep user config mode private; µAgent
  sets a non-empty loaded config file to mode `0600`.
- `/model`, `/effort` and `/variant` persist the interactive selection; an
  explicit environment setting still wins at the next launch.
- Do not enable `--yolo`, `UAGENT_APPROVAL=yolo`, project trust, credential
  forwarding, or broader tool capabilities without making the authority change
  explicit.
- Do not invent settings. `uagent_info` enumerates every registered setting; if
  a requested behavior is absent from it, say so rather than guessing.
