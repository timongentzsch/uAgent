# Bundled skills

Binary archives include the release-matched tree under `share/uagent/skills`;
the runtime finds it relative to the executable. Normal and staged installs
write only to the requested prefix. Existing user skills are preserved; the
release bundle outranks older user copies, and workspace skills override both.
For a bare development binary, set `UAGENT_SKILL_PATH` explicitly. The former
installer-only `UAGENT_SKILLS_DIR` override is retired.

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
path. Unknown keys are ignored by the loader. Bundled-skill discovery,
package contents and generated-reference checks validate the actual contracts.

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

`self-improve` performs one bounded, verified improvement attempt on µAgent's
own source, and drives the generation loop that compares the incumbent binary
with a candidate built from that source on byte-identical fresh copies of the
same snapshot. Its controller owns identity, isolation, budgets, gates, the
deterministic verdict and the promotion pointer; promotion needs explicit human
approval and rollback restores the exact prior version.

`browser-use` drives Playwright CLI through the existing approved `run` tool.
Its daemon reuses one browser across concise calls while snapshots stay outside
model context; recurring flows become deterministic Playwright code.
