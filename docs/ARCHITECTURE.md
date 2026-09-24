# Architecture

µAgent is a C++20 runtime with terminal and browser clients. The installed binary
contains the provider adapters, tool executor and optional web assets. There is
no application server language runtime or dynamically loaded plugin layer.

## Boundaries

| Domain | Responsibility |
| --- | --- |
| `src/app/` | Bootstrap, session transport and lifecycle |
| `src/app/session_*.cc` | Session-host facets: routing, schedules, supervision, snapshots (facade: `session_host.h`; event log: `replay_log`, attachments: `asset_store`, receipts: `outcome_store`) |
| `src/tools/registry_*.cc` | Tool registration by family (files, exec, activity, memory); `registry.cc` only orders the families |
| `src/app/commands_*.cc` | Slash-command dispatcher (commands.cc) plus model, session and control handlers |
| `src/cli/` | Terminal entry surface: flag parsing, slash-command registry, interactive reads, `--emit-reference` |
| `src/api/` | Provider dialects, streaming, capabilities, usage and HTTP captures: transport in client.cc, request-body construction in wire_request.cc |
| `src/agent/` | Turn execution, canonical conversation, context preparation, persistence, supervision services (process/jobs/child_agent), memory store and observation records |
| `src/providers/` | Route catalog, model selection grammar and route policy |
| `src/media/` | Attachment encoding and display projections |
| `src/transport/` | SSE framing for event delivery |
| `src/tools/` | Tool surface and adapters over agent services; no session or supervision ownership |
| `src/core/` | Shared policy, events, limits, filesystem, signals and platform primitives |
| `src/ui/` | Terminal input and presentation |
| `src/web/` | Authenticated HTTP/SSE adapter, assets and optional push |
| `web/src/` | Browser event projection and presentation (`app/` shell, `state/` host-data layer, `features/<name>/` self-contained UI, `shared/` cross-feature rendering and formatting) |
| `src/mcp/` | Bounded stdio JSON-RPC integration |
| `src/browser/` | Browser service for the Docker appliance: owns Chrome and Xvnc behind a private socket |
| `tests/`, `benchmarks/` | Behavioral contracts and measurement |

The registry owns tool contracts, configuration descriptors own settings, route
capabilities own provider behavior, and the runtime owns conversation state.
Clients do not interpret shell text to infer permission or mutation authority.

## Build layers

CMake mirrors the dependency DAG; each library links the one above it:

| Library | Contents |
| --- | --- |
| `uagent_core_base` | `src/core/`, `src/transport/`, `src/media/` (leaf) |
| `uagent_api` | `src/api/` |
| `uagent_toolcore` | Tool vocabulary (`src/tools/tool.cc`) and `src/providers/`; no agent, tool or app dependency |
| `uagent_agent` | `src/agent/`: turn loop, persistence, supervision services, memory store, observation records |
| `uagent_tools` | Tool surface, `src/mcp/` and `src/browser/`, consuming agent services |
| `uagent_app` | `src/app/`, `src/ui/`, `src/cli/` |
| `uagent_web` | `src/web/` and the embedded bundle; built when `UAGENT_WEB=ON` (default) |

`uagent_core` is an INTERFACE umbrella over `uagent_app`; tests, benchmarks,
fuzzers and `uagent_web` link it. Public headers live under `include/`
(top-level facades plus `include/<module>/`); only module-private shared
declarations stay in `src/<module>/*_internal.h`. The web bundle embeds
`web/dist` or fails with instructions (`npm run build` in `web/`, or
`-DUAGENT_WEB=OFF`).

## Session runtime and clients

Each interactive conversation has one runtime process. A private Unix socket is
its local command/event boundary. CLI and web attach to that same process; the
client that starts a conversation has no extra authority or capabilities.
The web server adapts HTTP commands and SSE to this boundary. It neither runs a
second agent nor reconciles competing conversation files.

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
live JSON sidecars, inbox files or terminal-specific mirroring. Schedule,
prompt and library invalidation uses the native multi-path watcher and wakes at
the next actual schedule deadline. Independent conversations may share a project
folder; edits to shared project files still require coordination.

