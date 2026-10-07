# Persistence

Where µAgent keeps local state, how long it stays, and how to move or remove
it. State is private (files `0600`, directories `0700`) but unencrypted, and
records can contain prompts, model output, tool results, paths and usage.

## Locations

Paths under `~/.uagent` unless shown otherwise.

| State | Location |
| --- | --- |
| sessions | `history/<workspace>/*.json` |
| session event journals | `<session>.json.events.jsonl` |
| session writer lease | `<session>.json.lock` |
| web uploads | `<session>.json.assets/*` |
| undo journal: each turn's changed files as they were before it, and whole diffs | `<session>.json.edits/{index.json,<turn>-<hash>,diff-*}` |
| debug traces | `sessions/*.jsonl` |
| captured large outputs and HTTP exchanges | `artifacts/*` |
| background command logs | `bg/*` |
| detached terminal records and logs | `terminals/<pid>.json`, `terminals/logs/*` |
| delegated children | `history/<workspace>/agent-<id>.json`, sessions whose header carries `delegation` |
| mail between sessions, coordinators and children | `mail/<id>/{new,cur}/*.json` |
| MCP server logs | `mcp/*` |
| memories | `memory/{global,projects/<repository>}/*.md` |
| memory audit and extraction claims | `memory/events.jsonl`, `memory/.processed/*` |
| scheduled tasks | `scheduled/state.json` |
| worktrees of scheduled runs and coordinator threads | `worktrees/<id>` |
| session links | `links/*` |
| web host discovery, devices and push keys | `web/*` |
| settings, for all conversations and per project folder | `config/settings.json` (and its `.lock`) |
| project trust, permission rules, tool categories, first-run welcome shown | `config/trusted-projects.json`, `config/permissions.json`, `config/tool-categories.json`, `config/welcomed` |
| instructions | `AGENTS.md`, `COORDINATOR.md`, and `<folder>/.uagent/COORDINATOR.md` |
| your skills | `skills/*` |
| shared browser profile (saved logins, cookies) | the directory `UAGENT_BROWSER_DATA` names |
| runtime sockets and their leases | `/tmp/uagent-<uid>-<hash>/*.sock`, `*.sock.lock` |
| scratch scripts | `<workspace>/.uagent/scratch/*.py`, `*.sh` |
| Playwright snapshots and logs | `<workspace>/.playwright-cli/*` |

`<workspace>` is a hash of the project folder's path; each session file's
header records its `cwd` and title. Session activities (IDs, PTYs, incremental
output) exist only in memory and end with the process.

No settings are read from a project folder. What is read from it: `.mcp.json`
(once trusted), instruction files (`AGENTS.md`, `.uagent/COORDINATOR.md`) and
skills (`.agents/skills`).

## Settings

