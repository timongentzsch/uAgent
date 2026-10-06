# Operations

How to inspect and change settings, cap what a session may spend, read the
limits, and triage failures. µAgent is a local, single-user POSIX CLI for
macOS and Linux; it confines the commands it runs (see
[SECURITY.md](../SECURITY.md)) but is not a container or a multi-tenant
service.

## Settings

Settings are saved by µAgent in one private document,
`~/.uagent/config/settings.json`. It is not a file to edit: change it with
`/config`, the web's Settings, or the agent's `uagent` tool (which asks you to
approve the exact change).

```text
/config                                  what is set, and where from
/config user UAGENT_MAX_TURN_COST=2      save for all conversations
/config project UAGENT_TOOL_TIMEOUT=120  save for this project folder
/config conversation UAGENT_APPROVAL=auto
/config user unset UAGENT_MAX_TURN_COST
/config project reset                    clear a scope (keeps secrets)
/debug-config UAGENT_SANDBOX             one setting, scope by scope
/context                                 route, capabilities, advertised schemas
```

Five scopes can hold a value; the later wins: All conversations, This
project, Environment (`UAGENT_*` variables of the process), Command line, and
This conversation (its model and approval mode only). `/config` names the scope
each value comes from. Secrets never appear in inspection output.

To move settings to another host, or back them up:

```sh
uagent config export > settings.json   # everything saved, API keys included
uagent config import settings.json     # replaces everything saved; - is stdin
```

The export holds credentials in clear text; treat the file as a secret.

A changed setting applies from your next message when the
[configuration reference](../skills/uagent-config/references/configuration.md)
marks it `next-user-turn`. A `restart-required` one (the route and
credentials, the sandbox, the tool set, memory on or off, MCP) is reported as
such; `/restart` applies it to the running conversation. A turn in flight never
changes. That reference lists every setting with its default.

## Limits

A setting of `0` disables the corresponding limit. Rows without a setting are
fixed in the binary.

| Concern | Default | Setting |
| --- | --- | --- |
| stream silence (before the first event or between events) / one request attempt | 300 s / 600 s | `UAGENT_STREAM_TIMEOUT`, `UAGENT_REQUEST_TIMEOUT` |
| request / response size | 64 / 32 MiB | |
| turn wall clock, rounds, tool calls | unlimited | `UAGENT_MAX_TURN_SECONDS`, `UAGENT_MAX_STEPS`, `UAGENT_MAX_TOOL_CALLS` |
| turn generated tokens / reported cost | unlimited | `UAGENT_MAX_TURN_TOKENS`, `UAGENT_MAX_TURN_COST` |
| session generated tokens / reported cost | unlimited | `UAGENT_SESSION_TOKEN_BUDGET` (`--token-budget`), `UAGENT_SESSION_BUDGET` (`--budget`) |
| one tool call | 30 s; `run`, `scratch`, `activity` and `subagent` are bounded by the turn | `UAGENT_TOOL_TIMEOUT` |
| tool result / parallel batch | 8,000 / 16,000 characters | `UAGENT_TOOL_RESULT_CHARS` (batch is twice the result cap) |
| `read_path` | 1,000 lines, at most 10,000 lines or 32 KiB | `UAGENT_READ_FILE_LINES` (default only) |
| `grep` | 200 matches | |
| `web_search` | 4 calls per turn, 180 s each | |
| trace pruning | newest 64 KiB protected; results from 32 KiB pruned | |
| automatic compaction | 85% of projected context | `UAGENT_AUTO_COMPACT_PCT` |
| compacted-trace archive | 16 MiB | |
| background jobs / foreground tool workers | 8 (6 open to subagents) / 4 | |
| `run` initial wait | 10 s; `yield_ms` 250–30,000, or 0 to wait for exit | |
| activity output buffer | 1 MiB, equal head and tail | |
| retained finished activities | 16 | |
| subagent completion in context | 6,000 characters each, 12 KiB per batch | |
| delegation depth / rounds / tool calls per child | 2 / 100 / 240 | `UAGENT_SUBAGENT_DEPTH`, `UAGENT_SUBAGENT_MAX_STEPS`, `UAGENT_SUBAGENT_MAX_TOOL_CALLS` |
| wall clock per child | the turn | `UAGENT_SUBAGENT_TIMEOUT` |
| subagent launches per turn | 32 | |
| coordinator threads at once / reported cost per day | 5 / $20 | `UAGENT_COORDINATOR_MAX_THREADS`, `UAGENT_COORDINATOR_DAILY_SPEND_USD` |
| idle session runtime | stops after 15 minutes; the next message starts it | |
| undo journal | 2 MiB per file, 64 MiB per session | |
| memory size / count per scope / always-on slice | 2 KiB / 32 / 2 KiB | |
| memory extraction | 32 KiB of one session after 6 idle hours | |
| memory event audit | 256 KiB | |
| skill body / discovery depth | 512 KiB / 6 directories | |
| CLI attachment / web upload | 10 / 8 MiB | `UAGENT_ATTACHMENT_MB` |
| input history / paste | 200 entries of 16 KiB / 64 KiB | |
| MCP call / optional-server startup | 60 s / 2 s shared | `UAGENT_MCP_TIMEOUT` (call only) |
| macOS sandbox profile | 64 KiB; larger profiles refuse the command | |

