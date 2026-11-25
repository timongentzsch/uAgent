# Budgets and recovery

This page limits what a conversation may spend, tunes timeouts, and gets
work back after a failure. How a turn checks its limits is in
[Turn loop](../architecture/turn-loop.md); retries and usage normalisation
are in [Providers](../architecture/providers.md); every default and range is
in [Settings](../interfaces/settings.md) and [Limits](../interfaces/limits.md).

## Read what was spent

```text
/cost
```

`/cost` (terminal) lists tokens and spend per route; the web shows the same
in a conversation's statistics. Every model call is charged to a route key
`authority|provider-or-purpose|model|effort`, so side requests appear under
their purpose: `compaction`, `image_analysis`, `permission_review` and
others. Tokens are split into input, cache read, cache write, output and
reasoning. Cost is only what the provider reports in `usage.cost`; µAgent
holds no price table and infers nothing. A route that reports no cost shows
`cost unavailable`. An agent's spend is written to
`~/.uagent/activity/usage-<session id>.jsonl` and counted in its parent's
usage and cost.

When the conversation's model cannot read images, an image is sent once to
the image route (`--image-model`), its bytes are replaced by the returned
description, and that call is charged to `image_analysis`.

## Set budgets and turn limits

```sh
uagent --budget 5 --token-budget 200000 -p "fix the failing test"
```

```text
/config user limits.maxTurnCost=2
```

| Setting | Flag | Ends |
| --- | --- | --- |
| `limits.sessionCost` (`UAGENT_SESSION_BUDGET`) | `--budget USD` | the turn once the conversation's reported cost passes it |
| `limits.sessionTokens` (`UAGENT_SESSION_TOKEN_BUDGET`) | `--token-budget N` | the turn once generated tokens reach it |
| `limits.maxTurnCost`, `limits.maxTurnTokens` | | a turn's reported cost or generated tokens |
| `limits.maxTurnSeconds`, `limits.maxSteps`, `limits.maxToolCalls` | | a turn's wall clock, model rounds or tool calls |
| `limits.repeatedCallStop` | | a turn after that many identical consecutive calls |

All are off at 0. Limits are captured when a turn starts; a change applies
at the next one. They are checked between model calls, so one response can
overrun what is left. At a limit the turn ends with a message such as
`session cost limit exceeded ($5.00)`, `turn time limit reached (600s)` or
`tool call limit reached (40)`. A turn stopped by steps, tool calls,
repetition or its deadline first gets one more model round with no tools,
whose prose becomes the answer. A headless run reports the cause in
`stop.reason` and exits 1 when no answer came
([Headless](../interfaces/headless.md)).

Token limits need reported usage, cost limits reported cost. Without them
the conversation warns once: `provider does not report usage; token budget
is not enforceable`, or, with a session budget set, `provider does not
report cost; dollar limits are not enforceable`. Usage is stored with the
conversation, so a session budget counts what earlier runs spent. Under a session budget
an agent gets the parent's remainder divided among the free `agents.max`
slots; with less than $0.01 left no agent starts.

## Timeouts and retries

| Setting | Bounds |
| --- | --- |
| `request.timeout` (`UAGENT_REQUEST_TIMEOUT`) | one model request attempt |
| `request.streamTimeout` (`UAGENT_STREAM_TIMEOUT`) | silence before the first stream event and between events |
| `tools.timeout` (`UAGENT_TOOL_TIMEOUT`) | one tool call; `run`, `activity` and `agent_start` are bounded by the turn |
| `mcp.timeout`, `webSearch.timeout` | one MCP call; one search request |
| `runtime.idleMinutes` (`UAGENT_IDLE_MINUTES`) | how long an idle runtime, and its Chrome, stays up |

A model request is tried at most 3 times (`kChatAttempts`, fixed), after
about 2 s and 4 s with jitter, longer when `Retry-After` says so. HTTP 408,
409, 429, 5xx, stalls and connection errors are retried, and only before
visible text, tool calls or usage arrived. A provider may already have
processed the failed attempt, so a retry can be billed twice. A context
length rejection is not retried: the turn compacts once and retries the
step; a second rejection ends the turn.

