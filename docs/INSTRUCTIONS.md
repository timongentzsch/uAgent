# Instructions and the system prompt

Instructions tell the agent how to work: `AGENTS.md` for every session.
`/init` drafts a folder `AGENTS.md`, and `/instructions` shows what a session
reads.

A session's system message is the built-in base followed by these
instructions. Instructions only add; the base itself is changed section by
section, under [The system message](#the-system-message). Host facts (date,
working directory, approval mode) and memory follow in a runtime note after
the system message; [Caching](CACHING.md#what-changes-the-prefix) says how a
change to them keeps the cached prefix.

## Instruction files

| Whose | File |
| --- | --- |
| Yours, in every folder | `~/.uagent/AGENTS.md` |
| The folder's | `AGENTS.md` at the repository root |

A session reads one instruction file per directory, from `~/.uagent` and
then from the repository root down to its working directory. In each
directory it takes `AGENTS.override.md`, else `AGENTS.md`, else `CLAUDE.md`
when `UAGENT_OTHER_AGENTS` names `claude`; the editors below open whichever
file a level uses.

Together the files are bounded to 32 KiB. They and memory are read once
when a session starts, so an edit or a saved memory reaches new and
restarted sessions (`/restart`), not a running one.

A folder's `AGENTS.md` is an ordinary repository file. Writing yours (any of
the three names in `~/.uagent`) always needs a person, including under YOLO;
reading them does not. Editors never write through a symbolic link.

## Editing

- Web: **Settings → All conversations** (or **This folder**) **→
  Instructions** shows the files in the order the model reads them, each
  editable in place.
- Terminal: `/instructions` shows it; `/instructions edit user|project`
  opens `$VISUAL` or `$EDITOR`.
- Agents: `uagent` topic `instructions`, and `uagent_file` with `file`
  `instructions`, `scope` and the whole `text`; the user approves the exact
  diff. Both come with the `uagent-config` skill.
- Scripts: `uagent --control` with `kind=instructions`, `action=show|set`,
  optional `cwd`, and for `set` the `scope` and `text`.

## The system message

The base has six sections: Evidence, Tools, Scratch, Changes, Delegation and
Answer. You can replace the text of any of them and add text after the last.

- Web: **Settings → All conversations → System message** lists the sections
  with their built-in text. A changed section is marked, and Reset brings
  the built-in text back.
- File: your changes are kept in `~/.uagent/config/prompt.json`, as
  `{"replace": {"## Scratch": "..."}, "append": "..."}`. Like instruction
  files it is read when a session starts.

Scratch says how the agent does one-off computation: it writes a script under
`.uagent/scratch` and starts it with `run`, Python through `uv run` so its
packages stay out of your environment. Change it there if you want another
interpreter or another place.

`uagent --show-system-prompt [--json]` prints what the model would read,
without a model call; `/context` shows a live conversation's full request.

## Limits and experiments

The assembled message is limited to 64 KiB. The runtime enforces tool
schemas, approvals, sandboxing and host limits; prompt text cannot change
them. The short capability notes added for some tools are shown by
`uagent --show-system-prompt` too.

`benchmarks/eval.py --prompt-overlay FILE` applies a file of the same shape
in place of the saved one for a run, so a variant can be measured without a
rebuild; its digest is recorded in request traces.
