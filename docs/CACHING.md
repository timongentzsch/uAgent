# Prompt caching

CLI and web workers use the same request encoder and usage accounting. Caching
is enabled, but a configured cache policy is not evidence of a cache hit or an
optimal hit rate. Check provider-reported usage on representative conversations.

## Provider contracts

- **OpenAI:** matching prefixes cache automatically. Official Responses routes
  send a stable session-scoped `prompt_cache_key`. GPT-5.6 and later default to
  implicit caching with a 30-minute minimum TTL; earlier models use different
  retention controls. Keep instructions, tools and earlier input stable. Newer
  APIs support append-only configuration/tool updates, but these require model
  and endpoint support. See [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching).
- **Anthropic:** requests use automatic `cache_control: {"type":"ephemeral"}`
  plus a system-block breakpoint. The latter keeps the tools/system prefix
  reachable when conversation growth exceeds the automatic 20-block lookback.
  The default TTL is five minutes. One-hour caching costs more to write and
  should be chosen from measured reuse intervals, not enabled indiscriminately.
  See [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching).

No public-API-only cache options are forced onto compatibility or subscription
endpoints. The local subscription proxy constructs its own upstream envelopes:
Codex uses a stable cache key; Claude enables automatic caching and preserves
explicit text-block cache controls. uAgent sends `X-Session-Id` on all model
requests so the proxy can keep account affinity across wire dialects. Account
availability, cooldown and failover can still move requests to a cold cache.

## Prefix stability and tradeoffs

The harness keeps its static system prefix stable, memoizes unchanged wire
encoding, preserves provider-native reasoning replay, and avoids rewriting
unchanged runtime context. Memoized JSON encoding reduces local work; it is
separate from provider prompt caching.

These intentional operations can reduce reuse:

- Compaction, attachment pruning and old tool-result pruning change history.
- Changed runtime context removes its previous copy before appending the update.
- Permissions, adaptive instructions, tool availability and effort changes can
  change earlier provider input or its hidden configuration.
- Forks use new session identities; resumed sessions retain their identity.

Maximizing hit rate alone is not the objective: retaining redundant context also
costs tokens and can hurt task performance. Do not disable pruning or change
prompt/permission semantics without a measured quality, latency and cost comparison.

## Measurement

`Usage` normalizes fresh input, cache reads and cache writes into disjoint counts.
OpenAI reports cache counts inside total input; Anthropic reports them separately.
The read share is `cache_read / (input + cache_read + cache_write)`. Missing usage
is unavailable evidence, not a measured cache miss. CLI statistics and web message
menus consume this same native accounting.

Regression tests cover both catalog effort formats, stable session headers on
Responses/Anthropic tool continuations, both Anthropic cache breakpoints,
identical cached/uncached request encoding, and equivalent cross-provider token
accounting. They establish the request contract, not upstream cache performance.
The proxy ledger measures only traffic through that proxy, across its clients;
its prefix diagnostics explain local input changes and cannot prove a provider
cache hit. For a performance comparison, record route/account, context size,
cache reads/writes, TTFT, completion time and task outcome before and after the
change. Keep raw prompts and credentials out of committed measurements.