Raise a bound only with a representative measurement.

## Budgets and accounting

To cap what a session may spend, pass a budget or save a limit:

```sh
uagent --budget 5 --token-budget 200000 -p "fix the failing test"
```

```text
/config user UAGENT_MAX_TURN_COST=2
```

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
  attempts. The waits are about 2 s, then 4 s, each shortened by up to a
  quarter of jitter; a valid `Retry-After` is treated as the minimum delay.
  `UAGENT_REQUEST_TIMEOUT` caps one attempt; chat retries stop at the turn
  deadline and side requests at their tool deadline. A stream is replayed only
  before visible text, tool calls, annotations or usage arrive. A provider may
  already have processed a failed request, so a retry can repeat upstream
  cost.
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
terminals share activity IDs. `/ps` lists them, `/ps ID output` shows one and
`/ps ID stop` ends it; the status bar counts them as `bg:N`.

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
- A subagent approves its own tool calls and runs its commands under the
  sandbox of the session that delegated to it; approving the delegation is the
  gate.
- A failed child reports its route, failure stage, bounded diagnostics and a
  remedy. µAgent never silently changes provider, model, pricing or privacy
  policy for a child.

## Interactive control

| Key | During a turn |
| --- | --- |
| Enter | queues guidance for the next model or tool boundary; a passive `activity` wait returns at once without cancelling its activity |
| Escape | interrupts the foreground request or tool batch |
| Ctrl+B | moves a running foreground tool batch to background supervision without restarting it |

Focus changes do not clear drafts, and a bracketed paste is inserted as one
edit.

## Memory

- `--no-memory` (or `UAGENT_MEMORY=0`) disables recall and writes for the
  coordinator and its children, for reproducible runs.
- `UAGENT_MEMORY_GENERATE=0` keeps recall but disables background extraction.
- `UAGENT_MEMORY_MODEL` runs extraction on a cheaper route. Each interactive
  startup extracts from at most one idle session.
- `UAGENT_OTHER_AGENTS=claude,codex` also indexes, read-only, Codex top-level
  memories and the current Claude Code project memory.
- `/memory` lists memories and the latest automatic change from the audit log.

The always-on slice inlines global memories newest first, whole entries only,
and startup warns when the cap drops one.

## Images

Set `UAGENT_IMAGE_MODEL` to a vision-capable route when the main route is
text-only. When the main route cannot read an image, µAgent sends it once to
the image route (by default the shared default route), removes the image bytes
from the conversation and keeps the textual description.

