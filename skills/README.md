# Bundled skills

Skills installed into `~/.uagent/skills` by `install.sh`, which never
overwrites a directory that is already there — edit or delete one and it stays
that way. A workspace can shadow any of them by name under `./.uagent/skills`.

Only the front matter of each `SKILL.md` is read at startup; the body is sent
to the model when it opens the skill.

| Skill | Origin | License |
| --- | --- | --- |
| `find-skills` | [vercel-labs/skills](https://github.com/vercel-labs/skills) | MIT, see `find-skills/LICENSE` |

`find-skills` teaches the agent to search the public skills ecosystem with
`npx skills`. It suggests and, if asked, runs installs — every one of those is
an ordinary `run` call, so it needs approval like any other command.
