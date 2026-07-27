# Bundled skills

Skills `install.sh` puts in `~/.uagent/skills`, but only when the machine has
no skill of that name anywhere on the search path — `~/.agents/skills`,
`~/.claude/skills`, `~/.codex/skills` or `~/.uagent/skills`. A skill installed
for another agent already works here, so copying ours in beside it would only
create a second copy that drifts. A workspace can shadow any skill by name
under `./.uagent/skills`.

Only the front matter of each `SKILL.md` is read at startup; the body is sent
to the model when it opens the skill.

| Skill | Origin | License |
| --- | --- | --- |
| `find-skills` | [vercel-labs/skills](https://github.com/vercel-labs/skills) | MIT, see `find-skills/LICENSE` |

`find-skills` teaches the agent to search the public skills ecosystem with
`npx skills`. It suggests and, if asked, runs installs — every one of those is
an ordinary `run` call, so it needs approval like any other command.
