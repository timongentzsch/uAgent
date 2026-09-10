# Library and scheduled tasks

Library and Scheduled are native capabilities. The web UI calls the same
operations as `/memory`, `/skills`, `/schedule`, and `uagent --control`.
Management commands do not start a model turn. The browser keeps one SSE
subscription: native storage changes invalidate Library views and publish
scheduled task/run snapshots. Ordinary worker events carry run progress,
approvals, activity, messages and usage.

## Library

The Library lists global and project memories and the discovered skill inventory.
Memories use a Global group and project folder groups matching the conversation
sidebar. Open a folder to load its memories, or select it in the scope/project
filter. Only the open project's contents are loaded and searched. The Project
field also accepts a directory outside the conversation catalogue.
Select an item to inspect its source, path, revision, size, provenance (when
recorded), or rendered Markdown. Edit opens the source; Save and Cancel return
to the rendered view. Read-only items remain in that view. Drafts survive navigation
in the current browser tab; a stale revision cannot overwrite a newer file.
Refresh discovers changes made by an external editor. Native management writes
also notify connected clients.

uAgent memories and skills are editable. Codex, Claude and bundled sources are
read-only; copy their content into a new uAgent entry to customize it. Skill
status distinguishes available, overridden, disabled and invalid manifests.
Required tool names and supporting file paths are shown without executing them.
Disabling a skill updates the global name exclusion; a project exclusion can
still apply. Deleting a skill removes its manifest and keeps supporting files.

Saving a library item does not rewrite an active agent's startup context.
Memory enablement, startup memory budgets and skill catalogue selection keep
using the existing native configuration. Start a new session to load changes.
Project memory remains on-demand; eligible global memory fits within the
configured startup slice. The UI does not claim that every stored item is
currently in a particular conversation's context.

```sh
uagent --control '{"kind":"memory","action":"list","cwd":"/absolute/project"}'
uagent --control '{"kind":"skills","action":"list","cwd":"/absolute/project"}'
```

Use `get` with an item's `key`, then include its returned `revision` with `set`,
`forget`, or memory `rename`/`copy`. New destinations use `global/name` or
`project/name` and an empty revision. `set` supplies `content`; `rename` and
`copy` supply `target`. Skills use inventory keys for existing items and
`enable`/`disable` for global exclusions. In the terminal, `/memory set KEY
@FILE` and `/skills set KEY @FILE` read a file and perform the same revision
check. A JSON argument exposes the complete control protocol in either UI.

## Scheduled

Start `uagent --web` on the host. The singleton host owns the runner; no browser
needs to remain open. Tasks support a future one-off time, a fixed interval of
at least one minute, or selected weekdays in an installed IANA timezone.
Previews use the same native calendar calculation as execution. Repeated local
times run at their first occurrence; nonexistent daylight-saving times are
skipped. Calendar calculations run in an isolated helper because libc timezone
state is process-global.

Each run freezes its task definition and is durably claimed before launch. It
creates an ordinary session with the task's model selection and Ask/YOLO
permissions. A blank model uses the project's current default. Ask can pause for
approval; open the run's conversation in the web UI, or resume its saved session
in the terminal after closing its web worker. The history record includes the
session path for CLI access.

Git worktrees are the default and start from committed `HEAD`. Choose Local to
use the working directory directly. Worktrees and conversations are retained
for review; they are not deleted by deleting a task. Use the normal Git worktree
commands to clean up a reviewed worktree.

- Run now tests the saved definition. Save edits first.
- Pause disables future occurrences and cancels queued runs. Active runs keep
  their frozen definition; Stop requests native interruption.
- The same task cannot overlap itself. Runs share the host's existing worker
  limit with interactive sessions.
- Occurrences more than 60 seconds late are recorded as missed. Other due times
  advance without catch-up bursts; overlap records a skipped occurrence.
- After a host restart, a claimed or running job becomes interrupted for review.
  It is never automatically submitted again. Unclaimed queued jobs may start.
- History retains up to 128 run receipts and 64 tasks in a bounded, atomically
  replaced store. Old terminal receipts are pruned; conversations/worktrees remain.

```sh
uagent --control '{"kind":"schedule","action":"save","revision":"","task":{"name":"Review","prompt":"Review the repository and report findings.","cwd":"/absolute/project","environment":"worktree","permissions":"prompt","schedule":{"type":"weekly","days":[1,2,3,4,5],"time":"09:00","timezone":"Europe/Zurich"}}}'
uagent --control '{"kind":"schedule","action":"list"}'
```

`get`, `save`, `forget`, `pause`, `resume`, `run`, `stop` and `preview` are shared
by `/schedule` and the web UI. Task mutations use `key` plus `revision`; `stop`
uses a run ID. `/schedule run ID`, `/schedule pause ID`, and `/schedule resume
ID` are terminal shortcuts. `preview` takes `schedule` and an optional Unix
`after` timestamp. Store errors are reported without replacing the damaged file.

## Design references and verification

The local host/worktree/review flow is informed by
[Codex automations](https://learn.chatgpt.com/docs/automations).
Scope and explicit settings follow the familiar
[VS Code user/workspace distinction](https://code.visualstudio.com/docs/configure/settings).
List/detail editing and collapsed provenance follow
[progressive disclosure](https://www.nngroup.com/articles/progressive-disclosure/).
These are design references, not evidence that this specific layout is optimal.

`integration_management` verifies native revision conflicts, scopes, external
copying, path confinement, DST behavior, overlap rejection, saved run results and
restart recovery. Browser tests exercise editing, retained drafts, model choices,
unread results, run navigation and compact layouts. New screens are loaded on
demand and remain within the existing bundle limits.
