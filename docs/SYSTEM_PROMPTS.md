# Instructions and the system prompt

A session's system message is the built-in base followed by the instructions
people write, then host facts. Instructions only ever add; nothing a person or
an agent writes replaces the base, except the agent's own opt-in
self-directive below.

## Instruction files

| | Every session | A folder's coordinator |
| --- | --- | --- |
| Yours, in every folder | `~/.uagent/AGENTS.md` | `~/.uagent/COORDINATOR.md` |
| The project's | `AGENTS.md` at the repository root | `<folder>/.uagent/COORDINATOR.md` |

A session reads `AGENTS.md` files from yours down through the repository root
to its working directory (one per directory: `AGENTS.override.md`, else
`AGENTS.md`, else `CLAUDE.md`). A coordinator then reads its `COORDINATOR.md`
files, yours first. Together they are bounded to 32 KiB and read once when a
session starts, so they stay in the cached prefix: an edit reaches new and
restarted sessions.

A project's `AGENTS.md` is an ordinary repository file. Writing yours, or any
`COORDINATOR.md`, always needs a person, including under YOLO; reading them
does not.

## Editing

- Web: **Settings → Agent → Instructions** shows the stack in the order the
  model reads it; each file is edited in place.
- Terminal: `/instructions` shows it; `/instructions edit sessions|coordinator
  user|project` opens `$VISUAL` or `$EDITOR`.
- Agents: `uagent` action `inspect` topic `instructions`, and
  `set_instructions` with `audience`, `scope` and the whole `text`; the user
  approves the exact diff.
- Scripts: `uagent --control` with `kind=instructions`, `action=show|set`,
  optional `cwd`, and for `set` the `audience`, `scope` and `text`.

`uagent --show-system-prompt [--json]` prints what the model would read, without
a model call; `/context` shows a live conversation's full request.

## Self-directive

With `UAGENT_ADAPT_SYSTEM=1` the agent gets `adapt_system`: text it adds to
(overlay) or, with the user's approval, substitutes for (replace) the base, for
the current conversation only. Writes carry the revision from `show` and a
non-empty `reason`; an approval is bound to the exact previewed text, expires
after five minutes and cannot be reused. `/instructions clear` and the web's
Clear remove it. It is saved with the conversation and copied by forks.

## Limits and experiments

The assembled message is limited to 64 KiB. Tool schemas, approvals,
sandboxing and host limits are enforced by the runtime and cannot be changed by
prompt text. `UAGENT_PROMPT_OVERLAY` names an experimental JSON file whose
named-section `replace` entries and `append` text are applied to the built-in
base, so a variant can be measured without a rebuild; its digest is recorded in
request traces.