## What survives which failure

| Failure | Kept | Lost |
| --- | --- | --- |
| client closed (terminal, tab) | everything; the runtime keeps working and stops after `runtime.idleMinutes` with no client | nothing |
| runtime crash or kill | the last saved session (saved when a message enters and after each input) | the running tool call, activities, PTYs; the web host marks the turn `interrupted`; nothing is repeated |
| web host restart | runtimes (own systemd scope or session), paired devices, pins, drafts, saved hosts | event cursors (clients resync), command receipts, Chrome (the browser service exits with the host) |
| machine reboot | sessions, settings, memory, schedules, Chrome profiles | every process and `RuntimeDir()`; detached terminals are treated as exited |
| SSH loss | history on the main; remote commands and terminals keep running | nothing, unless the remote worker restarts ([Remote access](remote-access.md)) |

Undelivered mail taken before a crash is put back and delivered at the next
start. A move between folders interrupted by a crash is finished when the
web host starts ([Sessions](../architecture/sessions.md#failure-modes)).

## Recover a conversation

`uagent -c` continues the most recent conversation; `uagent
--resume` picks one; the web lists every saved one. Opening one starts its
runtime from the session file. A file that fails to load is left untouched
and reported: `session header is invalid`, `session payload is invalid or
incomplete`, `unsupported session format N`. `/fork [TITLE] [@TURN]` and
`/rewind [N]` branch a copy at an earlier turn. Old conversations are
pruned by `history.days` and `history.files`; raise them to keep more
([Sessions](../architecture/sessions.md#failure-modes)).

## Undo

```text
/undo          # every file the last turn changed
/undo src/a.cc # one of them
```

`/changes` lists the files; the web's turn review has an Undo per file.
Undo puts back the content a file had before the turn's first change, and
only while the file still holds what the turn left. Only file-tool edits
are recorded, not shell commands. A file over 2 MiB, or past 64 MiB per
conversation, is listed but cannot be undone.

## Debug traces

```sh
uagent --debug -p "…"                 # ~/.uagent/sessions/<UTC stamp>-<pid>.jsonl
uagent --debug=/tmp/run.jsonl -p "…"
```

`--debug` sets `logs.level` to debug for that run; the setting at debug
traces every runtime to the default path.

One JSON object per line: `seq`, `time`, `elapsed_ms`, `pid`, `event`,
`data`. `agent_init` holds the tool schemas and instruction sources;
`model_request` holds the full message list after any shrink and the new
messages per later step, so every request can be rebuilt; `model_response`
holds content, reasoning, tool calls, usage, timings, `http_status`,
`generation_id` and `served_by`. Other events record retries, compaction,
faults, approvals and tool starts. Child agents and the runtimes a run
starts append to the same file. The trace holds prompts, file content and
tool output: it is sensitive and is pruned with the log trees
(`logs.keepDays`). `/http` shows the captured HTTP attempts of a running
conversation without a trace.

## When it goes wrong

| Symptom | Look at |
| --- | --- |
| a headless run exits non-zero | stderr, or `error` and `stop` in the `--json` envelope |
| a turn ended early | the stop message; `stop.reason` headless; the limits above |
| spend shows `cost unavailable` | the provider sends no `usage.cost`; dollar limits do not apply |
| one upstream behind a router misbehaves | `served_by` in message statistics and `/cost`; `generation_id` in `/http` or the trace |
| a model call fails or hangs | `/http`; the `api_retry` events in a `--debug` trace |
| a command was refused a write | the `[sandbox: …]` line in its result; [Sandbox](../architecture/sandbox.md) |
| startup says `degraded, commands run unconfined` | the kernel lacks Landlock; [Sandbox](../architecture/sandbox.md) |
| an MCP server failed | `/mcp`, and `~/.uagent/mcp/<server>-<pid>.log` |
| the web host does not start | `journalctl --user -u uagent-web`, `~/Library/Logs/uagent-web.log`, or `docker compose … logs uagent` |
| a conversation will not open in the web | the reason from the runtime's boot, shown with the error; the session file |
| an SSH host fails | its details in Remote hosts, `~/.uagent/web/ssh-<id>.log` |
