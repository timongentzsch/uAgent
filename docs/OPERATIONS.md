# Operations

Limits, budgets, recovery behavior and failure triage for operators. µAgent is
a local, single-user POSIX CLI for macOS and Linux; it confines the commands it
runs (see [SECURITY.md](../SECURITY.md)) but is not a container or a
multi-tenant service.

Every `UAGENT_*` setting, its default and when a change takes effect is listed
in the generated
[configuration reference](../skills/uagent-config/references/configuration.md).
`/debug-config [SETTING]` shows which layer supplied each active value;
`/context` shows the redacted route, its capabilities and the schemas currently
advertised.

## Limits

A setting of `0` disables the corresponding limit. Rows without a setting are
fixed in the binary.

| Concern | Default | Setting |
| --- | --- | --- |
| first event / stream idle / request | 300 s / 300 s / 600 s | `UAGENT_FIRST_EVENT_TIMEOUT`, `UAGENT_STREAM_IDLE_TIMEOUT`, `UAGENT_REQUEST_TIMEOUT` |
| request / response size | 64 / 32 MiB | `UAGENT_REQUEST_BYTES`, `UAGENT_RESPONSE_BYTES` |
| turn wall clock, rounds, tool calls | unlimited | `UAGENT_MAX_TURN_SECONDS`, `UAGENT_MAX_STEPS`, `UAGENT_MAX_TOOL_CALLS` |
| turn generated tokens / reported cost | unlimited | `UAGENT_MAX_TURN_TOKENS`, `UAGENT_MAX_TURN_COST` |
| session generated tokens / reported cost | unlimited | `UAGENT_SESSION_TOKEN_BUDGET` (`--token-budget`), `UAGENT_SESSION_BUDGET` (`--budget`) |
| one tool call | 30 s; `run`, `scratch`, `activity` are bounded by the turn | `UAGENT_TOOL_TIMEOUT` |
| tool result / parallel batch | 8,000 / 16,000 characters | `UAGENT_TOOL_RESULT_CHARS` (batch is twice the result cap) |
| `read_path` | 1,000 lines, at most 10,000 lines or 32 KiB | `UAGENT_READ_FILE_LINES`, `UAGENT_READ_FILE_MAX_LINES`, `UAGENT_READ_FILE_BYTES` |
| `grep` | 200 matches | `UAGENT_GREP_RESULTS` |
| trace pruning | newest 64 KiB protected; results from 32 KiB pruned | `UAGENT_TOOL_TRACE_PROTECT_CHARS`, `UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS` |
| automatic compaction | 85% of projected context | `UAGENT_AUTO_COMPACT_PCT`, `UAGENT_AUTO_COMPACT_TOKENS` |
| compacted-trace archive | 16 MiB | `UAGENT_SESSION_ARCHIVE_BYTES` |
| background jobs / foreground tool workers | 8 / 4 | `UAGENT_MAX_BACKGROUND_JOBS`, `UAGENT_TOOL_CONCURRENCY` |
| `run` initial wait | 10 s; `yield_ms` 250–30,000, or 0 to wait for exit | `UAGENT_RUN_YIELD_MS` |
| activity output buffer | 1 MiB, equal head and tail | |
| retained finished activities | 16 | |
| subagent completion in context | 6,000 characters each, 12 KiB per batch | |
| delegation depth / rounds / tool calls per child | 2 / 100 / 240 | `UAGENT_SUBAGENT_DEPTH`, `UAGENT_SUBAGENT_MAX_STEPS`, `UAGENT_SUBAGENT_MAX_TOOL_CALLS` |
| subagent launches per turn / persistent sidekicks | 32 / 3 | `UAGENT_SUBAGENT_CALLS_PER_TURN`, `UAGENT_PERSISTENT_MAX` |
| memory size / count per scope / always-on slice | 2 KiB / 32 / 2 KiB | `UAGENT_MEMORY_BYTES`, `UAGENT_MEMORY_FILES`, `UAGENT_MEMORY_ALWAYS_BYTES` |
| memory extraction | 32 KiB of one session after 6 idle hours | `UAGENT_MEMORY_EXTRACT_BYTES`, `UAGENT_MEMORY_IDLE_SECONDS` |
| memory event audit | 256 KiB | |
| skill body / discovery depth | 512 KiB / 6 directories | `UAGENT_SKILL_BYTES` |
| CLI attachment / web upload | 10 / 8 MiB | `UAGENT_ATTACHMENT_MB` |
| input history / paste | 200 entries of 16 KiB / 64 KiB | |
| MCP call / optional-server startup | 60 s / 2 s shared | `UAGENT_MCP_TIMEOUT`, `UAGENT_MCP_STARTUP_GRACE` |
| macOS sandbox profile | 64 KiB; larger profiles refuse the command | |