## MCP

Configured stdio servers start once and expose their tools directly. µAgent
requires the stateless `2026-07-28` protocol and rejects servers that do not
advertise it. `/mcp` shows the servers; `/mcp retry` and `/mcp on|off NAME`
retry one or switch it.

- Servers are required by default: a startup, discovery or tool-list failure
  stops bootstrap. Servers marked `"required": false` share a 2 s startup
  window, and a slow one finishes discovery at a later turn boundary without
  delaying model steps.
- A project's `.mcp.json` starts servers only once you trust it, at the
  startup prompt or with `--trust-project-config`; a change to its content
  asks again. `~/.mcp.json` is yours and needs no trust.
- The workspace is the default root. `UAGENT_MCP_ROOTS` (colon-separated) or a
  server's `roots` array in `.mcp.json` replaces it; per-server paths are
  relative to that file. Roots are cooperative protocol scope, not a sandbox:
  servers run with the user's permissions, outside the command sandbox.
- `roots/list` is answered through `input_required` continuations, at most
  eight per call. Tool-list changes arrive over one `subscriptions/listen`
  stream per server. A timed-out request sends `notifications/cancelled`.
- Server stderr goes to a rotating log under `~/.uagent/mcp/`, bounded at
  16 MiB. Errors without text point to that log.
- Not supported: HTTP transport, sampling, elicitation, task extensions,
  `$ref` and output-schema validation, and automatic restart of exited servers.

Browser automation is not MCP. Where the web host runs the shared Chrome, the
agent uses the built-in `browser` tool (see
[WEB.md](WEB.md#docker-browser-appliance)). Elsewhere the `browser-use` skill
drives `playwright-cli` through `run`: install it with
`npm install -g @playwright/cli@latest`, and use `attach --cdp=chrome` only for
a user-owned Chrome session.

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

- **A headless run failed.** It exits nonzero with a complete JSON envelope.
- **You need the full exchange.** `--debug` writes an opt-in, sensitive trace;
  see [ARCHITECTURE.md](ARCHITECTURE.md) for its contents. `/http` shows the
  captured requests of the running conversation.
- **A session will not load.** Corrupt sessions are reported and left
  untouched.
- **Processes outlived a session.** Runtime shutdown reaps managed processes.
  Closing a client only detaches; `SIGKILL` cannot guarantee cleanup.
- **A command was refused a write.** It reports the errno plus a
  `[sandbox: ...]` line. `/context` lists the writable roots; widen them with
  `UAGENT_SANDBOX_WRITE` (colon-separated, applies after a restart), or run
  the one command with `run(sandbox=false)`, which always asks a person. The
  same exemption applies to a command that needs credentials from a protected
  path (see [Tools](TOOLS.md#files-and-search)).
- **`sudo` or `ping` fails on Linux.** The sandbox sets `no_new_privs`, so
  setuid binaries fail inside it. Privileged work needs unconfined execution:
  `UAGENT_SANDBOX=0` or an approved `run(sandbox=false)`. `--yolo` does not
  lift the sandbox; it only stops the questions.
- **Every command is refused, or startup reports `degraded`.** The Linux
  kernel has no Landlock (5.13 or newer has it). With `UAGENT_SANDBOX` set
  anywhere, every command is refused; with it unset, commands run unconfined
  and startup says so. `UAGENT_SANDBOX_NET=0` on a kernel whose Landlock
  cannot restrict the network (before ABI 4) also refuses every command.
- **Old terminal logs pile up.** `~/.uagent/terminals/logs` is not pruned by
  age because live detached terminals write there. A crash between creating a
  log and writing its record leaves a `pending-*` file to delete by hand.
- **A Playwright attach fails.** It needs a live, user-approved Chrome
  debugging endpoint; `playwright-cli list` shows the session and `detach`
  leaves Chrome running.

Local state and removal are described in [PERSISTENCE.md](PERSISTENCE.md).
