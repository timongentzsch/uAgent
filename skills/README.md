# Bundled skills

Binary archives carry the release-matched tree under
`share/uagent/skills`; the runtime discovers it relative to the executable.
`install.sh` also refreshes the same skills in `~/.uagent/skills`, where a user
copy overrides an incompatible vendor copy. A workspace can shadow any skill
by name under `./.uagent/skills`.

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

`browser-use` drives Playwright CLI through the existing approved `run` tool.
Its daemon reuses one browser across concise calls while snapshots stay outside
model context; recurring flows become deterministic Playwright code.

`find-skills` teaches the agent to search the public skills ecosystem with
`npx skills`. It suggests and, if asked, runs installs — every one of those is
an ordinary `run` call, so it needs approval like any other command.
