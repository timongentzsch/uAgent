# Operations

µAgent is a local single-user POSIX CLI for macOS and Linux, not an OS sandbox
or multi-tenant service.

## Bounds

| Concern | Default |
| --- | ---: |
| first event / stream idle | 300 / 300 s |
| request / complete turn | 600 / 3600 s |
| model rounds / tool calls | unlimited / unlimited (both configurable) |
| subagent depth / rounds / calls | 2 / 25 / 60 |
| reported turn cost | unlimited |
| request / response | 64 / 32 MiB |
| source read / grep | 32 KiB / 200 matches |
| selected skill body / discovery depth | 512 KiB / 6 |
| tool result / batch | 8,000 / 16,000 characters |
| recent tool trace / prune batch | 64 / 32 KiB |
| background jobs / safe workers | 8 / 4 |
| per-activity incremental output buffer | 1 MiB, equal head/tail |
| default public `run` yield | 10 seconds (`UAGENT_RUN_YIELD_MS`) |
| retained delivered activity sessions | 16, LRU |
| memory file / files | 2 KiB / 32 per scope |
| memory always-on slice | 2 KiB |
| memory extraction | one session, 32 KiB, after 6 idle hours |
| memory event audit | 256 KiB, compacted under a cross-process file lock |
| attachment / terminal image | 10 / 10 MiB |
| input history / composer and bracketed paste | 200 x 16 KiB / 64 KiB |
| automatic compaction | 85% projected model context |
| removed-trace archive | 16 MiB |

Files, edits, instructions, memories, skills, schemas, MCP traffic, shell logs,
and Python scratch scripts have additional fixed bounds in `RuntimeConfig` and
their owning modules. Raise a bound only with a representative measurement.
`UAGENT_MAX_TOOL_CALLS` optionally adds an aggregate per-turn tool-call budget;
zero leaves it unlimited. `UAGENT_AUTO_COMPACT_TOKENS` optionally adds an
absolute compaction threshold; zero relies on the model-relative percentage.
`UAGENT_MAX_TURN_COST` optionally adds a reported-cost limit per turn; zero
leaves it unlimited. `UAGENT_SESSION_BUDGET` independently limits cumulative
reported session cost when set to a positive amount.
Set `UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS=0` to disable incremental pruning;
`UAGENT_TOOL_TRACE_PROTECT_CHARS` controls the recent-output budget.
Use `--no-memory` to remove memory recall and writes from the coordinator and
delegated children during reproducible runs. `UAGENT_MEMORY=0` is the equivalent
environment setting. `/memory` shows the active policy and saved keys without
calling a model.

Set `UAGENT_ADAPT_SYSTEM=1` to expose the experimental `adapt_system` tool. It
lets the model replace or clear a bounded free-form mutable section of message
zero at any model/tool boundary. Revisions persist until changed, including
across session save/resume, while permissions, tool availability, approval,
and resource limits remain host-enforced. Each revision and the next complete
request snapshot are recorded by `--debug`; leave it disabled for a static
prompt control run. A revision is intended for a concrete task observation
that changes subsequent strategy, not as an automatic first-step plan or a
restatement of the existing workflow; the tool's `reason` records both the
observation and the resulting strategic delta.

The always-on slice inlines behavioral (global-scope) memory into the startup
context; set `UAGENT_MEMORY_ALWAYS_BYTES=0` to disable it entirely.
Set `UAGENT_MEMORY_GENERATE=0` to retain recall while disabling background
extraction. Each interactive startup claims at most one eligible session;
`UAGENT_MEMORY_IDLE_SECONDS` and `UAGENT_MEMORY_EXTRACT_BYTES` tune its bounded
input. Codex top-level memories and the current Claude project memory directory
are indexed read-only.

Automatic extraction appears as a semantic `[memory]` activity rather than its
shell wrapper. On completion, created and updated memories (and failures) enter
UI scrollback but not model context. `/verbose` also shows unchanged/no-change
receipts. `/memory` lists the latest action, timestamp, automatic source, and a
redacted 160-character preview from the private bounded `events.jsonl` audit.

Set `UAGENT_IMAGE_MODEL` to a vision-capable model on the active provider when
the primary route is text-only. When direct image input is known to be
unavailable or the primary route first rejects it, µAgent sends the attachment
once to that model, removes the image bytes from the parent context, and
delivers the resulting textual visual evidence. Without this setting, rejected
images remain available only by their local paths.

Child processes are credential-sanitized. Approved shell commands alone may
receive explicitly named variables through `UAGENT_SHELL_ENV_ALLOW`. Project
`.uagent/.config` and `.mcp.json` require trust.

Dollar limits are enforced between calls and require provider-reported
`usage.cost`; output marks missing cost as unavailable. One in-flight request
may cross the remaining allowance. Budgeted delegation runs one child at a time
with the remaining allowance.

Debug model-response records expose `request_preparation_ms`, `end_to_end_ms`,
`dns_ms`, `connect_ms`, `tls_ms`, `pretransfer_ms`, `start_transfer_ms`,
`first_event_ms`, and `duration_ms`. Trace records are queued in sequence and
serialized/flushed by a background writer; shutdown drains the queue.

