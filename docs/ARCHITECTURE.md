# Architecture

µAgent is a C++20 runtime with terminal and browser clients. The installed binary
contains the provider adapters, tool executor and optional web assets. There is
no application server language runtime or dynamically loaded plugin layer.

## Boundaries

| Domain | Responsibility |
| --- | --- |
| `src/app/` | Bootstrap, commands, configuration, session transport and lifecycle |
| `src/agent/` | Turn execution, canonical conversation, context preparation and persistence |
| `src/api/` | Provider dialects, streaming, capabilities, usage and HTTP captures |
| `src/tools/` | Tool implementations and supervised processes |
| `src/core/` | Shared policy, events, limits, filesystem, signals and platform primitives |
| `src/ui/` | Terminal input and presentation |
| `src/web/` | Authenticated HTTP/SSE adapter, assets and optional push |
| `web/src/` | Browser event projection and presentation |
| `src/mcp/` | Bounded stdio JSON-RPC integration |
| `tests/`, `benchmarks/` | Behavioral contracts and measurement |

The registry owns tool contracts, configuration descriptors own settings, route
capabilities own provider behavior, and the runtime owns conversation state.
Clients do not interpret shell text to infer permission or mutation authority.

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
adapter adds its host epoch and SSE cursor so stale connections cannot overwrite
newer state. A changed runtime generation invalidates pending commands; recovery
never automatically repeats tools or an uncertain submission.

Closing a client detaches it. Closing the runtime cancels work, saves state and
reaps session-owned children. An internal writer lease prevents two runtimes
from owning one file; clients do not acquire that lease. Runtime discovery uses
private sockets and bounded catalogue scans, not live JSON sidecars, inbox files
or terminal-specific mirroring. Independent conversations may share a project
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
