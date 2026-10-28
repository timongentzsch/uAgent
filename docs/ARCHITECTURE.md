# Architecture

µAgent is a C++20 runtime with terminal and browser clients. One binary holds
the provider adapters, the tool executor and, optionally, the web assets.
There is no server-side language runtime and no dynamically loaded plugin
layer.

## Boundaries

| Path | Owns |
| --- | --- |
| `src/main.cc` | Entry point: internal modes such as `--session-worker` and `--browser-service`, otherwise the application |
| `src/cli/` | Flag parsing (`options.cc`), the slash-command registry (`cli.cc`), interactive reads |
| `src/app/` | Bootstrap (`bootstrap.cc`; tool assembly in `bootstrap_tools.cc`, approval decisions in `approvals.cc`), session transport and lifecycle |
| `src/app/session_*.cc` | The session runtime, its terminal client and the web host's facets; see [Session runtime and clients](#session-runtime-and-clients) |
| `src/app/commands_*.cc` | Slash-command handlers for model, session and control; `commands.cc` dispatches |
| `src/app/panel*.cc`, `thread_link.cc`, `launch.cc` | A folder's lead agent (board, plan, history, background agents, `panel -p`) and where a launched session runs |
| `src/agent/` | Turn execution, canonical conversation, context preparation, persistence, session links and search, supervision services (activities, child agent), mail delivery, the edit journal behind `/undo`, memory store and the transcript view every client draws |
| `src/api/` | Provider dialects, streaming, capabilities, usage and HTTP captures: transport in `client.cc`, request bodies in `wire_request.cc`, stream decoding in `wire_stream.cc` |
| `src/providers/` | Well-known endpoints, provider entries, model selection grammar and route policy |
| `src/tools/` | Tool surface and adapters over agent services; no session or supervision ownership |
| `src/tools/registry_*.cc` | Tool registration by family (files, exec, activity, memory); `registry.cc` only orders the families |
| `src/mcp/` | Bounded stdio JSON-RPC integration |
| `src/browser/` | The web host's browser service: owns Chrome and Xvnc behind a private socket |
| `src/core/` | Shared policy, events, limits, settings, mailboxes, sandbox, filesystem, signals and platform primitives |
| `src/media/` | Attachment encoding and display projections |
| `src/transport/` | Session frames and SSE framing |
| `src/ui/` | Terminal input and presentation; the Markdown renderer is `markdown.cc` with `markdown_math.cc` and `markdown_table.cc` |
| `src/web/` | Authenticated HTTP/SSE adapter (`master*.cc`), embedded assets, the browser viewer (`browser_viewer.cc`, `rfb_filter.cc`), the terminal's shells (`terminal.cc`) and optional push (`push.cc`; `no_push.cc` when `UAGENT_WEB_PUSH=OFF`) |
| `web/src/` | Browser event projection and presentation: `app/` shell and its hooks, `state/` host-data layer, `features/<name>/` self-contained UI, `shared/` cross-feature rendering and formatting |
| `tests/`, `benchmarks/` | Behavioral contracts and measurement |

The registry owns tool contracts, configuration descriptors own settings, route
capabilities own provider behavior, and the runtime owns conversation state.
Clients never interpret shell text to infer permission or mutation authority.

## Build layers

CMake mirrors the dependency DAG; each library links the one in the row
above it:

| Library | Contents |
| --- | --- |
| `uagent_core_base` | `src/core/`, `src/transport/` (leaf) |
| `uagent_api` | `src/api/` |
| `uagent_toolcore` | Tool vocabulary (`src/tools/tool.cc`), `src/media/` and `src/providers/`; no agent, tool or app dependency |
| `uagent_agent` | `src/agent/`: turn loop, persistence, supervision services, memory store, transcript view |
| `uagent_tools` | Tool surface and `src/mcp/`, consuming agent services; `src/browser/` when `UAGENT_BROWSER=ON` (the default with the web UI) |
| `uagent_app` | `src/app/`, `src/ui/`, `src/cli/` |
| `uagent_web` | `src/web/` and the embedded bundle; built when `UAGENT_WEB=ON` (default) |

