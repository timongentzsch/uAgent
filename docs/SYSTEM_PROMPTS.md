# Instructions and the system prompt

To tell the agent how to work, write instructions: `AGENTS.md` for every
session, `COORDINATOR.md` for a folder's coordinator. `/init` drafts a
project `AGENTS.md`, and `/instructions` shows what a session reads.

A session's system message is the built-in base followed by these
instructions. Instructions only ever add; nothing a person or an agent writes
replaces the base, except the agent's own opt-in self-directive below. Host
facts (date, working directory, approval mode) and the memory index ride in a
runtime note after the system message. When one changes, a new note is
appended and the earlier text stays, so the cached prefix holds.

## Instruction files

| | Every session | A folder's coordinator |
| --- | --- | --- |
| Yours, in every folder | `~/.uagent/AGENTS.md` | `~/.uagent/COORDINATOR.md` |
| The project's | `AGENTS.md` at the repository root | `<folder>/.uagent/COORDINATOR.md` |

A session reads one instruction file per directory, from `~/.uagent` and then
from the repository root down to its working directory. In each directory it
takes `AGENTS.override.md`, else `AGENTS.md`, else `CLAUDE.md` when
`UAGENT_OTHER_AGENTS` names `claude`; where a level has one of the others,
that is the file every editor below opens. A coordinator then reads its
`COORDINATOR.md` files, yours first.

Together the files are bounded to 32 KiB. They are read once when a session
starts, so they stay in the cached prefix: an edit reaches new and restarted
sessions (`/restart`), not a running one.

A project's `AGENTS.md` and `.uagent/COORDINATOR.md` are ordinary repository
files. Writing yours (`COORDINATOR.md` or any of the three names in
`~/.uagent`) always needs a person, including under YOLO; reading them does
not. Editors never write through a symbolic link.

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

With `UAGENT_ADAPT_SYSTEM=1` (off by default; applies after a restart) the
agent gets `adapt_system`: text it adds to (overlay) or, with the user's
approval, substitutes for (replace) the base, for the current conversation
only. An overlay needs no approval; a replace always needs a person. Writes
carry the revision from `show` and a non-empty `reason`; an approval is bound
to the exact previewed text, expires after five minutes and cannot be reused.
`/instructions clear` and the web's Clear remove it. It is saved with the
conversation and copied by forks.

## Limits and experiments

The assembled message is limited to 64 KiB. Tool schemas, approvals,
sandboxing and host limits are enforced by the runtime and cannot be changed by
prompt text. The built-in base and the short capability notes added for some
tools are listed in the generated
[system prompt reference](../skills/uagent-config/references/system-prompt.md).

`benchmarks/eval.py --prompt-overlay` names an experimental JSON
file whose named-section `replace` entries and `append` text are applied to
the built-in base, so a variant can be measured without a rebuild; its digest
is recorded in request traces.
