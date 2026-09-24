# Persistence

Where µAgent keeps local state, how it is written and retained, and how to
remove it. State is private (owner-only files) but unencrypted, and records can
contain prompts, model output, tool results, paths and usage.

## Locations

Paths under `~/.uagent` unless shown otherwise.

| State | Location |
| --- | --- |
| sessions | `history/<workspace>/*.json` |
| session event journals | `<session>.json.events.jsonl` |
| session writer lease | `<session>.json.lock` |
| web uploads | `<session>.json.assets/*` |
| debug traces | `sessions/*.jsonl` |
| captured large outputs and HTTP exchanges | `artifacts/*` |
| background command logs | `bg/*` |
| detached terminal records and logs | `terminals/<pid>.json`, `terminals/logs/*` |
| collaborators | `collaborators/agent-<id>.json`, `.session.json`, `.comms.jsonl`, and pending `.mail-*.json` |
| MCP server logs | `mcp/*` |
| memories | `memory/{global,projects/<repository>}/*.md` |
| memory audit and extraction claims | `memory/events.jsonl`, `memory/.processed/*` |
| scheduled tasks and their worktrees | `scheduled/state.json`, `worktrees/<id>` |
| session links | `links/*` |
| web host discovery, devices and push keys | `web/*` |
| project trust, model preference, permission rules, tool categories | `config/trusted-projects.json`, `config/model-preference.json`, `config/permissions.json`, `config/tool-categories.json` |
| configuration | `.config`, and `<workspace>/.uagent/.config` |
| system prompt overrides | `system-prompt.json`, and `<workspace>/.uagent/system-prompt.json` |
| scratch scripts | `<workspace>/.uagent/scratch/*.py`, `*.sh` |
| Playwright snapshots and logs | `<workspace>/.playwright-cli/*` |

Session activities (IDs, PTYs, incremental output) exist only in memory and
end with the process.

## Sessions

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
  (`[path, first_line, last_line]`) in its tool message so superseded-read
  pruning works after resume. Wire requests omit it.

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

## Activities and collaborators

Completed output larger than `UAGENT_TOOL_RESULT_CHARS` moves to `artifacts/`
instead of entering context whole, so a returned path can expire with
retention. Detached terminal records store a boot-scoped process identity; a
record whose identity is missing or does not match is treated as exited and
never signalled.

Each collaborator has a durable ID. A follow-up starts a new supervised child
from the collaborator's snapshot; a `persistent=true` collaborator keeps its
worker and processes alive until stopped or until the parent exits. Queued
messages persist until a follow-up launch succeeds. A saved conversation
resumes after a restart, but live activities, PTYs and output buffers do not.

## Memory

Memory Markdown files are the editable source of truth. They change only
through an explicit `memory` action or the single background extractor.
Codex memories in `~/.codex/memories` and the current Claude Code project
memory in `~/.claude/projects/<project>/memory` are read-only recall sources.

The audit log records only action, key, time, automatic source and a redacted
preview, which `/memory` shows. The extractor marks a session done only after
a successful run; a failed run releases its claim at once so a later run can
retry.

## Retention

Pruning runs at startup:

| Tree | Settings (days / files) |
| --- | --- |
| `history/`, `memory/.processed/` | `UAGENT_HISTORY_DAYS` 30 / `UAGENT_HISTORY_FILES` 200 |
| `sessions/`, `collaborators/` | `UAGENT_DEBUG_DAYS` 14 / `UAGENT_DEBUG_FILES` 50 |
| `bg/`, `artifacts/` | `UAGENT_BG_DAYS` 7 / `UAGENT_BG_FILES` 200 |
| `mcp/` | `UAGENT_MCP_LOG_DAYS` 7 / `UAGENT_MCP_LOG_FILES` 100 |
| exited detached terminals | `UAGENT_TERMINAL_DAYS` 7, removed with their logs |

Collaborator metadata and sessions are pruned together. Journals whose session
is gone are removed. Each log is also bounded in size by `UAGENT_BASH_LOG_BYTES`
or `UAGENT_MCP_LOG_BYTES`.

## Removal

Delete the files above to remove state. Rotate any credential that appeared in
a prompt, attachment, tool result or debug trace. Sharing the state directory
can disclose repository content even though its files are private.