Raise a bound only with a representative measurement.

## Budgets and accounting

- Budgets are checked between model calls, so one in-flight response can
  overrun the remaining allowance.
- Cost limits require provider-reported `usage.cost`. µAgent never infers cost;
  when a provider omits usage or cost, output marks it unavailable and warns
  that the corresponding limit cannot be enforced.
- The session token budget survives resume.
- A delegated child receives only the coordinator's remaining session
  allowance. While a session budget is set, children run one at a time.
  Child usage is charged to the parent by delta, so follow-ups do not reset
  accounting.
- Before a response reports usage, context size is estimated as serialized
  request bytes divided by 4 and labelled `est. ctx`. The estimate drives
  compaction and request guards, never billing.
- Compaction reserves a quarter of the context window for the response, or
  `UAGENT_MAX_TOKENS` when that is smaller.

## Recovery

- **Transient provider errors.** Overload, rate-limit, resource-exhaustion,
  timeout and unavailable errors, and HTTP 408, 409, 429 and 5xx, get three
  attempts. Backoff is exponential with jitter, from 500 ms up to 8 s; a valid
  `Retry-After` is treated as the minimum delay. Chat retries stop at the turn
  deadline and side requests at their tool deadline. A stream is replayed only
  before visible text, tool calls, annotations or usage arrive. A provider may
  already have processed a failed request, so a retry can repeat upstream cost.
- **Context overflow.** The first context-length rejection in a turn lowers
  the learned context window, compacts once and retries the step. If
  compaction fails, or the provider rejects again, the turn ends.
- **Repeated calls.** The third identical valid tool call adds an advisory, the
  sixth a direct instruction to change course, and the twelfth stops the turn.
  A call rejected by schema or policy stops after three equivalent attempts.
  Repeated no-change `activity` polls of one activity get advice after two,
  a direct instruction after four, and stop after twelve.
- **Out-of-range pacing arguments.** `run` `yield_ms` and `max_output_chars`,
  `grep` `context`, `activity` `wait_ms` and `max_output_chars`, and the
  `subagent` limits are clamped to their bound, and the result starts with a
  note such as `[clamped context to 10 of 40 requested]`. Every other
  out-of-range argument is rejected with the value and the limit.

## Activities and delegation

