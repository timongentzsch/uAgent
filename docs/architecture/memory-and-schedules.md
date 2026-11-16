# Memory and schedules

This page covers what a conversation carries in from earlier work and how
work runs unattended: instructions, the system message, memory, skills, the
Library and scheduled tasks. Defaults are in
[Settings](../interfaces/settings.md); request layout is in
[Turn loop](turn-loop.md).

## Responsibility

| Part | Source | Lives in |
| --- | --- | --- |
| Instructions | `src/core/project.cc` | `AGENTS.md` and its variants, `~/.uagent/config/prompt.json` |
| Memory | `src/tools/memory.cc`, `src/agent/memory_store.cc` | `~/.uagent/memory/` |
| Extraction | `src/tools/memory_pipeline.cc` | one side request per conversation start |
| Skills | `src/core/skills.cc`, `include/tools/skill.h` | `SKILL.md` folders on a search path |
| Import | `src/app/import.cc` | copies into `~/.uagent/skills` and memory |
| Library | `src/app/library.cc`, `src/web/master_library.cc` | a view; nothing is copied |
| Schedules | `src/app/schedule.cc`, `src/app/session_schedules.cc` | `~/.uagent/scheduled/state.json` |

All of it is read when a runtime starts, so a change reaches new and
restarted conversations; only `skill` also finds one written since.

## Processes and threads

Instructions, memory and skills load in each runtime's bootstrap.
Extraction is one side request of that runtime (`Agent::StartSide`) on the
`memory.model` route, at most 300 s (`kMemoryExtractSeconds`). Schedules
run in the web host's `scheduled` thread (`SessionHost::RunSchedules`),
woken at the next due time or a change to `state.json`, which the CLI and
the host share under a `flock`. A check runs in a `--control` helper in the
run's folder, under that folder's sandbox, or in the remote gateway.

## Data and formats

**Instructions.** `LoadProjectInstructions` reads one file per directory:
`~/.uagent` first, then the repository root (the nearest ancestor with
`.git`) down to the working directory. In each it takes the first of
`AGENTS.override.md`, `AGENTS.md`, and `CLAUDE.md` (only when
`skills.otherAgents` names `claude`). Together they are capped at
`instructions.maxKb` (`UAGENT_INSTRUCTIONS_KB`).

**System message.** Message zero is the built-in base (`kBase` in
`src/agent/prompt.cc`: a preamble and the sections Evidence, Tools, Scratch,
Changes, Delegation, Answer), with `prompt.json` applied: `replace` swaps a
section's body by heading and `append` adds text at the end. A
`## Capabilities` section follows for tools that are offered (`activity`,
`web_search`, `browser`), then the instructions under
`# AGENTS.md instructions for <path>`; over 64 KiB (`kSystemPromptBytes`)
is refused. Memory goes instead in the runtime note with date, cwd and
approval mode, appended anew when it changes.

**Memory.** A note is a Markdown file `<name>.md` under
`~/.uagent/memory/global/` or `~/.uagent/memory/projects/<repo>-<id>/`,
where the project directory is keyed by the repository's `.git` (so
worktrees share it) and the host. Keys are `global/<name>` and
`project/<name>`. `codex/<name>` (`~/.codex/memories`) and `claude/<name>`
(`~/.claude/projects/<slug>/memory`) are listed read-only when
`skills.otherAgents` names them. A note may hold `memory.maxBytes` and a
scope `memory.max` notes; the count limits new notes only, and a file
written by hand is read in part. `RedactMemorySecrets` (credential
assignments, bearer and known tokens, PEM keys) runs on save and read.
Writes are journaled in `events.jsonl` (newest half kept past 256 KiB).

**Skills.** A skill is a directory with `SKILL.md`, named by the directory.
Front matter is flat `key: value`: `description` (required, cut to
1,024 bytes), `argument-hint`, and `requires-tools` (a comma list; the skill
is hidden unless every tool is offered). Roots, a later one winning by
name (`SkillSearchPath`): `~/.agents/skills`, `~/.claude/skills` and
`~/.codex/skills` if `skills.otherAgents` names them, `~/.uagent/skills`,
`share/uagent/skills`, `.agents/skills` from `/` down to the folder, the
folder's `.claude/skills` and `.codex/skills` likewise, and its
`.uagent/skills`; each scanned 6 levels deep. `skills.path` replaces the
list, `skills.exclude` hides names, `skills.max` caps the count.

**Schedules.** `state.json` holds `v`, `revision`, at most 64 `tasks` and
128 `runs` (oldest finished dropped first), within 4 MiB. A task has
`name`, `prompt` (≤ 8 KiB), `cwd`, `host`, `environment` (`worktree` or
`local`), `permissions`, `model`, `check` (≤ 2 KiB), `schedule` (`once`,
`interval`, or `weekly` in an IANA zone), `enabled`, `next`; a run keeps a
frozen copy as `definition`.

## Protocol

