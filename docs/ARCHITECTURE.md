# Architecture

µAgent is a C++20 runtime with terminal and browser clients. The installed binary
contains the provider adapters, tool executor and optional web assets. There is
no application server language runtime or dynamically loaded plugin layer.

## Boundaries

| Path | Owns |
| --- | --- |
| `src/main.cc` | Entry point: internal modes such as `--session-worker`, otherwise the application |
| `src/cli/` | Flag parsing (`options.cc`), the slash-command registry (`cli.cc`), interactive reads, `--emit-reference` (`reference.cc`) |
| `src/app/` | Bootstrap, session transport and lifecycle |
| `src/app/session_*.cc` | The session runtime, its terminal client and the host's facets; see [Session runtime and clients](#session-runtime-and-clients) |
| `src/app/commands_*.cc` | Slash-command handlers for model, session and control; `commands.cc` dispatches |
| `src/app/coordinator.cc`, `chat.cc`, `thread_link.cc`, `launch.cc` | A folder's coordinator, its threads, its chat with members and where a launched session runs |
| `src/agent/` | Turn execution, canonical conversation, context preparation, persistence, supervision services (process, jobs, child agent), mail delivery, the edit journal behind `/undo`, memory store and observation records |
| `src/api/` | Provider dialects, streaming, capabilities, usage and HTTP captures: transport in `client.cc`, request bodies in `wire_request.cc`, stream decoding in `wire_stream.cc` |
| `src/providers/` | Route catalog, model selection grammar and route policy |
| `src/tools/` | Tool surface and adapters over agent services; no session or supervision ownership |
| `src/tools/registry_*.cc` | Tool registration by family (files, exec, activity, memory); `registry.cc` only orders the families |
| `src/mcp/` | Bounded stdio JSON-RPC integration |
| `src/browser/` | Browser service for the Docker appliance: owns Chrome and Xvnc behind a private socket |
| `src/core/` | Shared policy, events, limits, settings, mailboxes, sandbox, filesystem, signals and platform primitives |
| `src/media/` | Attachment encoding and display projections |
| `src/transport/` | Session frames and SSE framing |
| `src/ui/` | Terminal input and presentation |
| `src/web/` | Authenticated HTTP/SSE adapter (`master.cc`), assets and optional push |
| `web/src/` | Browser event projection and presentation: `app/` shell, `state/` host-data layer, `features/<name>/` self-contained UI, `shared/` cross-feature rendering and formatting |
| `tests/`, `benchmarks/` | Behavioral contracts and measurement |

The registry owns tool contracts, configuration descriptors own settings, route
capabilities own provider behavior, and the runtime owns conversation state.
Clients do not interpret shell text to infer permission or mutation authority.

## Build layers

CMake mirrors the dependency DAG; each library links the one in the row
above it:

| Library | Contents |
| --- | --- |
| `uagent_core_base` | `src/core/`, `src/transport/` (leaf) |
| `uagent_api` | `src/api/` |
| `uagent_toolcore` | Tool vocabulary (`src/tools/tool.cc`), `src/media/` and `src/providers/`; no agent, tool or app dependency |
| `uagent_agent` | `src/agent/`: turn loop, persistence, supervision services, memory store, observation records |
| `uagent_tools` | Tool surface and `src/mcp/`, consuming agent services; `src/browser/` when `UAGENT_BROWSER=ON` (default with the web UI) |
| `uagent_app` | `src/app/`, `src/ui/`, `src/cli/` |
| `uagent_web` | `src/web/` and the embedded bundle; built when `UAGENT_WEB=ON` (default) |

`uagent_core` is an INTERFACE umbrella over `uagent_app`; tests, benchmarks,
fuzzers and `uagent_web` link it.

Public headers live under `include/` (top-level facades plus
`include/<module>/`). Only module-private shared declarations stay in
`src/<module>/*_internal.h`. `tests/boundary_test.py` (CTest
`layer_boundary`) rejects any new include that points up this order; its
`KNOWN` set lists the remaining exceptions.

