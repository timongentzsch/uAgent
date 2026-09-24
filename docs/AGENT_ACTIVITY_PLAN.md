# Agent activity labels and provider reasoning

Implementation and comparison, updated 2026-09-24. The five implementation
steps below are complete. One core activity projection now serves terminal,
web and process-child progress. Source review covers provider requests/stream
decoding, replay, dispatch, status, retries and rendering; it is not an
exhaustive audit of either harness.

## Implemented contract

- `tool.call` means preparing; `tool.started` is emitted immediately before
  execution. Occurrence IDs keep parallel calls independent. Decisions,
  provider retry waits and terminal events override the caption.
- Native read/write/search use existing tool summaries. Optional `description`
  on `run` and `scratch` supplies model-authored display intent; it is stripped
  before validation, approval and execution. Original arguments remain in
  provider replay. This adds 240 serialized schema bytes across both tools,
  no extra inference request and no inferred token/cost claim.
- Responses summary parts, Anthropic readable thinking and OpenRouter
  text/summary details share part reconciliation. Final-only text, corrections,
  duplicate finals and interleaving update the display; opaque/signed replay
  remains provider-native. Verbose terminal output is append-only; provider
  corrections are marked explicitly rather than replaying a whole snapshot.
- Official OpenAI Responses routes request `reasoning.summary: auto` unless
  effort is `none`. Compatible routes opt in through `features.reasoning_summary`
  in provider/model configuration or `UAGENT_MODEL_FEATURES`. A structured
  unsupported summary/whole-reasoning-field rejection disables summary requests
  for that route/session
  and retries only before any semantic progress.
- Anthropic Models API adaptive-thinking/effort capabilities are read from
  catalog metadata. Adaptive thinking requires an advertised capability plus
  an explicit positive effort; supported summaries request `display: summarized`.
  An explicit effort is passed through even without adaptive metadata rather
  than silently ignored. No model names, extra thinking budget or beta update
  channel are inferred to improve a caption. Explicit route features override
  catalog metadata; capability rejection survives a catalog refresh.
- Short complete readable lines/headings are a documented display heuristic.
  A missing/long/incomplete line falls back to Thinking. Transport reconnection
  and provider retry remain separate. Labels never establish execution success.
- Process children reuse their existing bounded progress log; persistent child
  inspection reads the same status from its worker snapshot. The popup says
  latest reported activity: this does not create a new child subscription.
- Shared composer scaling now paints text at the selected density while keeping
  the editable layout font at least 16px on touch devices. Noninteractive dialog
  headings/containers have no focus outline; keyboard controls retain theirs.

