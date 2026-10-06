# Memory, skills and scheduled tasks

Memories, skills and scheduled tasks are managed the same way from three
places: the web UI (Library and Scheduled), the terminal (`/memory`,
`/skills`, `/schedule`) and scripts (`uagent --control`).

| To | Terminal |
| --- | --- |
| see what the agent remembers | `/memory` |
| give it a memory | `/memory set global/NAME @FILE`, or ask it to remember something |
| remove or move one | `/memory forget KEY`, `/memory rename KEY TARGET`, `/memory copy KEY TARGET` |
| see installed skills | `/skills` |
| add a skill | `/skills set global/NAME @SKILL.md` |
| switch a skill off or on | `/skills disable ID`, `/skills enable ID` |
| list scheduled tasks and runs | `/schedule` |
| run, pause or resume a task | `/schedule run ID`, `/schedule pause ID`, `/schedule resume ID` |

A key is `global/NAME` (every project) or `project/NAME` (this project).
Text after the key is taken as the content; `@FILE` reads it from a file.

## Library

A memory is a short Markdown note of at most 2 KiB; each scope holds up to
32. Global memories are read into every top-level session, smallest first,
while they fit in 2 KiB together. Of the rest the agent sees only the names
and opens one with its `memory` tool when the topic fits. It writes a memory
when you ask, and a background extractor adds lessons from finished sessions
unless `UAGENT_MEMORY_GENERATE=0`. `UAGENT_MEMORY=0` or `--no-memory` turns
memory off.

A skill is a folder with a `SKILL.md`; see
[Bundled skills](../skills/README.md) for the format and where skills are
found.

- µAgent's own memories and skills are editable. Bundled skills are
  read-only, and so is what Codex and Claude keep, which is listed only when
  `UAGENT_OTHER_AGENTS` names them. Copy one into a new entry to change it.
- Skill status is available, overridden, disabled or invalid. Required tools
  and supporting files are listed without executing anything.
- Disabling a skill adds it to `UAGENT_SKILL_EXCLUDE` as saved for all
  conversations; a project's own exclusion can still apply. Deleting a skill
  removes its `SKILL.md` and keeps supporting files.
- Saved changes apply to new sessions. They do not rewrite a running agent's
  startup context.

In the web UI, **Edit** opens an item's source; drafts survive navigation in
the current tab, and a stale revision cannot overwrite a newer file.
**Refresh** picks up changes made by an external editor.

From a script:

```sh
uagent --control '{"kind":"memory","action":"list","cwd":"/absolute/project"}'
uagent --control '{"kind":"skills","action":"list","cwd":"/absolute/project"}'
```

| Action | Applies to | Arguments |
| --- | --- | --- |
| `list`, `get` | memory, skills | `get` takes `key` |
| `set` | memory, skills | `key`, `revision`, `content` |
| `forget` | memory, skills | `key`, `revision` |
| `rename`, `copy` | memory | `key`, `revision`, `target` |
| `enable`, `disable` | skills | `key` |

Writes require the `revision` returned by `get`. New items use a
`global/NAME` or `project/NAME` key with an empty revision; an existing skill
is named by the `key` that `list` returns. The terminal commands fill in the
current revision themselves, and a JSON argument (`/memory {...}`) sends a
raw control request.

## Scheduled tasks

A scheduled task is a prompt that runs by itself: once at a future time, at
a fixed interval between one minute and one year, or on selected weekdays at
`HH:MM` in an installed IANA timezone. Create and edit tasks in the web UI's
Scheduled view, or with the `save` request below.

Tasks run only while `uagent --web` is running on the host; no browser needs
to stay open. Previews use the same calculation as execution. A repeated
local time runs at its first occurrence; a nonexistent daylight-saving time
is skipped.

Each run is an ordinary session, started from a frozen copy of the task:

- `model`: the task's model; blank uses the project default.
- `permissions`: the approval mode, `ask` (the default), `auto` or `yolo`.
  When a run needs a decision, open it in the web UI or terminal to answer.
- `environment`: `worktree` (the default) runs in a Git worktree created
  detached from committed `HEAD`; `local` runs in the project directory.

Worktrees and conversations are kept for review, including after the task is
deleted; remove a reviewed worktree with `git worktree remove`.

- **Run now** runs the saved definition; save edits first.
- **Pause** disables future occurrences and cancels queued runs. Active runs
  keep their frozen definition; **Stop** interrupts one.
- A task never overlaps itself; an occurrence during an active run is recorded
  as skipped. At most two scheduled runs execute at once; interactive sessions
  do not use these slots.
- Occurrences more than 60 seconds late are recorded as missed; there is no
  catch-up burst.
- After a web host restart, runs whose runtime survived reconnect. A claimed
  run whose runtime is gone becomes interrupted and is never resubmitted
  automatically; unclaimed queued runs may still start.
- Up to 64 tasks and 128 run records are kept; the oldest finished records
  are pruned first.

From a script, `save` creates a task (empty `revision`) or updates one
(`key` and the `revision` that `get` returns):

```sh
uagent --control '{
  "kind": "schedule", "action": "save", "revision": "",
  "task": {
    "name": "Review",
    "prompt": "Review the repository and report findings.",
    "cwd": "/absolute/project",
    "schedule": {"type": "weekly", "days": [1,2,3,4,5],
                 "time": "09:00", "timezone": "Europe/Zurich"}
  }
}'
```

A schedule is `{"type":"once","at":UNIX}`,
`{"type":"interval","seconds":N}` (optional `start`) or the weekly form
above, where day 0 is Sunday. `preview` takes a `schedule` and an optional
Unix `after` and returns when it would run.

The other actions take the task's `key`: `list` (none), `get`, `run`, and
`pause`, `resume` and `forget` with its `revision`; `stop` takes the run ID
as `key`. In the terminal, `/schedule run ID`, `/schedule pause ID`,
`/schedule resume ID`, `/schedule forget ID` and `/schedule stop RUN_ID` do
the same without JSON; the IDs are in `/schedule`.