`run` returns a still-running command as an activity after its initial wait.
`activity` polls, waits, writes, resizes and stops activities; see
[TOOLS.md](TOOLS.md#activities). Commands, delegated tasks and detached
terminals share activity IDs, and `/ps` lists them behind the status bar's
`bg:N`.

- Session activities have opaque IDs and live only in memory. Detached
  (`detach=true`) activities are PID-backed with a rotating log; a record is
  reused or signalled only when its boot-scoped process identity still
  matches. Launching the same detached command from the same directory reuses
  its process group. Detached activities cannot be reattached interactively
  after µAgent exits.
- Output already returned by `run` or `activity` is not delivered again.
  Command completion appears only in the UI and never starts a model turn.
  Subagent completion is added once to the next model call without starting
  one.
- Log readers wait on kqueue (macOS) or inotify (Linux) and fall back to
  bounded polling elsewhere.
- `stop` sends TERM, then KILL, to the whole process group and removes its
  records and logs.
- A failed child reports its route, failure stage, bounded diagnostics and a
  remedy. µAgent never silently changes provider, model, pricing or privacy
  policy for a child.

## Interactive control

Enter during a turn queues guidance for the next model or tool boundary; a
passive `activity` wait returns at once without cancelling its activity.
Escape interrupts the foreground request or tool batch. Ctrl+B moves a running
foreground tool batch to background supervision without restarting it. Focus
changes do not clear drafts, and a bracketed paste is inserted as one edit.

## Configuration reload

Config files are re-read at user-turn boundaries. Settings marked
`next-user-turn` apply at the next prompt; the rest are reported as
restart-required. Command-line flags and process variables always shadow the
files. Reload never changes an in-flight turn. Secrets never appear in
inspection output.

## Memory

`--no-memory` (or `UAGENT_MEMORY=0`) disables recall and writes for the
coordinator and its children, for reproducible runs. `UAGENT_MEMORY_GENERATE=0`
keeps recall but disables background extraction. Each interactive startup
extracts from at most one idle session; `UAGENT_MEMORY_MODEL` runs extraction
on a cheaper route. The always-on slice inlines global memories newest first,
whole entries only, and startup warns when the cap drops one. Codex top-level
memories and the current Claude Code project memory are indexed read-only.
`/memory` lists memories and the latest automatic change from the audit log.

## Images

Set `UAGENT_IMAGE_MODEL` to a vision-capable route when the main route is
text-only. When the main route cannot read an image, µAgent sends it once to
the image route (by default the shared default route), removes the image bytes
from the conversation and keeps the textual description.

## MCP

Configured stdio servers start once and expose their tools directly. µAgent
requires the stateless `2026-07-28` protocol and rejects servers that do not
advertise it.

- Servers are required by default: a startup, discovery or tool-list failure
  stops bootstrap. Servers marked `"required": false` share the
  `UAGENT_MCP_STARTUP_GRACE` window, and a slow one finishes discovery at a
  later turn boundary without delaying model steps.
- The workspace is the default root. `UAGENT_MCP_ROOTS` (colon-separated) or a
  server's `roots` array in `.mcp.json` replaces it; per-server paths are
  relative to that file. Roots are cooperative protocol scope, not a sandbox:
  servers run with the user's permissions, and `--yolo` does not change them.
- `roots/list` is answered through `input_required` continuations, at most
  eight per call. Tool-list changes arrive over one `subscriptions/listen`
  stream per server. A timed-out request sends `notifications/cancelled`.
- Server stderr goes to a rotating log under `~/.uagent/mcp/`, bounded by
  `UAGENT_MCP_LOG_BYTES`. Errors without text point to that log.
- Not supported: HTTP transport, sampling, elicitation, task extensions,
  `$ref` and output-schema validation, and automatic restart of exited servers.

Browser automation is the `browser-use` skill over `playwright-cli` through
`run`, not MCP. Install it with `npm install -g @playwright/cli@latest`; use
`attach --cdp=chrome` only for a user-owned Chrome session.

## Release

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
UAGENT_PREFIX=/tmp/uagent-prefix ./install.sh
/tmp/uagent-prefix/bin/uagent --version
```

`install.sh` reuses `build/release` and builds only the installable target
with four jobs; set `UAGENT_BUILD_JOBS` to change that. Release archives
include the bundled skills and are checked by `tests/package_contents.py`. The
release job publishes a `SHA256SUMS` manifest with a keyless Sigstore bundle
and GitHub artifact attestations. Before tagging, verify the installed archive,
one real turn per supported wire API, both Playwright browsers, one debug trace
and the hermetic suite ([TESTING.md](TESTING.md)).

## Failure triage

- Headless failures exit nonzero with a complete JSON envelope.
- Debug traces (`--debug`) are opt-in and sensitive; see
  [ARCHITECTURE.md](ARCHITECTURE.md) for their contents.
- Corrupt sessions are reported and left untouched.
- Runtime shutdown reaps managed processes. Closing a client only detaches;
  `SIGKILL` cannot guarantee cleanup.
- A command refused a write reports the errno plus a `[sandbox: ...]` line.
  `/context` lists the writable roots; widen them with `UAGENT_SANDBOX_WRITE`,
  or run the one command with `run(sandbox=false)`, which always asks a person.
- On Linux the sandbox sets `no_new_privs`, so setuid binaries such as `sudo`
  or `ping` fail inside it. Privileged work needs unconfined execution: YOLO,
  `UAGENT_SANDBOX=0` or an approved `run(sandbox=false)`.
- Without Landlock, an explicit `UAGENT_SANDBOX=1` refuses every command; with
  the setting unset, commands run unconfined and startup reports `degraded`.
- `~/.uagent/terminals/logs` is not pruned by age because live detached
  terminals write there. A crash between creating a log and writing its record
  leaves a `pending-*` file to delete by hand.
- A Playwright attach needs a live, user-approved Chrome debugging endpoint;
  `playwright-cli list` shows the session and `detach` leaves Chrome running.

Local state and removal are described in [PERSISTENCE.md](PERSISTENCE.md).