Tests, benchmarks and `uagent_web` link `uagent_app`; the fuzzers link only
`uagent_core_base` and the source they fuzz.

Public headers live in `include/<module>/`, mirroring `src/<module>/`.
Declarations shared only inside one module stay in `src/<module>/*_internal.h`.
`tests/boundary_test.py` (CTest `layer_boundary`) rejects a new include that
points up this order; its `KNOWN` set lists the remaining exceptions.

The web host embeds the built `web/dist` (`npm ci && npm run build` in `web/`,
or CI's `web-dist` artifact). Text assets are gzipped at build time and served
with `Content-Encoding: gzip`, or inflated for a client that does not accept
it. `index.html`, `sw.js` and the other files outside `/assets/` carry an
ETag and answer `If-None-Match` with 304. API JSON over 1 KiB is gzipped for a
client that accepts it, by a post-routing handler that touches nothing else:
assets keep their own encoding, the event stream is not JSON and a range
request is answered plain.
[Contributing](../CONTRIBUTING.md) says how to build it.

## Session runtime and clients

![The terminal and the web host attach over a Unix socket to one session runtime process per conversation; a browser or phone reaches the web host over HTTP and SSE; the web host starts the agent's browser, Chrome and Xvnc with one signed-in profile, on first use and streams its live screen to paired devices; the runtime's browser tool drives Chrome over a private socket; a script runs one turn in its own headless process; the agent loop runs tools in a sandbox and calls Chat Completions, OpenAI Responses or Anthropic Messages endpoints over HTTPS](images/architecture.svg)

Each interactive conversation has one runtime process, and a private Unix
socket is its command and event boundary. The terminal and the web host attach
to that same process; the client that started a conversation has no extra
authority. The web host adapts HTTP commands and SSE to this boundary. It never
runs a second agent or reconciles competing conversation files.

| Piece | Code |
| --- | --- |
| Runtime process (`uagent --session-worker`) | `src/app/session_worker.cc`; socket server in `session_server.cc`; streamed text and usage batched by `TransientBatcher` (`transient_batcher.cc`) |
| Terminal client | `src/app/session_terminal*.cc`: input loop, runtime link, client-side commands, live region |
| Web host | `src/web/master*.cc`: routes, pairing, a table of command handlers, event streams, a conversation's files (read-only); over `SessionHost` (`include/app/session_host.h`) |
| `SessionHost` facets | `src/app/session_router.cc` (commands), `session_supervisor.cc` (runtimes), `session_schedules.cc`, `session_snapshot.cc`, `session_assets.cc`; event log in `replay_log.cc`, attachments in `asset_store.cc`, receipts in `outcome_store.cc` |
| Frame format and limits | `include/transport/session.h` |
| Headless `-p` run | `src/app/application_headless.cc` |
| Subagents | `src/tools/subagent.cc`, `src/agent/child_agent.cc` |
| Mailboxes | `src/core/mailbox.cc`, `src/agent/mail_delivery.cc` |

The runtime owns the agent, provider session identity, tools, approvals,
pending interactions and supervised children. `ApplicationChannel` connects its
input queue and typed interaction replies to `Application`. Commands are
serialized and one turn runs at a time; guidance and interruption are explicit
commands that may arrive during it. Read-only projections never execute work.

Frames carry the protocol version, session ID and runtime generation. Commands
add a request ID, and reusing one with different content is rejected. Bounded
receipts suppress repeated delivery within a runtime. Input completion has its
own correlation ID, so a client cannot mistake an unrelated idle or background
update for its command finishing. A human reply names the pending interaction:
the first accepted reply wins, and a second client cannot answer a stale
approval.

Transport bounds are constants in `include/transport/session.h` unless named:

| Resource | Bound |
| --- | --- |
| Frame / command | 1 MiB / 512 KiB |
| A client's output queue / the web host's replay log | 4 MiB each |
| Command receipts | 256 per runtime |
| Session header | 16 KiB (`include/agent/session_store.h`) |
| Catalogue | 4,096 sessions (`include/core/limits.h`) |
| History page | 64 blocks or 384 KiB (`src/agent/session_view.cc`) |

One poll thread owns the client sockets and their bounded fanout queues, so
publishing never waits for a slow terminal or browser. A joining client
receives a checkpoint and the ordered events after it; a gap requires a
refresh, never a command replay. The web host numbers events with the
`SessionHost` sequence and keeps them in a bounded replay log, each frame once
with its SSE envelope, to give every stream a host epoch and cursor. After
replay it sends an unsequenced `ready` watermark, which means transport
catch-up, not that the engine is idle. Stale connections cannot overwrite newer
state. A changed runtime generation invalidates pending commands; recovery
never repeats a tool or an uncertain submission by itself.

The web host's HTTP pool idles at four threads and grows to one per open event
stream, browser viewer or terminal socket (at most 16), plus twelve for
requests, so long-lived connections cannot starve the API.

Closing a client detaches it. Closing the runtime cancels work, saves state
and reaps session-owned children. An internal writer lease keeps two runtimes
from owning one file; clients never take it. Runtime discovery uses private
sockets, a bounded startup scan and native directory notifications. Schedule,
prompt and library invalidation uses the same multi-path watcher and wakes at
the next schedule deadline. Independent conversations may share a folder
folder; edits to shared files still need coordination.

A delegated child is a headless `-p` run of the same binary, supervised as a
background activity of its parent. It saves an ordinary session file in the
folder's history whose header carries a `delegation` object (parent, name,
directive, mode, model, route); the catalogue hides such files, and the parent
finds its children by that header. A child approves its own tool calls and
runs its commands under its parent's sandbox. A follow-up, or a message to a
finished child, starts a new bounded child process from the saved
conversation. The result reaches the parent as an activity completion, which
an idle parent takes up at once as a turn.

