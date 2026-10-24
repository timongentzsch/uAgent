# Persistence

Where µAgent keeps local state, how long it stays, and how to remove it.
State is private (files `0600`, directories `0700`) but unencrypted, and can
contain prompts, model output, tool results, paths and usage.

## Locations

Paths are under `~/.uagent` unless shown otherwise.

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
| mail between sessions, panels and children | `mail/<id>/{new,cur}/*.json` |
| MCP server logs | `mcp/*` |
| provider model catalogues, one per endpoint and wire protocol, fresh for 24 hours; no credential is written or hashed into them | `models/<hash>.json` |
| memories | `memory/{global,projects/<repository>}/*.md` |
| memory audit and extraction claims | `memory/events.jsonl`, `memory/.processed/*` |
| a skill being imported from Claude Code or Codex, until it moves into `skills/` | `staging/<name>.<random>/` |
| scheduled tasks | `scheduled/state.json` |
| worktrees of scheduled runs and panel threads | `worktrees/<id>` |
| session links | `links/*` |
| web host discovery, devices and push keys | `web/*` |
| settings, for all conversations and per project folder | `config/settings.json` (and its `.lock`) |
| project trust, permission rules, tool categories, first-run welcome shown | `config/trusted-projects.json`, `config/permissions.json`, `config/tool-categories.json`, `config/welcomed` |
| your instructions | `AGENTS.md`, `PANEL.md` |
| your skills | `skills/*` |
| shared browser profile (saved logins, cookies) | the directory `UAGENT_BROWSER_DATA` names |
| runtime sockets and their leases | `/tmp/uagent-<uid>-<hash>/*.sock`, `*.sock.lock` |
| workspaces, and the name each goes by | `spaces/<folder>`, `spaces/<folder>/.uagent/name` |
| scratch scripts | `<workspace>/.uagent/scratch/*` |
| Playwright snapshots and logs | `<workspace>/.playwright-cli/*` |

`<workspace>` is a hash of the project folder's path; each session file's
header records its `cwd` and title. Live activities (IDs, PTYs, incremental
output) exist only in memory and end with the process.

