# Library and scheduled tasks

The web UI's Library and Scheduled views call the same native operations as
`/memory`, `/skills`, `/schedule` and `uagent --control`. Management commands
do not start a model turn. Storage changes invalidate open Library views and
publish scheduled task and run snapshots over the web event stream.

## Library

The Library lists global and project memories and the discovered skills.
Memories are grouped as Global plus one group per project folder, matching the
conversation sidebar; only the open project's contents are loaded and searched.
The Project field also accepts a directory outside the conversation catalogue.

Selecting an item shows its source, path, revision, size, recorded provenance
and rendered Markdown. **Edit** opens the source; drafts survive navigation in
the current tab, and a stale revision cannot overwrite a newer file.
**Refresh** picks up changes made by an external editor.

- uAgent memories and skills are editable. External (Codex, Claude) and bundled
  sources are read-only; copy them into a new uAgent entry to customize.
- Skill status is available, overridden, disabled or invalid. Required tools
  and supporting files are listed without executing anything.
- Disabling a skill edits the global `UAGENT_SKILL_EXCLUDE` list; a project
  exclusion can still apply. Deleting a skill removes its `SKILL.md` and keeps
  supporting files.
- Saved changes apply to new sessions. They do not rewrite a running agent's
  startup context.

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
`global/NAME` or `project/NAME` key with an empty revision. In the terminal,
`/memory set KEY @FILE` and `/skills set KEY @FILE` read content from a file
and fill in the current revision; a JSON argument sends a raw control request.

## Scheduled tasks

Scheduled tasks run only while `uagent --web` is running on the host; no
browser needs to stay open. A task runs once at a future time, at a fixed
interval between one minute and one year, or on selected weekdays at `HH:MM`
in an installed IANA timezone. Previews use the same calculation as execution.
A repeated local time runs at its first occurrence; a nonexistent
daylight-saving time is skipped.

Each run freezes its task definition and is claimed before launch. It creates
an ordinary session with the task's model (blank uses the project default) and
permission mode: Ask (`prompt`), Auto (`auto`) or YOLO (`yolo`). When a run
needs a decision, open it in the web UI or terminal to answer.

Runs use a Git worktree by default (`environment: "worktree"`), created
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
- The store holds up to 64 tasks and 128 run records; the oldest finished
  records are pruned first. A damaged store is reported and preserved.

```sh
uagent --control '{"kind":"schedule","action":"save","revision":"","task":{"name":"Review","prompt":"Review the repository and report findings.","cwd":"/absolute/project","environment":"worktree","permissions":"prompt","schedule":{"type":"weekly","days":[1,2,3,4,5],"time":"09:00","timezone":"Europe/Zurich"}}}'
uagent --control '{"kind":"schedule","action":"list"}'
```

| Action | Arguments |
| --- | --- |
| `list` | — |
| `get` | `key` |
| `save` | `task`, `revision` (empty for a new task; `key` to update) |
| `pause`, `resume`, `forget` | `key`, `revision` |
| `run` | `key` |
| `stop` | `key` set to the run ID |
| `preview` | `schedule`, optional Unix `after` |

Schedules use `{"type":"once","at":UNIX}`,
`{"type":"interval","seconds":N}` (optional `start`) or
`{"type":"weekly","days":[0-6],"time":"HH:MM","timezone":"Zone/Name"}`, where
day 0 is Sunday. `/schedule run ID`, `/schedule pause ID` and
`/schedule resume ID` are terminal shortcuts.