Regression coverage includes core projection and CLI parity, signed replay,
provider capability fallback, invalid display metadata, retry state, browser
zoom/focus and the mixed streaming workload in `web/tests/performance.spec.js`.
See [measurements](MEASUREMENTS.md#activity-display-verification) for benchmark
scope and limits. Physical iPhone focus behavior and real-provider latency
remain outside the desktop/mock checks.

## What oh-my-pi actually does

Cloned `can1357/oh-my-pi` and inspected commit
`73a11421fe34fbab8ad058ab9c1a2e4f447852ea`.

- Its default-enabled `tools.intentTracing` adds a short `i` string to tool
  schemas. Its prompt asks for a 2–6-word action description. The harness
  removes this display metadata before tool argument validation/execution.
- Its UI reads this field from streamed tool arguments and updates the working
  label. Tools can instead supply a deterministic `intent(args)` formatter;
  those tools need no injected field. This is the primary mechanism behind
  labels such as “Editing module,” rather than a universal provider summary.
- Reasoning content is a separate stream. Its Responses adapter explicitly
  requests summaries and tracks items/parts, deltas and final snapshots.
- It coalesces streaming updates and flushes them before terminal events. It
  also explicitly recovers the working indicator after retries/compaction.

Pinned sources: [intent schema and extraction](https://github.com/can1357/oh-my-pi/blob/73a11421fe34fbab8ad058ab9c1a2e4f447852ea/packages/agent/src/agent-loop.ts#L886),
[UI activity projection](https://github.com/can1357/oh-my-pi/blob/73a11421fe34fbab8ad058ab9c1a2e4f447852ea/packages/coding-agent/src/modes/controllers/event-controller.ts#L1413),
[default setting](https://github.com/can1357/oh-my-pi/blob/73a11421fe34fbab8ad058ab9c1a2e4f447852ea/packages/coding-agent/src/config/settings-schema.ts#L4664),
[provider summary handling](https://github.com/can1357/oh-my-pi/blob/73a11421fe34fbab8ad058ab9c1a2e4f447852ea/packages/ai/src/providers/openai-shared.ts#L3239).

## What is portable

| Source | Available data | Contract |
| --- | --- | --- |
| OpenAI Responses | Reasoning summary parts when requested/supported | Request `reasoning.summary: auto` only on supported routes; consume item/part identities and final snapshots. This is a readable summary, not raw hidden reasoning. |
| Anthropic Messages | Readable `thinking` / `thinking_delta`, plus signatures and possibly redacted blocks | Display only supplied readable text. Summary visibility and thinking configuration vary by model/API capability; preserve signatures/opaque blocks unchanged for continuation. |
| OpenRouter Chat Completions | `reasoning`, `reasoning_content`, structured `reasoning_details` | Prefer explicit summary details for summary provenance; distinguish text and encrypted details. Avoid duplicate display when convenience and structured fields repeat the same content. |
| Any tool-calling model | Tool name, arguments and observed execution state | Always sufficient for a useful action label. Model-authored purpose is optional; it never establishes success or grants permission. |

No provider promises a short, continuously updated action title for every model.
No readable text is a valid outcome. Summary output can arrive in bursts and
should not be used as a liveness clock or evidence that an action ran.
Provider contracts: [OpenAI reasoning summaries](https://developers.openai.com/api/docs/guides/reasoning#reasoning-summaries),
[Anthropic thinking](https://platform.claude.com/docs/en/build-with-claude/thinking),
[OpenRouter reasoning](https://openrouter.ai/docs/guides/best-practices/reasoning-tokens).

## Comparison with µAgent before this implementation

| Area | Current evidence | Decision |
| --- | --- | --- |
| Provider boundaries | `wire_stream.cc` and `openai_stream.cc` already normalize all three formats. Raw replay stays separate from display facts. | Extend these adapters; retain the event spine and replay separation. |
| Summary requests | Responses request construction sends effort but never opts into `reasoning.summary`. Parsing summary deltas alone does not request them. | Add capability-aware summary visibility, independently of effort. Do not increase reasoning effort to obtain nicer UI. |
| Thinking capabilities | The Anthropic request envelope maps selected efforts to adaptive thinking without an explicit model capability in `ProviderCapabilities`. | A wire format alone does not establish adaptive-thinking support. Audit catalog gating and represent supported thinking/summary modes before expanding requests to more models. |
| Stream fidelity | `WireStreamDelta.reasoning` collapses summary/text provenance. Responses summary part boundaries and `summary_text.done` are ignored for display. | Add minimal part identity/kind and final reconciliation; avoid concatenating adjacent parts or losing final-only summaries. Preserve the existing plain-text display projection for old clients/history. |
| Tool labels | `ToolSummary` already formats calls and `tool.call.activity.label` carries it. The worker status discards it and emits only “Running <tool>.” | Reuse the existing formatter/label, adding a short formatter only where the existing label is a long command. Do not reparse tool arguments in each UI. |
| Existing intent | `run`/`scratch` have an `intent` enum for explore/change/execute grouping. | It is a category, not prose. Keep its semantics; do not repurpose it as a status sentence or permission signal. |
| Execution timing | `tool.call` is emitted during preparation, before permission resolution and actual execution. Tool arguments stream earlier but do not currently produce a UI preview event. | Distinguish Preparing, Awaiting permission and Running. Attach running labels to actual execution starts. Never imply that partial JSON has been executed. |
| Parallel work | A single worker activity string can be overwritten by the latest call/result. | Track active call occurrence IDs; one completed call must not clear another running call. Show one label plus a count for multiple calls. |
| Performance | Native text/usage coalescing and browser frame batching already exist. | Reuse them. Emit a status update only when its text/source changes, never one extra event per token. |
| Retry policy | Shared bounded exponential backoff, jitter and Retry-After support exist. Automatic chat replay excludes visible answer/tool/usage progress. | Keep conservative replay; do not copy a more permissive harness retry without proving side-effect safety. Add structured provider-retry state so the web UI can show the actual wait. |
| Poll loops | Repeated calls nudge at 3/6, stop at 12; quiet activity polls nudge at 2/4, stop at 12. Blocking waits are exempt. | Already avoids the old three-poll stop. These remain explicit policies, not proof that work is stuck. Evaluate parking on a live activity instead of failing after ignored nudges as a separate behavior change. |
| Subagents | Main/child history and statistics are shared, but process children expose checkpoints rather than a complete live state subscription. | Deliver the same status contract wherever live events exist; label checkpoints honestly. Do not invent a second child polling/state engine. |

Local anchors: `src/api/wire_request.cc`, `src/api/wire_stream.cc`,
`src/api/openai_stream.cc`, `include/api/stream.h`, `src/tools/tool.cc`,
`src/agent/tool_loop.cc`, `src/app/session_worker.cc`, `include/api/retry.h`,
`src/agent/turn.cc`, `include/core/limits.h`.

## Implementation steps

### 1. Make observed actions useful first

Extend the existing activity projection with a small optional detail object:
`label`, `source`, `response_id`, `occurrence_id`. Keep lifecycle `phase`
authoritative and separate. One core projection serves the CLI, main web
conversation, child conversation and activity list. Reuse `ToolSummary` and the
existing occurrence identities. Maintain only active calls, clearing them at
results/cancellation/turn end. This yields useful labels for every model with
no extra inference call and no new schema tokens.

Examples: “Reading src/router.ts,” “Running tests,” “Editing browser.tsx,”
“Waiting for activity 7.” Where a purpose cannot be established, show the actual
operation/command instead of guessing “Refactoring.” Approval, retry, user
interaction and terminal states override descriptive labels.

### 2. Preserve and surface supplied summaries

Add capability-aware summary requests in the existing request adapters, plus
typed provenance and part reconciliation in their decoders. A small incremental
presentation helper selects a complete short heading/line from the readable
summary. Treat that selection explicitly as a display heuristic, not a model
API promise. Render plain text, keep the full supplied content in the existing
disclosure, and fall back to “Thinking” when no suitable text exists.

The UI must distinguish “Thinking · <summary excerpt>” from an operation that is
actually running. Never decode encrypted content, synthesize hidden reasoning,
or feed shortened display text back into provider continuation history.

### 3. Add semantic purpose only where it earns its cost

For arbitrary shell/script execution, optionally accept a short `description`
alongside the existing arguments, using one shared schema definition. This
provides oh-my-pi-style labels such as “Refactoring session history” where the
command itself is unreadable. Missing/invalid display metadata falls back to
the observed operation; it must not fail or authorize a call. Keep the existing
`intent` category unchanged.

Do not inject a required `i` into every native/MCP schema: closed/union schemas,
field collisions, argument replay and provider strict mode make that larger
than a presentation feature. Native read/write/search already have adequate
arguments. Add no status-only tool and no secondary summarizer model. Measure
the optional description's actual schema bytes and provider-reported token
usage; any tokenizer estimate must stay labeled as an estimate.

### 4. Keep transport, model waits and execution separate

The new connection component owns Connecting/Reconnecting/Disconnected.
Provider backoff uses a structured event containing attempt and retry deadline,
not a guessed elapsed timer. Thinking, generating tool arguments, approvals,
tool execution and background waits each retain their existing lifecycle owner.
Use the same narrow status component across main/child views. Keep one line
with ellipsis and accessible full text, bounded by the container at every zoom.

Centralize label length, incomplete-line buffering and update cadence in the
existing limits files, documenting them as presentation policies. Process only
new fragments with a bounded buffer; do not repeatedly scan the whole growing
reasoning transcript. Reuse existing coalescing rather than adding another
animation/polling loop.

### 5. Verify the contract before rollout

- Provider fixtures: summary/text/encrypted variants, absent reasoning,
  split UTF-8/headings, multiple/interleaved parts, duplicate convenience fields,
  final-only summaries, empty final snapshots and unsupported request fields.
- Exact replay tests: signed/opaque reasoning unchanged through tool handoffs;
  UI labels never enter history sent to a model.
- Lifecycle tests: parallel tools, approval denial, cancellation, retries,
  generation replacement, late events and child follow-ups. Old activity cannot
  overwrite a new turn; stop/error cannot leave a spinner or stale action.
- Browser/CLI parity: identical source label, retained/full-history consistency,
  mobile clipping, reduced motion and announcements only on meaningful changes.
- Performance: replay at least 1,000 mock tokens/s through the real delivery
  path, including reasoning plus tool events and concurrent children. Measure
  CPU/frame gaps, input latency, event rate and bounded label memory. Compare
  against the same workload without labels; do not equate a desktop mock with
  real provider or iPhone performance.

These changes retain the agent loop and existing transport batching. Parking
a repeatedly polled live activity remains a separate behavior change.