Saved settings are one document, `config/settings.json`:
`{"format": 2, "all": {...}, "projects": {"<folder>": {...}}}`, with
`config/settings.schema.json` beside it for an editor. µAgent writes it under
a lock, through `/config`, the web's Settings or the `uagent` tool; it may
also be edited by hand. See [OPERATIONS.md](OPERATIONS.md#settings).

- To back it up or move it to another host, use `uagent config export` and
  `uagent config import FILE`. The export contains API keys in clear text.
- A document that cannot be parsed is refused, never read as empty: the error
  names the file to fix or remove, and nothing is saved over it. One entry
  that cannot be taken is reported and left out, and the rest applies.
- A document of the format before (`"format": 1`, settings named by their
  environment variable and every value text) is rewritten in this format the
  first time it is read. `uagent config import` takes either.
- Text files of earlier versions are taken over once. `~/.uagent/.config`
  moves in on the first start. A project's `.uagent/.config` moves in the
  first time a conversation starts in that folder, if its content is what was
  approved when the workspace was trusted, or with `--trust-project-config`;
  otherwise it is ignored. A file taken over is renamed `.config.imported`
  and never read again.

## Sessions

To find a conversation, use `/sessions [PREFIX]`, `uagent --resume` or
`uagent -c` (the most recent). `/share` exports the transcript as Markdown.

A session file is a format-3 snapshot: active messages, the bounded archive of
compacted tool traces, usage, provider session identity, the system directive,
and optional `display` metadata (message IDs, readable reasoning, tool status
and timing, receipts, routes, usage and asset references) used to browse
history after context pruning. An optional `custom_title` keeps a name the
user chose.

- Saves validate the complete record, then write a private temporary file in
  the same directory, `fsync` it and rename it into place. An interrupted
  write leaves the previous record intact.
- Only format 3 loads. Incompatible, incomplete or corrupt files are reported
  and left untouched; there is no implicit migration.
- One runtime holds the writer lease for a conversation. CLI and web clients
  attach to it without taking a lease, and independent conversations can share
  a workspace. The lease is the locked descriptor, not the lock file's
  existence.
- A successful, untruncated file read stores `_uagent_read_range`
  (`[path, first_line, last_line]`) in its tool message. Wire requests omit
  it.
- The undo journal behind `/changes` and `/undo` keeps up to 2 MiB per file
  and 64 MiB per session. Changes made by shell commands are not in it.

The event journal (`uagent.session.event.v1`) keeps at most 512 records or
256 KiB of turn, tool, capability, configuration, presentation and artifact
metadata. It never holds prompts, reasoning, answer text or full tool results,
is not replay authority, and a missing or corrupt journal does not block
loading. Journals share their session's retention.

The snapshot is the only replay authority. `--debug` writes the separate,
sensitive, reconstructable trace. Interactive model calls also keep private
HTTP request and response captures in `artifacts/` for `/http` inspection;
credential headers are redacted, but prompts and tool output are not. See
[WEB.md](WEB.md#execution-and-persistence).

## Activities and delegated children

Completed output larger than `UAGENT_TOOL_RESULT_CHARS` moves to `artifacts/`
instead of entering context whole, so a returned path can expire with
retention. Detached terminal records store a boot-scoped process identity; a
record whose identity is missing or does not match is treated as exited and
never signalled.

Each delegated child has a durable ID naming its session file. A follow-up
starts a new supervised child from that saved conversation. A saved
conversation resumes after a restart, but live activities, PTYs and output
buffers do not.

## Mail

Sessions, a folder's coordinator and its threads, and delegated children talk
through one mailbox each, `mail/<id>/`, where `<id>` hashes the canonical
session file. A message is a private JSON file with a type
(`task.completed`, `ask`, `steer`, `note`, ...), sender, correlation id and
body.

- A send commits by an atomic rename into `new/`.
- The recipient moves what it takes to `cur/` and deletes it once the snapshot
  holding it is saved, so a crash in between delivers it again.
- The snapshot keeps the latest 256 delivered ids, so a message delivered
  twice enters the conversation once.
- Expired (24 h), unreadable and not-yet-permitted messages are never
  delivered; retention prunes the directory like `sessions/`.

## Memory

Memory Markdown files are the editable source of truth. They change only
through an explicit `memory` action or the single background extractor.
Codex memories in `~/.codex/memories` and the current Claude Code project
memory in `~/.claude/projects/<project>/memory` are read-only recall sources
for the agents named in `UAGENT_OTHER_AGENTS` (`claude`, `codex`); none is
by default.

The audit log records only action, key, time, automatic source and a redacted
preview, which `/memory` shows. The extractor marks a session done only after
a successful run; a failed run releases its claim at once so a later run can
retry.

## Retention

Pruning runs whenever a top-level session's runtime starts. A file older than
the days shown is removed; of the rest, the newest files up to the count are
kept.

| Tree | Days / files kept |
| --- | --- |
| `history/`, `memory/.processed/` | `UAGENT_HISTORY_DAYS` 30 / 200 |
| `sessions/` | 14 / 50 |
| `mail/` | 14 (undelivered mail expires after a day) |
| `bg/`, `artifacts/` | 7 / 200 |
| `mcp/` | 7 / 100 |
| exited detached terminals | 7, removed with their logs |

To keep conversations longer, raise the days, for example
`/config user UAGENT_HISTORY_DAYS=180`. The 200-file count is fixed and
counts across all projects.

Delegated children age out with `history/`. A session's journal, undo data and
attachments age out with it and never count against the file limit. Journals
whose session is gone are removed, and so are the socket and lease of a
runtime that was killed. Each process log is also bounded in size: 64 MiB for
commands, 16 MiB for an MCP server.

Not pruned: settings and the rest of `config/`, memories, skills, scheduled
tasks, the browser profile, and `worktrees/` (a thread's worktree is removed
when the thread is deleted, and only when it holds no uncommitted or
unbranched work).

## Removal

Delete the files above to remove state; no index has to be updated.

```sh
# one conversation
rm -r ~/.uagent/history/<workspace>/<session>.json*
# every conversation, capture and debug trace
rm -r ~/.uagent/history ~/.uagent/artifacts ~/.uagent/sessions
```

The first line takes the snapshot with its journal, uploads, undo data and
lock. Close the conversation first if its runtime is still running.

Rotate any credential that appeared in a prompt, attachment, tool result or
debug trace. Sharing the state directory can disclose repository content even
though its files are private.
