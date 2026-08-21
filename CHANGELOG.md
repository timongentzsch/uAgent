# Changelog

## Unreleased

### Fixed

- A provider error injected mid-stream discarded a whole turn's work. Such a
  frame is now retried when it arrived before any answer text, tool call,
  annotation or usage — the attempt left nothing to replay — and the bare
  `{"type":"api_error","message":…}` shape some providers send is read as the
  error it is instead of dropped as an unrecognized frame.
- Parallel tool calls streamed without an `index` piled into one slot, so a
  single call arrived carrying two tools' arguments and was rejected. Fragments
  without an index are keyed by call id instead.
- A numeric pacing hint outside its schema bounds — `yield_ms`, `context`,
  `wait_ms`, a page size — rejected the call, spending a model round to say
  "use the maximum". Those arguments are clamped into range; identifiers are
  not, and a fractional value is still a type error.
- An `activity` receipt named only its target, so a resize read as a bare poll
  and a write as a size with no operation. Each argument set now names its
  verb.
- Streaming markdown measured display width by counting UTF-8 lead bytes, so a
  line containing CJK, emoji, or combining marks reported the wrong row count
  and redrawn tables erased the wrong number of rows.
- The compiler-family guard in CMake was assigned an unevaluated expression and
  always tested true, so warnings-as-errors applied to every compiler and the
  "sanitizers require Clang or GCC" refusal could never fire.
- `web_fetch` now rejects loopback, private, link-local, reserved, and other
  non-public resolved destinations on every redirect connection, closing an
  SSRF path to local services and cloud metadata endpoints.

### Added

- `UAGENT_HEADLESS_PROGRESS=1` echoes every durable event as one stderr line.
  It is set for background children, whose stderr already lands in the log the
  parent polls, so a delegated run is traceable while it works; the stdout
  answer contract is unchanged.
- Attached documents are parsed deliberately on OpenRouter routes: requests
  that carry one send the `file-parser` plugin, with the engine chosen by
  `UAGENT_PDF_ENGINE` (free `cloudflare-ai` by default) rather than left to an
  unseen provider default. A route that refuses documents outright is now
  negotiated down like image input, and the attachment continues as a path the
  model can reach another way. Once a route is known to refuse documents, none
  is encoded for it again: the part is never built, so a large file is not read
  and base64'd only to be stripped.

### Changed

- The working row names the active route exactly as the idle row does. When a
  rolling reasoning ticker is competing for the same columns the route yields
  first: it does not change during a turn, and a fully qualified route id can
  otherwise leave the ticker too narrow to read.
- Streaming markdown hands a whole chunk to stdio once instead of taking the
  stream lock for every character, which was about a third of render cost, and
  `TerminalSafe` no longer rebuilds a string byte by byte when nothing in it
  needs escaping.
- The reasoning ticker normalizes its buffer once per drawn frame rather than
  once per streamed token: the renderer applies the transform, so the ui layer
  still owns what "displayable" means.
- Subagents default to 100 model rounds and 240 tool calls, up from 25 and 60 —
  the parent runs unbounded, and cost, wall clock and the session budget are
  the limits that protect a delegated run. A call may set `max_steps` or
  `max_tool_calls` for one child.
- `web_fetch` downloads up to the attachment budget rather than a fixed 2 MiB,
  which had truncated an ordinary arXiv paper, and it now points at `run` plus
  `attach` when the bytes are a document rather than markup.
- Converted HTML keeps its headings, table cells and `<pre>` indentation, and
  no longer leaks attribute markup into the text.
- A resumed session redraws the diff an edit produced instead of collapsing it
  to a grey summary line. Receipts are kept beside the transcript, so the
  model's copy of a tool result is unchanged, and they leave when their turn
  does.
- Tool calls are always shown in full. Only results are shortened outside
  `/verbose`, so a decision is never abbreviated in the scrollback while its
  output is.
- Hosted `web_search` is OpenRouter's `openrouter:web_search` only. The OpenAI
  Responses backend is gone, `UAGENT_WEB_SEARCH_BACKEND` takes `auto`,
  `openrouter` or `off`, and the model-facing tool, citations, bounds and usage
  accounting are unchanged.
- `subagent` is bounded per turn by `UAGENT_SUBAGENT_CALLS_PER_TURN` (32)
  instead of by the live-activity slot count, so sequential delegation waves are
  no longer refused while no child is running. Concurrency remains bounded by
  `UAGENT_MAX_BACKGROUND_JOBS` and cost by the session budget.
- Request payloads reuse the serialized prefix of the previous request instead
  of re-serializing the whole history, byte for byte, so provider-side prefix
  caching is unaffected.
- Redirected terminal output is line buffered rather than unbuffered, so the
  renderer's existing flush budget governs writes.
- Permission modes and the `run` yield bounds are single-sourced in
  `include/core/limits.h`.

### Removed

- The `openai_stream` fuzz target, which asserted nothing beyond "does not
  crash"; SSE framing remains fuzzed.

## v0.5.0 - 2026-08-20

### Added

- One provider-independent model-selection grammar for the conversation,
  image, subagent, web-search, and memory routes.
- Provider context, cache, cost, endpoint, and session-wide usage reporting.
- Bounded `web_fetch` support for retrieving cited pages without a browser.
- A quality-aware efficiency harness comparing normal and forced-compaction
  runs across request count, tool use, tokens, cache, cost, latency, RSS,
  schema size, and binary size.

### Changed

- Compaction now retains recent real user instructions independently of the
  generated handoff, reinjects runtime context, and preserves prior summaries
  within a bounded prose budget.
- Context-overflow recovery, provider usage normalization, reasoning replay,
  status presentation, and semantic observability now share centralized,
  provider-neutral paths.
- Native mode now executes only structured provider tool calls; the text
  compatibility protocol is enabled only after an explicit provider downgrade.
- The working row reports used and total context through the same formatter as
  the persistent status row.
- Foreground activity handling, terminal input, notices, and configuration
  reporting were consolidated and hardened while preserving the native
  single-binary runtime.

### Fixed

- Cache-token extraction across OpenAI-compatible provider response shapes.
- Strict chat-template compatibility by keeping exactly one initial system
  message and normalizing later harness context as user messages.
- Linux warnings-as-errors failures, Python and C++ formatting drift, terminal
  escape fragmentation, background wakeups, and session status rendering.

## v0.4.0 - 2026-08-13

- Published Linux x86_64, Linux ARM64, and macOS ARM64 archives with SHA-256
  checksums. See the GitHub release for the complete generated notes.
