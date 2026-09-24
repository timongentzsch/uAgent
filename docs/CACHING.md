# Prompt caching

What µAgent sends to make provider prompt caching work, what reduces reuse, and
how to measure it. A cache policy is not evidence of a cache hit; check
provider-reported usage on representative conversations.

## Requests

CLI and web sessions share one request encoder and one usage accounting path.

- **OpenAI.** Matching prefixes cache automatically. Responses requests to the
  official OpenAI host carry `prompt_cache_key`, a hash of the session ID. See
  [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching).
- **Anthropic.** Requests set top-level
  `cache_control: {"type":"ephemeral"}` for automatic caching, plus a
  breakpoint on the system block so the tools and system prefix stay reachable
  once a long tool round exceeds the automatic 20-block lookback. See
  [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching).
- **All routes.** Every model request carries an `X-Session-Id` header so a
  proxy can keep account affinity across wire dialects. Account failover in
  such a proxy can still move a request to a cold cache.

## What changes the prefix

The static system prefix stays stable, unchanged wire encoding is memoized, and
provider-native reasoning is replayed as received. These operations still
change earlier input and can cost cache reuse:

- compaction, attachment pruning and old tool-result pruning;
- updated runtime context, which removes its previous copy before appending;
- permission, adaptive-instruction, tool-availability and effort changes;
- forks, which get a new session identity (resumed sessions keep theirs).

µAgent sends no keepalive requests to hold a cache warm, including between
Fusion handoffs; they cost a model call and cannot guarantee a hit.

Hit rate alone is not the goal: redundant context also costs tokens and can
hurt results. Change pruning or prompt semantics only with a measured
comparison of quality, latency and cost.

## Measurement

`Usage` splits prompt tokens into disjoint fresh input, cache reads and cache
writes; OpenAI reports cached tokens inside input and Anthropic reports them
separately. The read share is `cache_read / (input + cache_read + cache_write)`.
Missing usage is unavailable evidence, not a miss. `/cost`, `/status` and the
web message details use the same accounting.

To compare a change, record route, context size, cache reads and writes,
time to first token, completion time and task outcome before and after. Keep
raw prompts and credentials out of committed measurements.