A normal collaborator follow-up starts a bounded child process from its saved
conversation. A persistent collaborator instead keeps one same-binary session
worker and its process supervisor under the parent runtime. Sequential
handoffs use the worker's ordinary command/checkpoint protocol, so its shell
and background activities survive between handoffs. Parent shutdown stops that
worker and its complete process group. A lifetime pipe also closes the worker
after an abrupt parent exit; it is never inherited by tool executables.

Headless `-p` runs use the same application, agent and event policies in one
process. Their bounded invocation and machine-output contract are separate from
an attached interactive client. Collaborator processes retain their explicit
supervisor-owned lifetime.

## Events and observability

`EventId` and one compile-time policy table define names, durability and public
redaction. Producers publish semantic facts; fixed consumers handle terminal
presentation, JSONL, debug output and a bounded session journal. Runtime adapters
subscribe to the same application events. Subscriber delivery is ordered and
cannot return a tool result or grant authority.

The command side changes state; the event side reports the change.
`message.changed` updates visible history, `usage.updated` reports current
provider accounting, `activities.changed` reports supervised work,
`collaborator.changed` reports retained-worker lifecycle, and interaction
events carry correlated decisions. Final checkpoints reconcile complete state.
Terminal Markdown/ANSI and browser DOM state are projections, never alternate
writers.

A model attempt receives a runtime response identity before its first delta.
That identity reaches the saved assistant display record, while provider tool
call IDs remain raw provider facts. Tool occurrences are scoped to the response
and have a separate retained-detail identity. Content revision and completeness
are independent: a bounded checkpoint preview at the same revision cannot
replace a fuller body already held by a client. The runtime also publishes its
canonical execution phase and pending decision; transport connection health
remains client-owned.

One activity projection (`include/core/activity.h`) derives the working label
for the terminal, the browser and process-child progress. `tool.call` marks a
call being prepared, before approval; `tool.started` is emitted immediately
before execution. Calls are tracked by occurrence ID, so one finished parallel
call cannot clear another. Pending decisions, provider retry waits and terminal
states override descriptive labels. `run` and `scratch` accept an optional
model-authored `description` for the label; it is stripped before validation,
approval and execution, while the original arguments stay in provider replay.
While the model reasons, the label is `Thinking · <line>`, where the line is
the latest complete readable line of supplied reasoning, stripped of Markdown
decoration and capped at 160 bytes; lines over 192 bytes are skipped, and
without one the label is `Thinking`. This is a display heuristic: labels never
establish that an action ran or succeeded.

Readable reasoning summaries are requested per route. Official OpenAI
Responses routes send `reasoning.summary: auto` unless effort is `none`.
Anthropic routes send adaptive thinking with `display: summarized` when the
Models API catalog advertises adaptive thinking and an effort other than
`none` is set. Other routes opt in with the `reasoning_summary` and
`adaptive_thinking` model features (provider `features` or
`UAGENT_MODEL_FEATURES`), which override catalog metadata. A 400 response
that rejects the summary field turns summary requests off for that route and
retries only if the attempt produced no progress. Signed or opaque reasoning is replayed to the provider unchanged.

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

`config_registry.h` defines defaults, bounds, sensitivity and activation timing.
Runtime getters, CLI flags, diagnostics and generated skill references consume
that source. Configuration precedence is explicit; trusted project configuration
cannot silently override a command-line selection. Reload happens at a turn
boundary, while controls report settings that require a restart.

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

Add behavior at its owning boundary: a registry entry for a tool, a route adapter
for wire semantics, a runtime command/event for session behavior, or a presenter
for layout. Tests should assert the externally meaningful contract at that
boundary. Keep distinct safety, concurrency and platform cases; avoid tests that
only count implementation strings. [Testing](TESTING.md) documents the checks.