Commands, delegated tasks, and detached terminals share activity IDs.
Session-lifetime activities use opaque IDs distinct from OS PIDs; persistent
detached records remain PID-backed. `/ps` lists active work behind the status
bar's `bg:N`. Launching the exact same detached command again from the same
working directory reuses its process group.

`run` waits 10 seconds by default before returning a still-running command as
an activity; `UAGENT_RUN_YIELD_MS` changes that default. Explicit `yield_ms=0`
waits to the turn deadline, while 250 through 30,000 select another initial
wait. Set `tty=true` only when the process needs interactive input. A PTY
activity retains merged output, writable input, process-group interruption, and resize support.
Persistent `detach=true` activities remain rotating-log based and cannot be
interactively reattached after the harness exits.

`activity_output(id)` drains bounded new output without cancelling ownership;
`wait_ms` optionally blocks for output or exit and `until` waits for fixed
readiness text. `activity_input(id, chars)` writes raw PTY bytes; empty input
polls, `\u0003` interrupts the process group, and `rows` plus `cols` resize the
PTY. Ordinary writes to non-TTY activities are rejected. Ordinary and PTY
activities share one event-driven output/reap thread, so
`activity_output`, `activity_input`, foreground yielding, and `activity_wait`
use notifications rather than log polling. Interactions against one activity
are serialized. Incremental output uses a 1 MiB equal head/tail buffer, while
private logs continue to support diagnostics and large-output artifacts.
`max_output_chars` can lower the host cap for one `run`, output, input, or wait
interaction.

Use `task(background=false)` when the next step requires the child result;
background tasks notify the agent automatically on exit. `activity_wait(ids,
mode)` is an intentional join when no useful parent work remains.
`activity_stop(id)` sends TERM, then KILL if needed, to the complete process
group and removes its records and logs. Persistent TUI and headless runs resume
automatically after background completion.

Configured MCP servers start once and expose their discovered tools directly.
When a server reports an error without diagnostic text, the result points to
that server's private stderr log under `~/.uagent/mcp/`.
µAgent advertises MCP roots and answers `roots/list`; the current workspace is
the default root. Set
colon-separated `UAGENT_MCP_ROOTS`, or a server's
`roots` string array in `.mcp.json`, to authorize a different bounded set.
Per-server paths are relative to that configuration file and override the
global default. `--yolo` controls tool approval, not an MCP server's root
boundary. Roots are cooperative protocol scope, not an OS sandbox; MCP servers
still run with the user's process permissions.

Browser automation is a deferred `browser-use` skill over `playwright-cli`, not
MCP. CLI calls use the existing approved `run` tool and a per-process session;
Playwright keeps its browser daemon alive between calls and writes snapshots
outside model context. Install it once with
`npm install -g @playwright/cli@latest`. Use `attach --cdp=chrome` only for a
user-owned Chrome session; otherwise use an isolated `open` session.

The interactive composer owns stdin for the whole session. Enter during work
queues guidance into the active turn at the next model/tool boundary. While a
foreground command is running, Ctrl+B transfers the complete foreground tool
batch to background supervision without signaling or restarting it, so queued
guidance can apply immediately. Escape interrupts only the foreground
request/tool batch and applies queued guidance. Terminal focus changes do not
clear drafts or interrupt work; multiline bracketed paste is normalized and
inserted as one edit before Enter submits it. Background commands, delegated
tasks, and detached terminals retain their supervisor ownership.

The active and conditional tool inventory is listed in [TOOLS.md](TOOLS.md).
Use `/context` for the exact schemas currently advertised after policy, route,
state, skill, delegation, and MCP filtering.

## Release

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
UAGENT_BUILD_DIR=/tmp/uagent-build UAGENT_PREFIX=/tmp/uagent-prefix ./install.sh
/tmp/uagent-prefix/bin/uagent --version
```

CI adds warnings-as-errors, sanitizers, TSan, parser fuzzing, coverage, Google
C++ style, and native Linux/macOS builds. Before a tag, verify the installed
archive, one real turn per supported route, Playwright isolated and user attach,
one private debug trace, and the hermetic suite.
`install.sh` defaults to four build workers; set `UAGENT_BUILD_JOBS` for the
host when a different resource limit is appropriate.

The composite Action verifies a release SHA-256 and runs `--yolo --json` with
a reported-cost budget. It requires provider credentials in the job environment
and a trusted checkout. Never expose secrets while running untrusted fork code;
pin both the Action ref and release version.

## Failure triage

- Headless failures use nonzero exit status and a complete JSON envelope.
- MCP logs live under `~/.uagent/mcp`; debug traces are opt-in and sensitive.
- A Playwright attach requires a live user-approved Chrome debugging endpoint;
  `playwright-cli list` shows the active session and `detach` preserves Chrome.
- Corrupt sessions are reported and left untouched.
- Managed processes are reaped on catchable exits; `SIGKILL` cannot guarantee
  cleanup.

Local state and removal paths are listed in [PERSISTENCE.md](PERSISTENCE.md).
