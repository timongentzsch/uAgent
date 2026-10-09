# Operations

How to inspect and change settings, cap what a session may spend, read the
limits, and triage failures. µAgent is a local, single-user POSIX CLI for
macOS and Linux; it confines the commands it runs (see
[SECURITY.md](../SECURITY.md)) but is not a container or a multi-tenant
service.

## Settings

Settings are saved in one private document,
`~/.uagent/config/settings.json`:

```json
{
  "$schema": "./settings.schema.json",
  "format": 2,
  "all": {
    "model": "openrouter/vendor/model",
    "limits.maxTurnCost": 2,
    "sandbox.network": false,
    "openrouter.apiKey": "$OPENROUTER_KEY",
    "variables": { "OPENROUTER_KEY": "sk-or-…" }
  },
  "projects": {
    "/home/me/project": { "tools.timeout": 120 }
  }
}
```

`all` applies to every conversation and `projects` to the conversations in
one folder. A setting is named as the
[configuration reference](../skills/uagent-config/references/configuration.md)
lists it and holds a value of its type. A value may be `$NAME`, which stands
for the entry of that name under `variables`.

Change it with `/config`, the web's Settings, the agent's `uagent` tool
(which asks you to approve the exact change), or an editor:
`settings.schema.json` beside it lets one complete and check the file. An
edit applies from your next message. A name that is no setting, or a value
its setting does not take, is reported and left out; everything else in the
file applies, and what you wrote stays in it.

```text
/config                                  what is set, and where from
/config user limits.maxTurnCost=2        save for all conversations
/config project tools.timeout=120        save for this project folder
/config conversation approval.mode=auto
/config user unset limits.maxTurnCost
/config project reset                    clear a scope (keeps secrets)
/debug-config UAGENT_SANDBOX             one setting, scope by scope
/context                                 route, capabilities, advertised schemas
```

A setting has a second name, an environment variable (`UAGENT_MAX_TURN_COST`
for `limits.maxTurnCost`), which `/config` takes as well.

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

Rows without a setting are fixed in the binary. `0` switches off a turn or
session limit and the coordinator's daily cost limit.

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
| chat turns per participant, and messages each, for one message of the user's | 3, 2 | `UAGENT_COORDINATOR_CHAT_TURNS` |
| members of one coordinator's chat | 16 | |
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
| session file | 64 MiB | |
| web upload / files per prompt | 8 MiB / 8 | |
| conversation / global source assets | 64 MiB / 512 MiB | |
| paired devices / device lifetime / pairing code | 16 / 30 days / 5 minutes | |
| event streams per web host | 4 at once | |

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
- A delegated child receives only its parent's remaining session
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

Commands, delegated tasks and detached terminals share activity IDs. `/ps`
lists them, `/ps ID output` shows one and `/ps ID stop` ends it; the status
bar counts them as `bg:N`. [Tools](TOOLS.md#activities) describes how they
start, what survives the conversation and how a result is delivered.

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
- Server stderr goes to a rotating log under `~/.uagent/mcp/`, bounded at
  16 MiB. Errors without text point to that log.
- Not supported: HTTP transport, sampling, elicitation, task extensions,
  `$ref` and output-schema validation, and automatic restart of exited servers.

## Failure triage

- **A headless run failed.** It exits nonzero and prints the error to stderr;
  with `--json` it prints a complete JSON envelope instead.
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
  kernel has no Landlock (5.13 or newer has it). With `UAGENT_SANDBOX`
  explicitly switched on, every command is refused; with it unset, commands
  run unconfined and startup says so. `UAGENT_SANDBOX_NET=0` on a kernel
  whose Landlock cannot restrict the network (before ABI 4) also refuses
  every command.
- **Old terminal logs pile up.** `~/.uagent/terminals/logs` is not pruned by
  age because live detached terminals write there. A crash between creating a
  log and writing its record leaves a `pending-*` file to delete by hand.
- **A Playwright attach fails.** It needs a live, user-approved Chrome
  debugging endpoint; `playwright-cli list` shows the session and `detach`
  leaves Chrome running.

Local state and removal are described in [PERSISTENCE.md](PERSISTENCE.md);
memory, skills and scheduled tasks in [MANAGEMENT.md](MANAGEMENT.md).