The web host embeds the built `web/dist` (`npm ci && npm run build` in
`web/`, or CI's web-dist artifact). A first configure without it builds
CLI-only and says how to add the UI.

## Session runtime and clients

Each interactive conversation has one runtime process. A private Unix socket is
its local command/event boundary. CLI and web attach to that same process; the
client that starts a conversation has no extra authority or capabilities.
The web server adapts HTTP commands and SSE to this boundary. It neither runs a
second agent nor reconciles competing conversation files.

| Piece | Code |
| --- | --- |
| Runtime process (`uagent --session-worker`) | `src/app/session_worker.cc`; its socket server in `session_server.cc` |
| Terminal client | `src/app/session_terminal.cc` |
| Web host | `src/web/master.cc` over `SessionHost` (`include/app/session_host.h`) |
| `SessionHost` facets | `src/app/session_router.cc` (commands), `session_supervisor.cc` (runtimes), `session_schedules.cc`, `session_snapshot.cc`, `session_assets.cc`; event log in `replay_log.cc`, attachments in `asset_store.cc`, receipts in `outcome_store.cc` |
| Frame format and limits | `include/transport/session.h` |
| Headless `-p` run | `src/app/application_headless.cc` |
| Subagents | `src/tools/subagent.cc`, `src/agent/child_agent.cc` |
| Mailboxes | `src/core/mailbox.cc`, `src/agent/mail_delivery.cc` |

The runtime owns the agent, provider session identity, tools, approvals, pending
interactions and supervised children. `ApplicationChannel` connects its input
queue and typed interaction replies to `Application`. Commands are serialized;
a single turn runs at a time. Guidance and interruption are explicit commands
that can arrive while that turn runs. Read-only projections never execute work.

Frames carry protocol version, session ID and runtime generation. Commands add a
request ID; reuse with different content is rejected. Bounded receipts suppress
repeated delivery within a runtime. Input completion has its own correlation ID:
a client cannot mistake an unrelated idle/background update for its command
finishing. Human replies identify the pending interaction, so the first accepted
reply wins and a second client cannot answer a stale approval.

The transport's bounds are constants, in `include/transport/session.h` unless
named:

| Resource | Bound |
| --- | --- |
| Frame / command | 1 MiB / 512 KiB |
| A client's output queue / the web host's replay log | 4 MiB each |
| Command receipts | 256 per runtime |
| Session header | 16 KiB (`include/agent/session_store.h`) |
| Catalogue | 4,096 sessions (`src/agent/session_store.cc`) |
| History page | 64 blocks (`src/agent/session_view.cc`) |

One poll thread owns client sockets and bounded fanout queues. Publishing does
not wait for a slow terminal or browser. A joining client receives a checkpoint
and its ordered event suffix. Gaps require refresh, not command replay. The web
adapter uses the native `SessionHost` sequence and bounded replay log to add its
host epoch and SSE cursor. After replay it sends an unsequenced `ready` watermark;
this means transport catch-up, not that the engine is idle. Stale connections
cannot overwrite newer state. A changed runtime generation invalidates pending
commands; recovery never automatically repeats tools or an uncertain submission.

Closing a client detaches it. Closing the runtime cancels work, saves state and
reaps session-owned children. An internal writer lease prevents two runtimes
from owning one file; clients do not acquire that lease. Runtime discovery uses
private sockets, a bounded startup scan and native directory notifications, not
live JSON sidecars or terminal-specific mirroring. Schedule,
prompt and library invalidation uses the native multi-path watcher and wakes at
the next actual schedule deadline. Independent conversations may share a project
folder; edits to shared project files still require coordination.

A delegated child is a headless `-p` run of the same binary, supervised as a
background activity of its parent. It saves an ordinary session file in the
workspace's history whose header carries a `delegation` object (parent, name,
directive, mode, model, route); the catalogue hides such files, and the parent
finds its children by that header. A child approves its own tool calls and
runs its commands under its parent's sandbox. A follow-up starts a new bounded
child process from the saved conversation, and a message to a finished child
starts one on that message. A child's result reaches its parent through the
activity completion; an idle parent takes it up at once as a turn.

A folder's coordinator is a session that reads and delegates but never writes
or runs commands. The sessions it starts are its threads.

The coordinator's conversation is also a chat (`src/app/chat.cc`). A member
is a thread the coordinator added under a name and a persona instead of a
brief: it reads and searches, changes nothing, and its prompt comes from its
session header, which it cannot rewrite. The coordinator is one of the
participants, with no part of its own beyond its tools.

- Everyone hears every message. What the coordinator's runtime settles, and
  no model does, is who is woken: the participants a message names with `@`,
  or all of them when it names nobody. The rest read it without a turn.
- A woken participant answers, or answers `PASS` (nothing to add) or `WAIT`
  (someone who is typing is likely to cover it). Neither is shown to anyone.
  Each wake-up says who is typing; whoever waited is woken by the next
  message, or as soon as nobody is typing.
- A member's answer appears in the coordinator's conversation under its
  name. It starts a turn there only when it wakes the coordinator like any
  other participant.
- Two limits keep an exchange from running on by itself, both counted from
  one message of the user's: `UAGENT_COORDINATOR_CHAT_TURNS` turns per
  participant, and two messages each. The coordinator cannot message a
  member past the chat. Members spend from the same daily limit as threads.
- A file the user attaches reaches the members as its path in the message;
  they read what is shared in the coordinator's chat without review, like
  the folder itself.
- The round (each participant's turns and messages, who is typing, who
  waits) is saved beside the
  coordinator's session, so a runtime that starts again takes it up. A
  member's answer is forwarded under an id derived from it: forwarded again
  after a crash, it is the mail its readers already have.

