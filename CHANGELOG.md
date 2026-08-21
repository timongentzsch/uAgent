# Changelog

## Unreleased

### Fixed

- Streaming markdown measured display width by counting UTF-8 lead bytes, so a
  line containing CJK, emoji, or combining marks reported the wrong row count
  and redrawn tables erased the wrong number of rows.
- The compiler-family guard in CMake was assigned an unevaluated expression and
  always tested true, so warnings-as-errors applied to every compiler and the
  "sanitizers require Clang or GCC" refusal could never fire.
- `web_fetch` now rejects loopback, private, link-local, reserved, and other
  non-public resolved destinations on every redirect connection, closing an
  SSRF path to local services and cloud metadata endpoints.

### Changed

- `web_fetch` downloads up to the attachment budget rather than a fixed 2 MiB,
  which had truncated an ordinary arXiv paper, and it now points at `run` plus
  `attach` when the bytes are a document rather than markup.
- Converted HTML keeps its headings, table cells and `<pre>` indentation, and
  no longer leaks attribute markup into the text.
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