A folder's lead agent (internally a panel) is a session that reads and
delegates but never writes: it has no tool that edits, and its runtime forces
the `readonly` sandbox level, so its commands write nothing but temporary
files. The sessions it starts are its background agents (internally threads),
which run at the default level.

The lead agent asks its questions of subagents: it has the `subagent` tool,
and its children get only what their parent has, its sandbox included, so
they read and research and change nothing; starting one needs no approval. Its plan (goals, decisions, open questions and steps) is saved
beside its session and rebuilt into its context every turn, with the
subagents and background agents that carry each step's id.

Sessions message each other through durable mailboxes
([PERSISTENCE.md](PERSISTENCE.md#mail)): linked peers, a parent and its
children, children of one parent, and a folder's lead agent and its
background agents.

- Each runtime watches its mailbox (inotify, kqueue on macOS) and delivers at
  the next model step, including after a final answer, which reopens the turn.
- An idle runtime starts a turn on mail meant to wake it. A headless child
  answers mail that arrives after its last step before it exits.
- A background agent's finished turns and questions reach its lead agent
  within milliseconds, starting the lead agent's runtime when none runs. At
  the daily spend limit its mail waits.
- A sender is refused, visibly, past 64 pending messages, 20 a minute or 8
  forwards (`include/core/mailbox.h`).

Headless `-p` runs use the same application, agent and event policies in one
process, with their own bounded invocation and machine-output contract.

## Events and observability

`EventId` (`include/core/events.h`) and one compile-time policy table
(`src/core/events.cc`) define event names, durability and public redaction.
Producers publish semantic facts; fixed consumers handle terminal
presentation, JSONL, debug output and a bounded session journal. Runtime
adapters subscribe to the same events. Delivery is ordered, and a subscriber
can neither return a tool result nor grant authority.

The command side changes state; the event side reports the change.
`message.changed` updates visible history, `usage.updated` reports provider
accounting, `activities.changed` reports supervised work, and interaction
events carry correlated decisions. Final checkpoints reconcile complete state.
Terminal Markdown and browser DOM state are projections, never writers.

A model attempt receives a runtime response identity before its first delta.
That identity reaches the saved assistant display record, while provider tool
call IDs stay raw provider facts. Tool occurrences are scoped to the response
and have a separate retained-detail identity. Content revision and
completeness are independent: a bounded checkpoint preview at the same
revision cannot replace a fuller body a client already holds.

One reducer (`ApplySessionEvent`, `src/agent/session_view.cc`) folds events
into a view of rows: one per message and one per tool call, keyed so that a
live call, its result and the saved message land on the same row. The worker,
the web host and an attached terminal each fold with it. Browsers receive
`block` patches: for a checkpoint, the rows it changed and then its state
without the view. The whole view is sent only on a new view epoch or a change
that rows cannot express. The runtime also publishes its execution phase and
pending decision; connection health stays with the client.

How much of the view a client shows is one display setting,
`UAGENT_VERBOSITY` (`include/core/verbosity.h`). The model never sees it.

One activity projection (`include/core/turn_status.h`) derives the working label
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
  complete readable line of supplied reasoning, stripped of Markdown and
  capped at 160 bytes. Lines over 192 bytes are skipped; without a line the
  label is `Thinking`.

Labels are a display heuristic: they never establish that an action ran or
succeeded.

Readable reasoning summaries are requested per route
(`src/api/wire_request.cc`):

- OpenAI Responses routes that declare `reasoning_summary` (official OpenAI
  endpoints do) send `reasoning.summary: auto` unless effort is `none`.
- Anthropic routes send adaptive thinking with `display: summarized` when the
  Models API catalog advertises adaptive thinking and an effort other than
  `none` is set.
- Other routes opt in with the `reasoning_summary` and `adaptive_thinking`
  model features (a provider's `features` in `UAGENT_PROVIDERS`), which
  override catalog metadata.

A 400 response that rejects the summary field turns summaries off for that
route and retries only if the attempt made no progress. Signed or opaque
reasoning is replayed to the provider unchanged.

Provider-reported partial usage is combined with the confirmed session total
for live display; final usage replaces it through the normal accounting path.
Unknown cost stays unknown. Every model call is charged to a route keyed
`authority|purpose-or-provider|model|effort`: the conversation's own calls
under their provider, and titles, compaction, memory extraction, image
analysis, web search and permission review under their purpose, so `/cost`
names what each one spent. Turn statistics describe one turn; compaction has
its own display record and never appears as another turn. Spend that a
background child reports after its turn ended is added to that turn and
journaled as `turn.usage_amended`, since `turn.completed` is written once.
Durations and time to first token describe the attempt that answered; retry
backoff shows only in the end-to-end time.

`--json-stream` has its own redacted public contract. `--debug` is an opt-in,
sensitive trace. The bounded journal records lifecycle and resource metadata,
not prompts, answer deltas or complete tool output. None of these streams
substitutes for the saved conversation; see [Persistence](PERSISTENCE.md).

## Turn and process lifecycle

A turn prepares the effective prompt and advertised tools, calls the provider,
validates native tool calls, executes eligible independent tools and
incorporates their results. It repeats until an answer, an explicit stop, an
error or a configured budget ends the turn. Tool schemas and dispatch share
validation; prose that resembles a tool call is never executed.

The process supervisor owns spawn, PTY and pipe I/O, output retention,
foreground handoff and reaping. Waiters wake on process and signal
notifications. Queued guidance ends passive waits without cancelling the
activity behind them; explicit interruption uses the cancellation path. A
background result is added to the next model call; one that arrives while
the conversation is idle starts that call as a turn.

Turn time, model and tool calls, generated tokens and reported spend have
optional caps; individual operations, output, processes and context have
bounded defaults. [Operations](OPERATIONS.md) lists them and their failure
modes.

## Protocol and authority

Canonical conversation roles retain provenance. Explicit adapters encode Chat
Completions, Responses or Anthropic Messages. Provider-native reasoning and
hosted-tool data stay lossless on supported paths; unsupported semantics fail
explicitly. Model names grant no capabilities. Hosted web search requires a
route declaration; otherwise search uses a separately configured route.

Files, tools, fetched pages, memories and model-authored summaries are
evidence and cannot change approval policy. Native schemas, typed dispatch,
path checks, mandatory human approval and process ownership enforce that
boundary; prompt wording only complements them.

Ordinary approval may be automatic, but changes to µAgent's own configuration
and other mandatory-human operations always need a person. Reusing a shell
approval is scoped to the exact command, not the executable name. The shared
sandbox wrapper is applied when a command is spawned, and confinement that is
explicitly required fails closed when it cannot be enforced. Linux and macOS
share policy composition and have separate platform tests. Child environments
apply the shared credential and permission policy. See
[Security](../SECURITY.md).

## Configuration is described once

`include/core/config_registry.h` defines each setting once: its default,
bounds, sensitivity, activation timing and the scopes it may be saved in.
Runtime getters, CLI flags, `/config`, the web's Settings, diagnostics and
`uagent inspect` all read that source.

A value comes from the registry's default or one of the scopes
[Operations](OPERATIONS.md#settings) lists, the later winning: all
conversations and this folder (both in `~/.uagent/config/settings.json`),
the environment, the command line and this conversation (its session).
Only the model (`UAGENT_MODEL`, with its effort) and the approval mode
(`UAGENT_APPROVAL`) can be set for one conversation: `/model`, `/effort`,
`/permissions` and `/yolo` set them there, and `--model` or `--yolo` sets them
for the conversation it starts. One resolver answers what applies, where it
comes from and what each scope holds, so a settings list can say what a change
would override. Settings reload at a turn boundary; controls report the ones
that need a restart.

A browser's display preferences (appearance, motion, zoom, clock) are not host
settings; that browser alone keeps them.

Instruction files are layered, yours before the folder's, and only add to
the built-in base. Their editors and the agent tool share revision checking
and approval policy, and no prompt text changes tool permissions. [Instructions](INSTRUCTIONS.md)
describes authoring; [Management](MANAGEMENT.md) covers memory, skills and
scheduled tasks.

## Context and cache

Model context and visible history are separate views of the canonical
conversation. Compaction replaces bounded model context with an explicitly
non-authoritative summary while keeping display identity and archived history.
Pruning cannot turn a user message into a new apparent submission.

Request preparation reuses unchanged material and preserves stable prefixes.
Provider-specific cache controls stay in the wire adapters, and reported cache
reads and writes stay separate from ordinary input usage. Attachment
preparation uses declared model capabilities and retained originals, with
bounded extraction or an explicit fallback; a turn encodes each attached file
once for all its model calls. See [Caching](CACHING.md) and [Tools](TOOLS.md).

## Failure model and extension

A saved snapshot is the atomic replay authority. The journal may be absent or
truncated without making model history unrecoverable. Corrupt or incompatible
snapshots are reported and never overwritten. A crash does not establish
whether an external side effect of a tool happened, so a restart never assumes
it is safe to repeat it.

Add behavior at its owning boundary:

| To add | Change |
| --- | --- |
| A tool | A registry entry in `src/tools/registry_<family>.cc` |
| Wire semantics for a provider | The route adapter in `src/api/wire_request.cc` and `wire_stream.cc` |
| A setting | Its descriptor in `include/core/config_registry.h` |
| A slash command | The registry in `src/cli/cli.cc` and a handler in `src/app/commands_*.cc` (those the terminal client handles itself, such as `/quit`, in `session_terminal_commands.cc`) |
| Session behavior | A runtime command (`src/app/session_command.cc`) or an event (`include/core/events.h`) |
| Layout | A presenter in `src/ui/` or `web/src/` |

Tests assert the externally meaningful contract at that boundary. Keep
distinct safety, concurrency and platform cases, and avoid tests that only
count implementation strings. [Testing](TESTING.md) documents the checks.
