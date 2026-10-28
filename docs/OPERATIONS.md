# Operations

Settings, limits, budgets, recovery and failure triage. µAgent is a local,
single-user CLI for macOS and Linux; [Security](../SECURITY.md) describes
what it confines.

## Settings

Saved settings live in one private document,
`~/.uagent/config/settings.json`:

```json
{
  "$schema": "./settings.schema.json",
  "format": 3,
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

`all` applies to every conversation, `projects` to the conversations in one
folder. Each key is a setting as `/config`
names it, with a value of its type. A value of `$NAME` stands for the entry
of that name under `variables`. Every setting also has an environment name
(`UAGENT_MAX_TURN_COST` for `limits.maxTurnCost`), which `/config` accepts
too. A value set in the environment may refer to a variable or another
environment name the same way, inside a JSON value such as
`UAGENT_PROVIDERS` too; there a `$` that names nothing defined stays as
written.

Change settings with `/config`, the web's Settings, the agent's `uagent` tool
(you approve the exact change) or an editor; `settings.schema.json` beside
the file lets an editor complete and check it. An unknown name, or a value
its setting does not take, is reported and skipped; the rest applies, and
the file keeps what you wrote.

```text
/config                                  what is set, and where from
/config user limits.maxTurnCost=2        save for all conversations
/config project tools.timeout=120        save for this folder
/config conversation approval.mode=auto
/config user unset limits.maxTurnCost
/config project reset                    clear a scope (keeps secrets)
/config UAGENT_SANDBOX                   one setting, scope by scope
/context                                 route, capabilities, advertised schemas
```

Five scopes can hold a value, and the later wins: All conversations, This
folder, Environment (`UAGENT_*` variables), Command line, and This
conversation (model and approval mode only). `/config` names the scope each
value comes from. Secrets never appear in its output.

A setting marked `next-user-turn` in the reference applies from your next
message. A `restart-required` one (route and credentials, sandbox, tool set,
memory on or off, MCP) is reported as such; `/restart` applies it to the
running conversation. A turn in flight never changes.

To move settings to another host, copy `~/.uagent/config/settings.json`. It
holds credentials in clear text; treat it as a secret.

## Limits

Rows without a setting are fixed in the binary. `0` switches off a turn or
session limit and the lead agent's daily cost limit.

| Concern | Default | Setting |
| --- | --- | --- |
| stream silence (before the first event or between events) / one request attempt | 300 s / 600 s | `UAGENT_STREAM_TIMEOUT`, `UAGENT_REQUEST_TIMEOUT` |
| request / response size | 64 / 32 MiB | |
| turn wall clock, rounds, tool calls | unlimited | `UAGENT_MAX_TURN_SECONDS`, `UAGENT_MAX_STEPS`, `UAGENT_MAX_TOOL_CALLS` |
| turn generated tokens / reported cost | unlimited | `UAGENT_MAX_TURN_TOKENS`, `UAGENT_MAX_TURN_COST` |
| identical consecutive tool calls that end a turn | 12 (the model is warned at 3 and 6) | `UAGENT_REPEATED_CALL_STOP` (at least 7) |
| session generated tokens / reported cost | unlimited | `UAGENT_SESSION_TOKEN_BUDGET` (`--token-budget`), `UAGENT_SESSION_BUDGET` (`--budget`) |
| one tool call | 30 s; `run`, `scratch`, `activity` and `subagent` are bounded by the turn | `UAGENT_TOOL_TIMEOUT` |
| tool result / parallel batch | 8,000 / 16,000 characters | `UAGENT_TOOL_RESULT_CHARS` (batch is twice the result cap) |
| `read_path` | 1,000 lines, at most 10,000 lines or 32 KiB | `UAGENT_READ_FILE_LINES` (default only), `UAGENT_READ_FILE_KB` |
| `edit_file` / diff | files up to 10 MiB | `UAGENT_EDIT_FILE_MB` |
| `list_dir` / `grep` | 1,000 entries / 200 matches | `UAGENT_LIST_ENTRIES`, `UAGENT_GREP_RESULTS` |
| `web_search` | 4 calls per turn, 3 queries and 5 results each, 60 s per attempt | `UAGENT_WEB_SEARCH_CALLS`, `UAGENT_WEB_SEARCH_MAX_USES`, `UAGENT_WEB_SEARCH_RESULTS`, `UAGENT_WEB_SEARCH_TIMEOUT` |
| trace pruning | newest 64 KiB protected; results from 32 KiB pruned | |
| automatic compaction | 85% of projected context | `UAGENT_AUTO_COMPACT_PCT` |
| compacted-trace archive | 16 MiB | |
| background activities / foreground tool workers | 8 (all but 2 open to subagents) / 4, up to the subagent share for a batch of subagents | `UAGENT_MAX_ACTIVITIES`, `UAGENT_TOOL_CONCURRENCY` |
| `run` initial wait | 10 s; `yield_ms` 250–30,000, or 0 to wait for exit | |
| activity output buffer / command log | 1 MiB, equal head and tail / 64 MiB | `UAGENT_ACTIVITY_LOG_MB` (log) |
| retained finished activities | 16 | |
| subagent completion in context | 6,000 characters each, 12 KiB per batch | |
| delegation depth / rounds / tool calls per child | 2 / 100 / 240 | `UAGENT_SUBAGENT_DEPTH`, `UAGENT_SUBAGENT_MAX_STEPS`, `UAGENT_SUBAGENT_MAX_TOOL_CALLS` |
| wall clock per child | the turn | `UAGENT_SUBAGENT_TIMEOUT` |
| subagent launches per turn | 32 | `UAGENT_SUBAGENT_CALLS_PER_TURN` |
| background agents at once / reported cost per day per lead agent | 5 / $20 | `UAGENT_PANEL_MAX_THREADS`, `UAGENT_PANEL_DAILY_SPEND_USD` |
| a background agent's report to its lead agent | 6,000 characters; `history` reads the rest | |
| steps in a lead agent's plan | 24 | |
| idle session runtime | stops after 15 minutes; the next message starts it | `UAGENT_IDLE_MINUTES` |
| undo journal | 2 MiB per file, 64 MiB per session | |
| memory size / count per scope / always-on slice | 2 KiB / 32 / 2 KiB | `UAGENT_MEMORY_BYTES`, `UAGENT_MEMORY_MAX`, `UAGENT_MEMORY_ALWAYS_BYTES` |
| memory extraction | 32 KiB of one session after 6 idle hours | `UAGENT_MEMORY_IDLE_HOURS` (idle time) |
| memory event audit | 256 KiB | |
| skills offered / skill body / discovery depth | 64 / 512 KiB / 6 directories | `UAGENT_SKILLS_MAX` (offered) |
| folder instructions | 32 KiB | `UAGENT_INSTRUCTIONS_KB` |
| CLI attachment / web upload / files per prompt | 10 MiB / 8 MiB / 8 | `UAGENT_ATTACHMENT_MB` (CLI attachment), `UAGENT_ATTACHMENT_COUNT` |
| image side before scaling down | 2048 px | `UAGENT_IMAGE_MAX_PX` |
| input history / paste | 200 entries of 16 KiB / 64 KiB | |
| MCP call / optional-server startup | 60 s / 2 s shared | `UAGENT_MCP_TIMEOUT` (call only) |
| MCP servers / tools per server | 32 / 256 | `UAGENT_MCP_MAX_SERVERS`, `UAGENT_MCP_MAX_TOOLS` |
| macOS sandbox profile | 64 KiB; larger profiles refuse the command | |
| session file | 64 MiB | |
| conversation / global source assets | 64 MiB / 512 MiB | |
| web Files pane: text shown / search matches | first 1 MiB / 200 | |
| paired devices / device lifetime / pairing code | 16 / 30 days / 5 minutes | |
| event streams per web host | 4 at once | |

Raise a bound only with a representative measurement.

## Budgets and accounting

Cap what a session may spend with a budget flag or a saved limit:

```sh
uagent --budget 5 --token-budget 200000 -p "fix the failing test"
```

```text
/config user UAGENT_MAX_TURN_COST=2
```

- Budgets are checked between model calls, so one in-flight response can
  overrun what remains.
- Cost limits need provider-reported `usage.cost`. µAgent never infers cost;
  when a provider omits it, output marks cost unavailable and warns that the
  limit cannot be enforced.
- The session token budget survives resume.
- A delegated child gets only its parent's remaining session allowance; the
  children of one batch run together and share it equally. While a session
  budget is set, no child starts while a background one runs. Child usage is
  charged to the parent by delta, so follow-ups do not reset accounting.
- Context size is estimated as serialized request bytes divided by 4, plus
  what the last turn request's reported prompt held beyond its own estimate
  (mostly the provider's fixed tool-use prompt, which bytes cannot show). It
  is labelled `est. ctx`, drives compaction and request guards, never
  billing, and is measured again after compaction or a model switch.
- Compaction reserves a quarter of the context window for the response, or
  `UAGENT_MAX_TOKENS` when that is smaller.

## Recovery

- **Transient provider errors.** Overload, rate-limit, resource-exhaustion,
  timeout and unavailable errors, and HTTP 408, 409, 429 and 5xx, get three
  attempts. The waits are about 2 s, then 4 s, each shortened by up to a
  quarter of jitter; a valid `Retry-After` is the minimum delay.
  `UAGENT_REQUEST_TIMEOUT` caps one attempt; chat retries stop at the turn
  deadline and side requests at their tool deadline. A stream is replayed
  only before visible text, tool calls, annotations or usage arrive. A
  provider may already have processed a failed request, so a retry can repeat
  upstream cost.
- **Context overflow.** The first context-length rejection in a turn lowers
  the learned context window, compacts once and retries the step. If
  compaction fails or the provider rejects again, the turn ends.
- **Repeated calls.** The third identical valid tool call adds an advisory,
  the sixth a direct instruction to change course, and the twelfth stops the
  turn. A call rejected by schema or policy stops after three equivalent
  attempts. Repeated no-change `activity` polls of one activity get advice
  after two, a direct instruction after four, and stop after twelve.
- **Out-of-range pacing arguments.** `run` `yield_ms` and
  `max_output_chars`, `grep` `context`, `activity` `wait_ms` and
  `max_output_chars`, and the `subagent` limits are clamped to their bound,
  and the result starts with a note such as
  `[clamped context to 10 of 40 requested]`. Any other out-of-range argument
  is rejected with the value and the limit.

## Activities and delegation

Commands, delegated tasks and detached terminals share activity IDs. `/ps`
lists them, `/ps ID output` shows one and `/ps ID stop` ends it; the status
bar counts them as `act:N`. [Tools](TOOLS.md#activities) describes how they
start, what outlives the conversation and how results are delivered.

## Images

When the main route cannot read an image, µAgent sends it once to the image
route ([Models and providers](GUIDE.md#models-and-providers) says which),
removes the image bytes from the conversation and keeps the textual
description.

## MCP

Configured stdio servers start once and expose their tools directly. µAgent
requires the stateless `2026-07-28` protocol and rejects servers that do not
advertise it. `/mcp` shows the servers; `/mcp retry` and `/mcp on|off NAME`
retry one or switch it.

- Servers are required by default: a startup, discovery or tool-list failure
  stops bootstrap. Servers marked `"required": false` share a 2 s startup
  window; a slow one finishes discovery at a later turn boundary without
  delaying model steps.
- A folder's `.mcp.json` starts servers only once you trust it, at the
  startup prompt or with `--trust-project-config`; a change to its content
  asks again. `~/.mcp.json` is yours and needs no trust.
- The folder is the default root. A server's `roots` array in `.mcp.json`
  replaces it; per-server paths are
  relative to that file. Roots are protocol scope, not a sandbox. Servers run
  under the command sandbox with the folder writable, unless their entry
  sets `"sandbox": false` ([Sandboxing](../SECURITY.md#sandboxing)).
- Server stderr goes to a rotating log under `~/.uagent/mcp/`, bounded at
  16 MiB. Errors without text point to that log.
- Not supported: HTTP transport, sampling, elicitation, task extensions,
  `$ref` and output-schema validation, and automatic restart of exited
  servers.

## Failure triage

- **A headless run failed.** It exits nonzero and prints the error to stderr;
  with `--json` it prints a complete JSON envelope instead.
- **You need the full exchange.** `--debug` writes an opt-in, sensitive
  trace ([Persistence](PERSISTENCE.md#sessions)). `/http` shows the captured
  requests of the running conversation.
- **One provider behind a router misbehaves.** OpenRouter can serve each call
  from a different upstream. A message's statistics in the web say which one
  ("served by …"); the conversation's statistics and `/cost` count calls and
  rejected tool calls per upstream; `/http` and the `--debug` trace's
  `model_response` add the generation id to quote to the router.
- **A session will not load.** Corrupt sessions are reported and left
  untouched.
- **Processes outlived a session.** Runtime shutdown reaps managed processes.
  Closing a client only detaches; `SIGKILL` cannot guarantee cleanup.
- **A command was refused a write.** It reports the errno and a
  `[sandbox: ...]` line naming the writable roots, as `/context` does. Add a
  root with `UAGENT_SANDBOX_WRITE` (colon-separated, applies after a
  restart), raise the level (`UAGENT_SANDBOX=relaxed` opens your home
  folder), or run the one command with `run(sandbox=false)`, which always
  asks a person. A session started in your home directory has no writable
  folder: see [Sandboxing](../SECURITY.md#sandboxing).
- **`sudo` or `ping` fails on Linux.** The sandbox sets `no_new_privs`, so
  setuid binaries fail inside it. Privileged work needs `UAGENT_SANDBOX=off`
  or an approved `run(sandbox=false)`. `--yolo` stops the questions, not the
  sandbox.
- **Every command is refused, or startup reports `degraded`.** The Linux
  kernel has no Landlock (it needs 5.13 or newer). With `UAGENT_SANDBOX` set
  explicitly, every command is refused; with it unset, commands run
  unconfined and startup says so. Denying the network
  (`UAGENT_SANDBOX_NET=0`) on a kernel whose Landlock cannot restrict it
  (before ABI 4) also refuses every command.
- **A command cannot read a credentials file.** The sandbox hides µAgent's
  settings, other tools' credential stores and similar paths, and the file
  tools refuse them too. Add your own with `UAGENT_SANDBOX_HIDE`, lift a
  default with `UAGENT_SANDBOX_REVEAL`, or run the one command with
  `run(sandbox=false)` ([Sandboxing](../SECURITY.md#sandboxing)).
- **Old terminal logs pile up.** `~/.uagent/terminals/logs` is not pruned by
  age because live detached terminals write there. A crash between creating a
  log and writing its record leaves a `pending-*` file to delete by hand.
- **A Playwright attach fails.** It needs a live, user-approved Chrome
  debugging endpoint; `playwright-cli list` shows the session and `detach`
  leaves Chrome running.

[Persistence](PERSISTENCE.md) covers local state and removal;
[Memory, skills and scheduled tasks](MANAGEMENT.md) covers the library.
