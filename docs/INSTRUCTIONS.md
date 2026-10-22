# Instructions and the system prompt

Instructions tell the agent how to work: `AGENTS.md` for every session,
`PANEL.md` for a folder's panel. `/init` drafts a project `AGENTS.md`, and
`/instructions` shows what a session reads.

A session's system message is the built-in base followed by these
instructions. Instructions only add; nothing a person or an agent writes
replaces the base. Host facts (date, working directory,
approval mode) and memory follow in a runtime note after the system message;
[Caching](CACHING.md#what-changes-the-prefix) says how a change to them keeps
the cached prefix.

## Instruction files

| | Every session | A folder's panel |
| --- | --- | --- |
| Yours, in every folder | `~/.uagent/AGENTS.md` | `~/.uagent/PANEL.md` |
| The project's | `AGENTS.md` at the repository root | `<folder>/.uagent/PANEL.md` |

A session reads one instruction file per directory, from `~/.uagent` and
then from the repository root down to its working directory. In each
directory it takes `AGENTS.override.md`, else `AGENTS.md`, else `CLAUDE.md`
when `UAGENT_OTHER_AGENTS` names `claude`; the editors below open whichever
file a level uses. A panel then reads its `PANEL.md` files, yours first.

Together the files are bounded to 32 KiB. They and memory are read once
when a session starts, so an edit or a saved memory reaches new and
restarted sessions (`/restart`), not a running one.

A project's `AGENTS.md` and `.uagent/PANEL.md` are ordinary repository
files. Writing yours (`PANEL.md` or any of the three names in
`~/.uagent`) always needs a person, including under YOLO; reading them does
not. Editors never write through a symbolic link.

## Editing

- Web: **Settings → All conversations** (or **This project**) **→
  Instructions** shows the files in the order the model reads them, each
  editable in place.
- Terminal: `/instructions` shows it; `/instructions edit sessions|panel
  user|project` opens `$VISUAL` or `$EDITOR`.
- Agents: `uagent` action `inspect` topic `instructions`, and
  `set_instructions` with `audience`, `scope` and the whole `text`; the user
  approves the exact diff.
- Scripts: `uagent --control` with `kind=instructions`, `action=show|set`,
  optional `cwd`, and for `set` the `audience`, `scope` and `text`.

`uagent --show-system-prompt [--json]` prints what the model would read, without
a model call; `/context` shows a live conversation's full request.

## Limits and experiments

The assembled message is limited to 64 KiB. The runtime enforces tool
schemas, approvals, sandboxing and host limits; prompt text cannot change
them. The built-in base and the short capability notes added for some tools
are in the generated
[system prompt reference](../skills/uagent-config/references/system-prompt.md).

`benchmarks/eval.py --prompt-overlay FILE` applies an experimental JSON
file's named-section `replace` entries and `append` text to the built-in
base, so a variant can be measured without a rebuild; its digest is recorded
in request traces.