Sessions message each other through durable mailboxes
([PERSISTENCE.md](PERSISTENCE.md#mail)): linked peers, a parent and its
children, children of one parent, and a folder's coordinator and its threads.

- Each runtime watches its mailbox (inotify, kqueue on macOS) and delivers at
  the next model step, including after a final answer, which reopens the turn.
- An idle runtime starts a turn on mail meant to wake it. A headless child
  answers mail that arrives after its last step before it exits.
- A thread's finished turns and questions reach its coordinator this way
  within milliseconds, starting the coordinator's runtime when none runs. At
  the daily spend limit the coordinator's mail waits.
- Senders are refused, visibly, past 64 pending messages, 20 a minute or 8
  forwards (`include/core/mailbox.h`).

Headless `-p` runs use the same application, agent and event policies in one
process. Their bounded invocation and machine-output contract are separate from
an attached interactive client.

## Events and observability

`EventId` (`include/core/events.h`) and one compile-time policy table
(`src/core/events.cc`) define names, durability and public redaction.
Producers publish semantic facts; fixed consumers handle terminal
presentation, JSONL, debug output and a bounded session journal. Runtime
adapters subscribe to the same application events. Subscriber delivery is
ordered and cannot return a tool result or grant authority.

The command side changes state; the event side reports the change.
`message.changed` updates visible history, `usage.updated` reports current
provider accounting, `activities.changed` reports supervised work, and interaction
events carry correlated decisions. Final checkpoints reconcile complete state.
Terminal Markdown/ANSI and browser DOM state are projections, never alternate
writers.

A model attempt receives a runtime response identity before its first delta.
That identity reaches the saved assistant display record, while provider tool
call IDs remain raw provider facts. Tool occurrences are scoped to the response
and have a separate retained-detail identity. Content revision and completeness
are independent: a bounded checkpoint preview at the same revision cannot
replace a fuller body already held by a client.

One reducer (`ApplySessionEvent`, `src/agent/session_view.cc`) folds events
into a view of rows: one per message and one per tool call, keyed so a live
call, its result and the saved message land on the same row. The worker, the
host and an attached terminal each fold with it; browsers receive the host's
result as `block` patches. The runtime also publishes its canonical execution
phase and pending decision; transport connection health remains client-owned.

How much of that view a client shows is one display setting,
`UAGENT_VERBOSITY` (`include/core/verbosity.h`). The model never sees it.

One activity projection (`include/core/activity.h`) derives the working label
for the terminal, the browser and process-child progress.

- `tool.call` marks a call being prepared, before approval; `tool.started` is
  emitted immediately before execution. Calls are tracked by occurrence ID, so
  one finished parallel call cannot clear another.
- Pending decisions, provider retry waits and terminal states override
  descriptive labels.
- `run` and `scratch` accept an optional `intent` (`explore`, `research`,
  `edit`, `verify`, `run`, `setup`). It only groups the call for display and
  grants nothing.
- While the model reasons, the label is `Thinking · <line>`: the latest
  complete readable line of supplied reasoning, stripped of Markdown
  decoration and capped at 160 bytes. Lines over 192 bytes are skipped;
  without a line the label is `Thinking`.

Labels are a display heuristic: they never establish that an action ran or
succeeded.

Readable reasoning summaries are requested per route
(`src/api/wire_request.cc`):

- Official OpenAI Responses routes send `reasoning.summary: auto` unless
  effort is `none`.
- Anthropic routes send adaptive thinking with `display: summarized` when the
  Models API catalog advertises adaptive thinking and an effort other than
  `none` is set.
- Other routes opt in with the `reasoning_summary` and `adaptive_thinking`
  model features (a provider's `features` in `UAGENT_PROVIDERS`), which
  override catalog metadata.

A 400 response that rejects the summary field turns summary requests off for
that route and retries only if the attempt produced no progress. Signed or
opaque reasoning is replayed to the provider unchanged.

Provider-reported partial usage is combined with the confirmed session total for
live display. Final usage replaces that provisional view through the normal
accounting path. Unknown cost stays unknown. Turn statistics describe one turn;
compaction has a separate display record and cannot masquerade as another turn.

`--json-stream` has its own redacted public contract. `--debug` is an opt-in
sensitive trace. The bounded journal records lifecycle and resource metadata,
not prompts, answer deltas or complete tool output. None of these operational
streams substitutes for the saved conversation. See [Persistence](PERSISTENCE.md).

## Turn and process lifecycle

A turn prepares the effective prompt and advertised tools, makes a provider call,
validates native tool calls, executes eligible independent tools, and incorporates
results. It repeats until an answer, explicit stop, error or configured budget
ends the turn. Tool schemas and dispatch share validation; prose that resembles a
tool call is never executed.

The process supervisor owns spawn, PTY/pipe I/O, output retention, foreground
handoff and reaping. Waiters wake on process and signal notifications. Queued
guidance yields passive waits without cancelling their underlying activity.
Explicit interruption uses the cancellation path. Background completion is
observational until the next natural model call; it does not create a model turn
merely to announce a finished command.

Aggregate turn time, model/tool calls, generated tokens and reported spend have
optional caps. Individual operations, output, processes and context have bounded
defaults. The definitions and practical failure modes are in
[Operations](OPERATIONS.md), rather than duplicated here.

## Protocol and authority

Canonical conversation roles retain provenance. Explicit adapters encode Chat
Completions, Responses or Anthropic Messages. Provider-native reasoning and
hosted-tool data remain lossless on supported paths; unsupported semantics fail
explicitly. Model names do not grant capabilities. Hosted web search requires a
route declaration, otherwise search uses the configured separate route.

Files, tools, fetched pages, memories and model-authored summaries are evidence;
they cannot change approval policy. Native schemas, typed dispatch, path checks,
mandatory human approval and process ownership enforce the boundary. Prompt
wording complements those checks.

Ordinary approval may be automatic. Changes to µAgent's own configuration and
other mandatory-human operations still require a person. Shell approval reuse is
scoped to the exact command, not an executable name. The shared sandbox wrapper
is applied at command spawn: explicitly required confinement fails closed if it
cannot be enforced. Linux and macOS implementations share policy composition and
have distinct platform tests. Child environments apply the shared credential and
permission policy. See [Security](../SECURITY.md).

## Configuration is described once

`include/core/config_registry.h` defines each setting once: its default, bounds,
sensitivity, activation timing and the scopes it may be saved at. Runtime
getters, CLI flags, `/config`, the web's Settings, diagnostics and the
generated skill references all consume that source.

A value comes from one of these scopes, the later winning:

| Scope | Affects | Kept in |
| --- | --- | --- |
| Default | everything | the registry |
| All conversations | every conversation, browser and terminal of this user | `all` in `~/.uagent/config/settings.json` |
| This project | conversations in one folder | `projects[<folder>]` in the same document |
| Environment | one process and those it starts | `UAGENT_*` variables |
| Command line | one run | flags |
| This conversation | one conversation | its session |

Only the model (`UAGENT_MODEL`, with its variant and effort) and the approval
mode (`UAGENT_APPROVAL`) may be chosen for one conversation; `/model`,
`/effort`, `/variant`, `/permissions` and `/yolo` choose them there, and a flag
that names one (`--model`, `--yolo`) chooses it for the conversation it starts.
One resolver answers what applies, where it comes from and what each scope
holds, so a settings list can say what a change would override. Reload happens
at a turn boundary, while controls report settings that require a restart.

A browser's display preferences (appearance, motion, zoom, clock) are not
settings of the host: they are kept in that browser only.

System-prompt documents support inherit, overlay and replacement at conversation,
project and global scope. Their editors and agent tool share revision checking
and approval policy. Replacing prompt text does not replace tool permissions.
[System prompts](SYSTEM_PROMPTS.md) describes authoring; [Management](MANAGEMENT.md)
describes memory, skills and scheduled tasks.

## Context and cache

Model context and visible history are separate views of the canonical
conversation. Compaction replaces bounded model context with an explicitly
non-authoritative summary while retaining display identity and archived history.
Pruning cannot rewrite a user message into a new apparent submission.

Request preparation reuses unchanged material and preserves stable prefixes.
Provider-specific cache controls stay in the wire adapters; reported cache reads
and writes remain separate from ordinary input usage. Attachment preparation uses
declared model capabilities and retained originals, with bounded extraction or
an explicit fallback. See [Caching](CACHING.md) and [Tools](TOOLS.md).

## Failure model and extension

A saved snapshot is atomic replay authority. The journal may be absent or
truncated without making model history unrecoverable. Corrupt or incompatible
snapshots are reported without overwriting them. A crash does not establish
whether an external tool side effect happened; restart never assumes that it is
safe to repeat it.

Add behavior at its owning boundary:

| To add | Change |
| --- | --- |
| A tool | A registry entry in `src/tools/registry_<family>.cc` |
| Wire semantics for a provider | The route adapter in `src/api/wire_request.cc` and `wire_stream.cc` |
| A setting | Its descriptor in `include/core/config_registry.h` |
| A slash command | The registry in `src/cli/cli.cc` and a handler in `src/app/commands_*.cc` |
| Session behavior | A runtime command (`src/app/session_command.cc`) or an event (`include/core/events.h`) |
| Layout | A presenter in `src/ui/` or `web/src/` |

Tests should assert the externally meaningful contract at that boundary. Keep
distinct safety, concurrency and platform cases; avoid tests that only count
implementation strings. [Testing](TESTING.md) documents the checks.
