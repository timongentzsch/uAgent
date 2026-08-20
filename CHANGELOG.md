# Changelog

## v0.5.0 - 2026-08-20

### Added

- One provider-independent model-selection grammar for the conversation,
  advisor, image, subagent, web-search, and memory routes.
- Tool-capable advisor calls, including bounded workspace exploration through
  the caller's active read-only tools.
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
