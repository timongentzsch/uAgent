# Logging

This page lists what µAgent records about its own work: each record, the
level that writes it, where it lives, its format and how long it is kept.
Paths are under `~/.uagent`; the whole tree is in
[On-disk formats](../interfaces/on-disk.md). The session file itself is in
[Sessions](sessions.md).

## Responsibility

One table, `kLogRecords` (`include/core/logs.h`), names every record a
level decides, its files and the lowest level that writes it. Writers ask `Logs(kind)` and
nothing else. The level is the setting `logs.level`:

| Level | Writes |
| --- | --- |
| `off` | only what tools need to work |
| `events` | also the journal, diagnostics and MCP server stderr |
| `full` (default) | also HTTP captures and tool exchanges |
| `debug` | also the trace |

`--debug[=PATH]` sets the level to debug for that run and writes the trace
to `PATH`, or to the default path without one.

## Processes and threads

Every runtime (web conversation, terminal, headless, child agent) reads the
level when it starts and again before each message, so a change applies from
the next message. Only the trace is decided once: it starts with a process
at debug, and stops with it.

## Data and formats

| Record | Level | Path | Format | Read by |
| --- | --- | --- | --- | --- |
| Journal | events | `history/<name>.json.events.jsonl` | JSONL `uagent.session.event.v1`: metadata only ([Sessions](sessions.md#data-and-formats)) | people; never replayed |
| Diagnostics | events | `logs/<UTC day>.jsonl` | JSONL: `seq`, `time`, `elapsed_ms`, `pid`, `event` (the name), `data` with `level` (warning or error), `role`, `session_id`, `text`, `data` | people |
| MCP stderr | events | `mcp/<server>-<pid>.log` | the server's own text | the MCP status (`/mcp`), people |
| HTTP captures | full | `artifacts/http-XXXXXX` | request and response bodies of one model call | `/http`, the web's Raw view |
| Tool exchanges | full | `artifacts/exchange-XXXXXX` | JSON `{request, response, status, complete}` | the web's tool inspector |
| Trace | debug | `sessions/<UTC stamp>-<pid>.jsonl` or `PATH` | JSONL: `seq`, `time`, `elapsed_ms`, `pid`, `event`, `data` ([Debug traces](../operations/budgets-and-recovery.md#debug-traces)) | people |
| Large tool output | always | `artifacts/output-XXXXXX` | the full output a result points to | `read` |
| Command logs | always | `activity/<pid>-<activity>.log` | a command's output | the `activity` tool |

Diagnostics are the programs' own warnings and errors (`Diagnose`); their
text also goes to stderr at every level. The day file opens with the first
one, so a process that reports nothing writes nothing. The trace and the
captures hold prompts, file content and tool output: they are sensitive.
Credentials are never written: a capture's credential headers are redacted
(`CredentialLikeKey`), and the pairing code is not logged.

## Invariants and limits

- The journal is capped at 4 MiB (`kSessionJournalBytes`); past it an append
  first keeps the newest half. An MCP server's stderr is capped at 16 MiB
  (`kMcpLogBytes`), a command log at `activities.logMb`.
- Retention runs once a day (`MaintainArtifacts`). In `logs/`, `mcp/`,
  `artifacts/`, `sessions/` and `activity/` files older than `logs.keepDays`
  go. Then the oldest files a level writes (the table's records) go until
  together they fit `logs.keepMb`; what tools need to work (large tool
  output, command logs, a child's spend ledger, attached images) only ages.
  The journal goes with its session (`history.days`, `history.files`).

## Failure modes

- A capture or exchange the cleanup removed reads as "removed by the
  cleanup"; a tool exchange then shows what the transcript holds.
- A diagnostic that cannot open the day file still reaches stderr.
- A journal that cannot be written fails the save that wrote it, and the
  events wait for the next save.

## Source and tests

| Path | What |
| --- | --- |
| `include/core/logs.h`, `src/core/logs.cc` | levels and the record table |
| `src/core/events.cc` | the journal; `Observability::Diagnostic` |
| `src/core/debug.cc` | the trace and day-file sink; `Diagnose`, `DebugLog` |
| `src/core/fs.cc` | `MaintainArtifacts`, `AppendPrivateLine` |
| `tests/unit/events_test.cc` | levels, journal, diagnostics, retention |
| `tests/integration_runtime/sessions.py` | what a turn leaves on disk at each level |
