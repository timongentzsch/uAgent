# Operations

µAgent is a local single-user POSIX CLI for macOS and Linux, not an OS sandbox
or multi-tenant service.

## Bounds

| Concern | Default |
| --- | ---: |
| first event / stream idle | 300 / 300 s |
| request / complete turn | 600 / 3600 s |
| model rounds / tool calls | 100 / 100 |
| subagent depth / rounds / calls | 2 / 25 / 60 |
| reported turn cost | $1 |
| request / response | 64 / 32 MiB |
| source read / grep | 32 KiB / 200 matches |
| selected skill body / discovery depth | 512 KiB / 6 |
| tool result / batch | 8,000 / 16,000 characters |
| recent tool trace / prune batch | 64 / 32 KiB |
| background jobs / safe workers | 8 / 4 |
| memory file / files | 2 KiB / 32 per scope |
| memory always-on slice | 2 KiB |
| memory extraction | one session, 32 KiB, after 6 idle hours |
| attachment / terminal image | 10 / 10 MiB |
| input history / composer and bracketed paste | 200 x 16 KiB / 64 KiB |
| automatic compaction | 85% projected context |
| removed-trace archive | 16 MiB |

Files, edits, instructions, memories, skills, schemas, MCP traffic, shell logs,
and Python scratch scripts have additional fixed bounds in `RuntimeConfig` and
their owning modules. Raise a bound only with a representative measurement.
Set `UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS=0` to disable incremental pruning;
`UAGENT_TOOL_TRACE_PROTECT_CHARS` controls the recent-output budget.
Use `--no-memory` to remove memory recall and writes from the coordinator and
delegated children during reproducible runs. `UAGENT_MEMORY=0` is the equivalent
environment setting. `/memory` shows the active policy and saved keys without
calling a model.

The always-on slice inlines behavioral (global-scope) memory into the startup
context; set `UAGENT_MEMORY_ALWAYS_BYTES=0` to disable it entirely.
Set `UAGENT_MEMORY_GENERATE=0` to retain recall while disabling background
extraction. Each interactive startup claims at most one eligible session;
`UAGENT_MEMORY_IDLE_SECONDS` and `UAGENT_MEMORY_EXTRACT_BYTES` tune its bounded
input. Codex top-level memories and the current Claude project memory directory
are indexed read-only.

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

Commands, delegated tasks, and detached terminals share activity IDs.
`/ps` lists the current session's active work behind the status bar's `bg:N`.
`activity_output(id)` reads a bounded log without cancelling ownership;
`wait_ms` optionally blocks for output/exit and `until` requires fixed
readiness text. Use `task(background=false)` when the next step requires the
child result; background tasks notify the agent automatically on exit.
`activity_wait(ids, mode)` is available when no useful work remains and a join
is intentional. `activity_stop(id)` sends TERM, then KILL if needed, to the
complete process group and removes its record and rotating logs. Output is
limited to bytes the child has flushed. Persistent TUI and headless runs resume
after background completion without a second process watcher.

Configured MCP servers start once and expose their discovered tools directly.
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
queues guidance into the active turn at the next model/tool boundary; Escape
interrupts only the foreground request/tool batch and applies queued guidance
immediately. Terminal focus changes do not clear drafts or interrupt work;
multiline bracketed paste is normalized and inserted as one edit before Enter
submits it.
Background commands, delegated tasks, and detached terminals retain their
existing supervisor ownership.

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
