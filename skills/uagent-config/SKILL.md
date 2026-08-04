---
name: uagent-config
description: Configure, inspect, troubleshoot, or explain µAgent settings, providers, models, effort, approvals, tools, skills, memory, MCP, web search, limits, retention, and installation. Use for questions about UAGENT_* or OPENROUTER_* variables, ~/.uagent/.config, trusted project .uagent/.config, .mcp.json, or changing µAgent behavior.
---

# Configure µAgent

Read [references/environment.md](references/environment.md) completely before
answering or changing configuration. It is the bundled reference for the same
µAgent release as this skill.

## Workflow

1. Establish scope: one command, the global user config, or a trusted project.
2. Inspect the relevant existing file and preserve unrelated settings. Never
   print secret values; report only whether they are set.
3. Prefer `~/.uagent/.config` for persistent user settings. Use
   `./.uagent/.config` only when project-specific behavior is intended, and
   explain that the project must be trusted. Use process exports for a one-off
   command.
4. Change only the settings needed for the requested outcome. Keep limits at
   defaults unless there is a measured reason to raise them.
5. Validate syntax without making a billable model call. Use `uagent --help`
   and `uagent --version`; inspect `.mcp.json` as JSON when it changed. Explain
   that a real prompt is the end-to-end check and may incur provider usage.

## Rules

- Apply precedence correctly: process environment, trusted project config,
  global config, then built-in defaults. `UAGENT_CONFIG_FILE` replaces both
  config-file locations.
- Treat `UAGENT_API_KEY`, `OPENROUTER_API_KEY`, keys embedded in
  `UAGENT_PROVIDERS`, web-search keys, and MCP credentials as secrets.
- Do not add secrets to a repository. Keep user config mode private; µAgent
  sets a non-empty loaded config file to mode `0600`.
- Prefer `/model` and `/effort` when the user wants an interactive persisted
  selection. An explicit environment setting still wins.
- Do not enable `--yolo`, `UAGENT_APPROVAL=yolo`, project trust, credential
  forwarding, or broader tool capabilities without making the authority change
  explicit.
- Do not invent settings. If a requested behavior is absent from the bundled
  reference, inspect the installed release or source before claiming support.
