# Bundled skills

Skills `install.sh` puts in `~/.uagent/skills`. It refreshes its own bundled
copy on later installs so release-specific references stay current. A skill of
the same name under `~/.agents/skills`, `~/.claude/skills`, or
`~/.codex/skills` wins and is left untouched. A workspace can shadow any skill
by name under `./.uagent/skills`.

Only the front matter of each `SKILL.md` is read at startup; the body is sent
to the model when it opens the skill.

| Skill | Origin | License |
| --- | --- | --- |
| `uagent-config` | µAgent | MIT, this repository |
| `find-skills` | [vercel-labs/skills](https://github.com/vercel-labs/skills) | MIT, see `find-skills/LICENSE` |

`uagent-config` documents the release's configuration precedence and complete
runtime, install, and integration environment reference. It keeps that detail
out of the base system prompt and loads it only for configuration work.

`find-skills` teaches the agent to search the public skills ecosystem with
`npx skills`. It suggests and, if asked, runs installs — every one of those is
an ordinary `run` call, so it needs approval like any other command.
