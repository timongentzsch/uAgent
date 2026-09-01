# Bundled skills

Binary archives include the release-matched tree under `share/uagent/skills`;
the runtime finds it relative to the executable. `install.sh` also refreshes
`~/.uagent/skills`. User copies override vendor copies, and workspace skills
under `./.uagent/skills` override both.

Only the front matter of each `SKILL.md` is read at startup; the body is sent
to the model when it opens the skill. Optional comma-separated
`requires-tools` metadata hides a skill when its runtime tools are unavailable.
`argument-hint` advertises invocation input; the `skill` tool replaces
`$ARGUMENTS` and `${SKILL_DIR}` when the body is loaded.

| Skill | Origin | License |
| --- | --- | --- |
| `uagent-config` | µAgent | MIT, this repository |
| `self-improve` | µAgent | MIT, this repository |
| `browser-use` | µAgent | MIT, this repository |

`uagent-config` documents the release's configuration precedence and complete
runtime, install, and integration environment reference. It keeps that detail
out of the base system prompt and loads it only for configuration work.

`self-improve` runs one bounded personal prompt-overlay experiment. It
pre-registers the hypothesis and limits, records control and treatment trials,
calculates a deterministic verdict, requires human-approved activation, and
preserves exact rollback without storing session text.

`browser-use` drives Playwright CLI through the existing approved `run` tool.
Its daemon reuses one browser across concise calls while snapshots stay outside
model context; recurring flows become deterministic Playwright code.
