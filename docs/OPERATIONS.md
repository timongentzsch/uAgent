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
| memory file / files / session input | 2 KiB / 32 per scope / 64 KiB |
| memory always-on slice | 2 KiB |
| memory idle / eligible age | 6 hours / 10 days |
| attachment / terminal image | 10 / 10 MiB |
| checkpoint hint / urgent / emergency | 65 / 85 / 95% |
| removed-trace archive | 16 MiB |

Files, edits, instructions, memories, skills, schemas, MCP traffic, shell logs,
and Python scratch scripts have additional fixed bounds in `RuntimeConfig` and
their owning modules. Raise a bound only with a representative measurement.
Set `UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS=0` to disable incremental pruning;
`UAGENT_TOOL_TRACE_PROTECT_CHARS` controls the recent-output budget.
Use `--no-memory` to remove memory recall and writes from the coordinator and
delegated children during reproducible runs. `UAGENT_MEMORY=0` disables recall;
`UAGENT_MEMORY_GENERATE=0` keeps recall but prevents new contributions. `/memory`
shows the active policy and saved keys without calling a model.
Automatic consolidation uses the current route, consumes one supervised
background slot, and can incur provider usage. It scans at most one eligible
session on interactive startup. Tune its eligibility with
`UAGENT_MEMORY_IDLE_SECONDS` and `UAGENT_MEMORY_SESSION_DAYS`, and its filtered
input cap with `UAGENT_MEMORY_SESSION_BYTES`. Completed maintenance output is
discarded instead of being injected into the next model request.

The always-on slice inlines behavioral (global-scope) memory into the startup
context; set `UAGENT_MEMORY_ALWAYS_BYTES=0` to disable it entirely.

Child processes are credential-sanitized. Approved shell commands alone may
receive explicitly named variables through `UAGENT_SHELL_ENV_ALLOW`. Project
`.uagent/.config` and `.mcp.json` require trust.

Dollar limits are enforced between calls and require provider-reported
`usage.cost`; output marks missing cost as unavailable. One in-flight request
may cross the remaining allowance. Budgeted delegation runs one child at a time
with the remaining allowance.

Commands, delegated tasks, and detached terminals share activity IDs.
`activity_output(id)` reads a bounded log without cancelling ownership;
`wait_ms` optionally blocks for output/exit and `until` requires fixed
readiness text. Use `task(background=false)` when the next step requires the
child result; background tasks notify the agent automatically on exit.
`activity_wait(ids, mode)` is available when no useful work remains and a join
is intentional. `activity_stop(id)` sends TERM, then KILL if needed, to the
complete process group and removes its record and rotating logs. Output is
limited to bytes the child has flushed. Persistent TUI and headless runs resume
after background completion without a second process watcher.

For configured MCP servers, `mcp_status` reports ready/inactive/error state,
PID, discovered tool count, and the last lifecycle error; `mcp_restart`
restarts and handshakes a named server. These generic tools are omitted when
the built-in Chrome server is the only MCP configuration: `chrome_session`
already switches its mode/toolset and performs a page-level health probe.

The interactive composer owns stdin for the whole session. Enter during work
queues guidance into the active turn at the next model/tool boundary; Escape
interrupts only the foreground request/tool batch and applies queued guidance
immediately.
Background commands, delegated tasks, detached terminals, and memory
maintenance retain their existing supervisor ownership.

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
archive, one real turn per supported route, Chrome isolated and user attach,
one private debug trace, and relevant [`uagent eval`](TESTING.md) scenarios.
`install.sh` defaults to four build workers; set `UAGENT_BUILD_JOBS` for the
host when a different resource limit is appropriate.

The composite Action verifies a release SHA-256 and runs `--yolo --json` with
a reported-cost budget. It requires provider credentials in the job environment
and a trusted checkout. Never expose secrets while running untrusted fork code;
pin both the Action ref and release version.

## Failure triage

- Headless failures use nonzero exit status and a complete JSON envelope.
- MCP logs live under `~/.uagent/mcp`; debug traces are opt-in and sensitive.
- `chrome_session` succeeds only after a page-level health probe; a handshake
  without a selectable page is reported as unavailable.
- Corrupt sessions are reported and left untouched.
- Route-profile mismatches self-heal and should trigger recertification.
- Managed processes are reaped on catchable exits; `SIGKILL` cannot guarantee
  cleanup.

Local state and removal paths are listed in [PERSISTENCE.md](PERSISTENCE.md).
