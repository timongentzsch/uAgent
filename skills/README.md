# Bundled skills

Binary archives include the release-matched tree under `share/uagent/skills`;
the runtime finds it relative to the executable. `install.sh` also refreshes
`~/.uagent/skills`. User copies override vendor copies, and workspace skills
under `./.uagent/skills` override both.

Only the front matter of each `SKILL.md` is read at startup; the body is sent
to the model when it opens the skill.

## Front matter

| Key | Origin | Read by µAgent |
| --- | --- | --- |
| `name`, `description` | [Agent Skills](https://agentskills.io) | `description`; the directory name wins over `name` |
| `license`, `metadata`, `compatibility`, `allowed-tools` | Agent Skills | no |
| `argument-hint` | Claude Code | yes — advertises invocation input |
| `requires-tools` | µAgent | yes — comma-separated; hides a skill when its runtime tools are absent |

`ParseSkillFrontMatter` is a flat `key: value` reader that ignores keys it does
not know, so a third-party skill written to the spec loads here unchanged.
`requires-tools` is the divergence in the other direction: it is µAgent's own
key, and a skill carrying it is not valid to upload elsewhere. That is a
deliberate trade — the spec's home for client-specific fields is a nested
`metadata:` map, and parsing one would mean a YAML map reader in the startup
path for a portability nobody is currently using. `benchmarks/slopscan.py`
enforces exactly the union above, so a misspelling fails the build instead of
being silently ignored.

The `skill` tool replaces `$ARGUMENTS` and `${SKILL_DIR}` when the body is
loaded.

## What ships

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