No settings are read from a project folder. µAgent reads from it only
`.mcp.json` (once trusted), instruction files (`AGENTS.md`,
`.uagent/PANEL.md`) and skills (`.agents/skills`, `.uagent/skills`; see
[Bundled skills](../skills/README.md#discovery) for the rest).

## Settings

[Operations](OPERATIONS.md#settings) describes the settings document and how
to change, export and import it. On disk:

- µAgent writes `config/settings.json` under a lock and keeps
  `config/settings.schema.json` beside it for editors.
- A document that cannot be parsed is refused, never read as empty: the error
  names the file to fix or remove, and nothing is saved over it.
- The format is 3, written since v1.5.0; an earlier format is not read, and
  `uagent config import` takes format 3 only.

## Sessions

Find a conversation with `/sessions [PREFIX]`, `uagent --resume` or
`uagent -c` (the most recent). `/share` exports the transcript as Markdown.

A session file is a format-3 snapshot: active messages, the bounded archive
of compacted tool traces, usage, provider session identity, the system
directive, and `display` metadata (message IDs, readable reasoning, tool
status and timing, receipts, routes, usage and asset references) for
browsing history after context pruning. `custom_title` says whether the user
chose the title.

- A save validates the complete record, writes a private temporary file in
  the same directory, `fsync`s it and renames it into place. An interrupted
  write leaves the previous record intact.
- Only format 3 loads. Incompatible, incomplete or corrupt files are reported
  and left untouched; there is no implicit migration.
- One runtime holds the writer lease for a conversation. CLI and web clients
  attach to it without taking a lease, and independent conversations can
  share a workspace. The lease is the locked descriptor, not the lock file's
  existence.
- A successful, untruncated file read stores `_uagent_read_range`
  (`[path, first_line, last_line]`) in its tool message; wire requests omit
  it.

The event journal (`uagent.session.event.v1`) keeps at most 512 records or
256 KiB of turn, tool, capability, configuration, presentation and artifact
metadata. It never holds prompts, reasoning, answer text or full tool
results, is not replay authority, and a missing or corrupt journal does not
block loading.

`--debug` writes a separate, sensitive trace from which the exchange can be
reconstructed. Interactive model calls also keep private HTTP request and
response captures in `artifacts/` for `/http`; credential headers are
redacted, prompts and tool output are not.

## Activities and delegated children

Completed output larger than `UAGENT_TOOL_RESULT_CHARS` moves to
`artifacts/` instead of entering context whole, so a returned path can expire
with retention. Detached terminal records store a boot-scoped process
identity; a record whose identity is missing or does not match is treated as
exited and never signalled.

Each delegated child has a durable ID naming its session file, and a
follow-up starts a new supervised child from that saved conversation. Saved
conversations resume after a restart; live activities, PTYs and output
buffers do not.

## Mail

Sessions, a folder's panel and its threads, and delegated children talk
through one mailbox each, `mail/<id>/`, where `<id>` hashes the canonical
session file. A message is a private JSON file with a type
(`task.completed`, `ask`, `steer`, `note`, ...), sender, correlation id and
body.

- A send commits by an atomic rename into `new/`.
- The recipient moves what it takes to `cur/` and deletes it once the
  snapshot holding it is saved, so a crash in between delivers it again.
- The snapshot keeps the latest 256 delivered ids, so a message delivered
  twice enters the conversation once.
- Expired (24 h), unreadable and not-yet-permitted messages are never
  delivered.

## Memory

Memory Markdown files are the editable source of truth. They change only
through an explicit `memory` action or the single background extractor.
Codex memories in `~/.codex/memories` and the current Claude Code project
memory in `~/.claude/projects/<project>/memory` are read-only recall sources
for the agents named in `UAGENT_OTHER_AGENTS` (`claude`, `codex`; none by
default).

The audit log records only action, key, time, automatic source and a
redacted preview, which `/memory` shows. The extractor marks a session done
only after a successful run; a failed run releases its claim so a later run
can retry.

## Retention

Pruning runs whenever a top-level session's runtime starts. A file older
than the days shown is removed; of the rest, the newest files up to the
count are kept.

| Tree | Days / files kept |
| --- | --- |
| `history/`, `memory/.processed/` | `UAGENT_HISTORY_DAYS` (30) / 200 |
| `sessions/` | 14 / 50 |
| `mail/` | 14 (undelivered mail expires after a day) |
| `bg/`, `artifacts/` | 7 / 200 |
| `mcp/` | 7 / 100 |
| `models/` | 7 (a catalogue older than a day is fetched again) |
| exited detached terminals | 7, removed with their logs |
| `staging/` | 1, whole folders (only a killed import leaves any) |

To keep conversations longer, raise the days, for example
`/config user UAGENT_HISTORY_DAYS=180`. The 200-file count is fixed and
counts across all projects.

Delegated children age out with `history/`. A session's journal, undo data
and uploads age out with it and do not count against the file limit.
Orphaned journals are removed, and so are the socket and lease of a runtime
that was killed. Each process log is also bounded in size: 64 MiB for
commands, 16 MiB for an MCP server.

Not pruned: `config/`, memories, skills, scheduled tasks, the browser
profile, and `worktrees/` (a thread's worktree is removed when the thread is
deleted, and only when it holds no uncommitted or unbranched work).

## Removal

Delete the files above to remove state; no index has to be updated.

```sh
# one conversation, with its journal, uploads, undo data and lock
rm -r ~/.uagent/history/<workspace>/<session>.json*
# every conversation, capture and debug trace
rm -r ~/.uagent/history ~/.uagent/artifacts ~/.uagent/sessions
```

Close the conversation first if its runtime is still running.

Rotate any credential that appeared in a prompt, attachment, tool result or
debug trace. Sharing the state directory can disclose repository content
even though its files are private.
