# Headless runs

`uagent -p PROMPT` runs a conversation in the calling process and exits; a
script reads stdout as text, one JSON envelope (`--json`, schema
`uagent.headless.v1`) or JSON lines (`--json-stream`, `uagent.event.v2`).
`--control` is a model-free request/reply command. Flags: [Command line](cli.md).

## Invocation

`HeadlessMain` (`src/main.cc`) bootstraps a runtime in-process (no session
worker, no socket), runs the prompt as a turn (`Application::RunHeadless`)
and saves the conversation, so `-c -p` continues the folder's latest one; a
`-c` with none starts a new one and says so on stderr. A headless run keeps
no event journal. Afterwards it takes mail from agents it started as
further turns, and waits while they owe it a report. stdout goes to
`/dev/null` while it works; the result is written after teardown.

| Flag | With `-p` |
| --- | --- |
| `--json`, `--json-stream`; `-c`; `--attach PATH` | output form (exclusive, each needs `-p`); continue; a file for the first message. `--resume` is refused |
| `--model` and the other route flags | override that route for this run |
| `--yolo` | approve ordinary calls without asking |
| `--budget USD`, `--token-budget TOKENS` | `UAGENT_SESSION_BUDGET`, `UAGENT_SESSION_TOKEN_BUDGET` for this run |
| `--trust-project-config` | trust the folder's `.mcp.json`; an untrusted one fails the run with status 2 |

Stdin carries no prompt and never answers an approval. When stdin is a
terminal, a call that asks a person (`approval.mode`,
[Settings](settings.md)) reads one line there: `y`, `s` or `a` allow (once,
for the conversation, always in this folder), `x` refuses always in this
folder, `n` or end of file denies, other text denies and is queued as
steering. When it is not, every call that would ask a person is denied,
including what Auto's reviewer cannot decide and what always needs a
person; `--yolo` never covers the latter. A folder rule
(`/permissions allow TOOL MATCH`, `permissions.json`) is how an unattended
run is allowed to act.

## Plain output and exit status

| Status | Meaning | stdout | stderr |
| --- | --- | --- | --- |
| 0 | an answer | the last assistant text | |
| 1 | model or tool error, budget stop without an answer, "agent produced no answer", bootstrap failure | | the error |
| 2 | bad flags, unreadable attachment, untrusted `.mcp.json`, `-c` that cannot resume | | the error |

A turn stopped by `max_steps`, `max_tool_calls`, `repeated_calls` or
`turn_deadline` gets one more round to answer; if it does, the status is 0
and only `stop.reason` shows the limit.

## `--json` envelope

One object on one line from `HeadlessResult` (`include/app/bootstrap.h`),
also for statuses 1 and 2, except an unreadable `--attach` file, which
writes only to stderr. Keys are sorted.

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | string | `uagent.headless.v1` |
| `answer` | string | the final answer; `""` on failure |
| `error` | string or null | the error; null on success |
| `stop` | object or null | why the last turn ended; null when no turn ran |
| `trace` | array | the latest tool trace: `{type: "function", id, name, arguments, result}`, `result` the output text or null |
| `usage` | object | what this run spent: `input`, `output`, `cache_read`, `cache_write`, `reasoning`, `web_searches` (integers), `cost` (USD), `cost_reported` (bool) |
| `routes` | object | the `usage` shape per route `<host>\|<provider>\|<model>\|<effort>` this run used |
| `exit_code` | integer | the process exit status |

`stop` holds `reason` (`completed`, `cancelled`, `error`, `step_limit`,
`budget_exceeded`, `turn_deadline`, `session_token_budget`, `turn_tokens`,
`session_budget`, `turn_cost`, `max_tool_calls`, `repeated_calls`,
`max_steps`), `detail`, `steps`, `tool_calls`, `generated_tokens`, `cost`,
`session_generated_tokens`, `session_cost`, and `limits`: the turn's
`max_steps`, `max_tool_calls`, `max_turn_seconds`, `max_turn_tokens`,
`session_token_budget`, `max_turn_cost`, `session_budget`.

## `--json-stream` events

One object per line (`JsonEventStream::Emit`, `src/core/debug.cc`):

