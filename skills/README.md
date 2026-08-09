# Bundled skills

Skills `install.sh` puts in `~/.uagent/skills`. It refreshes its own bundled
copy on later installs so release-specific procedures stay matched to the
binary and override incompatible user-level vendor copies of the same name. A
workspace can shadow any skill by name under `./.uagent/skills`.

Only the front matter of each `SKILL.md` is read at startup; the body is sent
to the model when it opens the skill. Optional comma-separated
`requires-tools` metadata hides a skill when its runtime tools are unavailable.
`argument-hint` advertises invocation input; the `skill` tool replaces
`$ARGUMENTS` and `${SKILL_DIR}` when the body is loaded.

| Skill | Origin | License |
| --- | --- | --- |
| `uagent-config` | µAgent | MIT, this repository |
| `browser-use` | µAgent | MIT, this repository |
| `find-skills` | [vercel-labs/skills](https://github.com/vercel-labs/skills) | MIT, see `find-skills/LICENSE` |

`uagent-config` documents the release's configuration precedence and complete
runtime, install, and integration environment reference. It keeps that detail
out of the base system prompt and loads it only for configuration work.

`browser-use` drives either the shared user Chrome or an isolated browser with
µAgent's built-in Chrome MCP. It includes session health checks and uses only
the tools registered by this runtime.

`find-skills` teaches the agent to search the public skills ecosystem with
`npx skills`. It suggests and, if asked, runs installs — every one of those is
an ordinary `run` call, so it needs approval like any other command.