**Listing memory.** At start, `LoadAlwaysOnMemory` inlines whole global
notes, smallest first, while they fit `memory.alwaysBytes`; agents skip
this. `LoadMemoryIndex` then lists every other note as `- key: summary`
(front matter `description`, else the first sentence, 96 characters), ours
first and newest first, in what the instructions left of their budget. The
index is shown only while the `memory` tool is offered. The tool's actions
are `list`, `search`, `get` (adds the save date), `set` and `forget`; `set`
and `forget` take the library write lease. Its description tells the model
to save when the user corrects it, states a standing preference or asks,
and to update the note that already covers a topic.

**Extraction.** When a top-level local conversation starts with
`memory.enabled` and `memory.generate` on, `StartMemoryExtractor` looks for
one older conversation of the same folder that had memory on, has at least
2 turns, has been idle `memory.idleHours`, is younger than `history.days`
and is not waiting to be filed. It claims it with a marker in
`~/.uagent/memory/.processed/` (a stale claim expires after 15 min) and
sends one request: rules, the memory index, and the newest 32 KiB of user
and assistant text, redacted. The reply is `none` or `key: …` and a note,
saved through the same path as the tool. When a scope is three-quarters
full the request also carries that scope's notes, and a `merge: <key>` line
folds one of them into the new note. The conversation shows a notice only
when a note was created, updated or the extraction failed.

**Opening a skill.** The `skill` tool's description lists names and
descriptions within 8,000 characters, shortening descriptions first. A call
with `name`, or `$name` in a message, returns the body (≤ 512 KiB) with
`${SKILL_DIR}` and `$ARGUMENTS` replaced, plus a compatibility note for one
from `.claude` or `.codex`. A tool may name a skill that unlocks it
(`Tool::skill`): `uagent` is offered once `uagent-config` is open.

**Import.** `/skills import`, `/memory import` and the Library's Import
list Claude Code and Codex skills (up to 4 levels deep) and this folder's
Claude Code memories, whether or not `skills.otherAgents` names them. A
skill is copied to `~/.uagent/skills/<name>` with its files; tool names in
backticks or before the word "tool", and in `allowed-tools` or `tools`, are
mapped to µAgent's, and a `requires-tools` line naming them is added when
the skill has none. A skill whose licence forbids
copying, or that is too large, is listed with the reason. Nothing is
overwritten.

**Library.** `--control` `kind: "library"` lists each place's memories and
skills tagged with `kind` and `place` (`""` for global); `GET /api/library`
adds local artifacts ([Artifacts](artifacts.md)) and files shared with
`present`.

**A scheduled run.** Each tick the thread marks due tasks: an occurrence
over 60 s late is `missed`, one while the task's run is active `skipped`,
else `queued`. It claims up to 2 queued runs, less those already running
(`kScheduledConcurrency`), and for each: creates a detached worktree at
`HEAD` for `worktree`, runs the check if any (50 s; exit 0 starts the run
with the last 4 KiB of output added to the prompt, exit 1 records `quiet`,
anything else `failed`), then starts a session runtime with the task's
model and `permissions` (`plan`, `ask`, `auto` or `yolo`). A run that needs
a decision waits for a person like any conversation. Through the `schedule`
tool an agent may create a task with at most its own approval mode, in its
folder, `local`, on its model.

## Invariants and limits

- The model always sees memory labelled non-authoritative.
- A task never overlaps itself; missed runs are not caught up; runs start
  only while `uagent --web` holds its lease.
- Remote conversations get no extraction; remote tasks get no worktree.
- Unread results are a per-browser mark kept by the client, set when a
  run's conversation gets events while not open.

## Failure modes

- An extraction that fails is journaled and the conversation is released
  for another try; one cut off before its reply also releases it.
- After a host restart, runs whose runtime survived reattach; a claimed run
  whose runtime is gone is marked `interrupted` and never restarted.
- A save or change with a stale `revision` is refused with `conflict`; a
  remote task whose host is disconnected fails.

## Source and tests

| Path | What |
| --- | --- |
| `src/core/project.cc`, `src/agent/prompt.cc`, `src/agent/request.cc` | instructions, system message, runtime note |
| `src/tools/memory.cc`, `src/tools/registry_memory.cc`, `src/agent/memory_store.cc` | memory store, tool, redaction, events |
| `src/tools/memory_pipeline.cc`, `src/app/application_channel.cc` | extraction and its trigger |
| `src/core/skills.cc`, `include/tools/skill.h` | discovery, front matter, `skill` tool |
| `src/app/import.cc`, `src/app/library.cc`, `src/web/master_library.cc` | import, Library |
| `src/app/schedule.cc`, `src/app/session_schedules.cc`, `src/app/uagent_tool.cc` | store, scheduler, `schedule` tool |
| `tests/memory_fixture.py`, `tests/integration_management.py` | memory, skills, import, schedules |
| `tests/integration_web/library.py` | Library listing and delete |