```json
{"data":{"attachments":0,"context_tokens":812,"input":"inspect","local_time":"2026-11-16 10:00:00 CET (UTC+0100)","messages":2,"origin":"user","turn":1},"schema":"uagent.event.v2","seq":1,"time":"2026-11-16T09:00:00Z","type":"turn.started"}
```

`seq` starts at 1 and rises by one per line; lines are written under one
lock, so file order is `seq` order. `time` is UTC to the second. The types
are those with a `public_type` in `kPolicies` (`src/core/events.cc`):

| `type` | `data` |
| --- | --- |
| `turn.started` | `turn`, `origin`, `input` (the prompt text), `attachments`, `messages`, `context_tokens`, `local_time` |
| `tool.call` | `turn`, `step`, `call_id`, `response_id`, `occurrence_id`, `detail_id`, `name`, `arguments_digest`, `issue_code`, `issue_field`, `argument_keys`, `argument_types`, `operation` when the arguments carry a short `operation` or `action`; never argument values |
| `tool.result` | `turn`, `step`, `call_id`, `name`, `status`, `completion_status`, `error_code`, `issue_code`, `issue_field`, `duration_ms`, `result` (the output text), `result_chars`, … |
| `usage.updated` | `usage`, with `context_tokens` while a response streams or `statistics` after it |
| `notice` | `text` |
| `usage` | the turn summary: `turn`, `outcome`, `steps`, `tool_calls`, `model_calls`, `duration_ms`, `usage`, `messages`, `context_tokens`, … |
| `answer` | the `--json` envelope; the last line when the status is 0 |
| `error` | the `--json` envelope; the last line otherwise |

Approvals, interactions, deltas, session lifecycle events and the events
of agents the run starts are not on this stream.

## `--control JSON|-`

`ControlMain` reads one JSON object from the argument, or from stdin with
`-` (at most 1 MiB), and prints one. It takes no other flag; `cwd` names
the folder (default the current one).

| `kind` | `action` |
| --- | --- |
| `memory` | `list` (default), `get`, `set`, `forget`, `rename`, `copy` |
| `skills` | `list` (default), `get`, `enable`, `disable`, `set`, `forget` |
| `import` | `list` (default), `run`; `only` `memory` or `skill`, `key` a name or `all` |
| `schedule` | `list`, `preview`, `get`, `check`, `save`, `run`, `pause`, `resume`, `forget`, `stop` |
| `instructions` | `show` (default), `set` with `scope` `user`, `project` or `prompt`, `text`, optional `base` |
| `permission_rules` | `list` (default), `delete`, `clear` |
| `openrouter` | `start` returns `url`; a call with `code` and `state` returns `model` |
| `library`, `models`, `provider`, `reference`, `calendar` | read-only |

## Errors

Changing an existing memory, skill or schedule needs the `revision` from a
`get`. A failed control request prints `{"error": "<text>"}` and exits 1;
one over 1 MiB exits 2. With `--json-stream`, a `-c` that cannot resume
writes an `error` line whose `data` is `{"error": "cannot resume: …"}`, not
the envelope.

## Compatibility

`schema` is the version; match `uagent.headless.v1` and `uagent.event.v2`
exactly. Within a version fields are only added. Not contract: fields
marked …, unlisted event types, key order, `trace` arguments
([Tool schemas](tool-schemas.md)), notice wording, `--control` replies.

## Source and tests

| Path | What |
| --- | --- |
| `src/main.cc`, `src/cli/options.cc` | `HeadlessMain`, `Fail`, flag rules |
| `src/app/application_headless.cc`, `include/app/bootstrap.h` | the run, `HeadlessResult` |
| `src/core/events.cc`, `src/core/debug.cc` | public types, `PublicProjection`, the writer |
| `src/app/approvals.cc`, `src/app/control.cc` | approvals with nobody to ask; `--control` |
| `tests/integration_runtime/sessions.py` | envelope, stream, resume refusal |
| `tests/integration_runtime/budgets.py`, `tests/integration_runtime/recovery.py`, `tests/unit/events_test.cc` | `stop`, answers at a limit; public type names |
