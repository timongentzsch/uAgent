# Prompt caching

What µAgent sends so providers can cache prompts, what reduces reuse, and
how to measure it. A cache policy is no evidence of a hit; check
provider-reported usage on representative conversations.

## Requests

CLI and web sessions share one request encoder and one usage accounting path.

- **OpenAI.** Matching prefixes cache automatically. Every Responses request,
  to the official host or to a proxy in front of it, carries
  `prompt_cache_key`, a hash of the session ID. See
  [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching).
- **Anthropic.** Requests set top-level
  `cache_control: {"type":"ephemeral"}` for automatic caching, plus a
  breakpoint on the system block so the tools and system prefix stay reachable
  once a long tool round exceeds the automatic 20-block lookback. Only the
  system messages that open the conversation form that system block; a
  harness note added later is sent in place as a user text block marked
  `[harness note, not a user message]`, so it never rewrites the cached
  tools and system prefix. See
  [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching).
- **OpenRouter.** Most providers behind it cache matching prefixes by
  themselves; some, such as Anthropic's models, only when asked, so every
  request to an endpoint that declares `prompt_cache_control` (OpenRouter
  does) carries the same top-level `cache_control`. See
  [OpenRouter prompt caching](https://openrouter.ai/docs/features/prompt-caching).
- **Panels.** A panel's request ends in a context note that changes every
  time and is never stored. The last stored message carries an explicit
  breakpoint (`cache_control` on Anthropic and on Chat Completions endpoints
  that declare `prompt_cache_control`), so the stored history is read from
  cache instead of the moving automatic breakpoint landing on the note.
- **Compaction.** The summary request is the live request (same system
  messages, tools and conversation) plus one instruction, so the conversation
  is a cache read. Chat Completions and Responses requests also set
  `tool_choice: "none"`; Anthropic requests do not, because changing
  `tool_choice` drops the cached messages, and the instruction forbids tool
  use instead. A conversation that no longer fits the context window is
  summarized from a bounded transcript with no tools.
- **All routes.** Every model request carries an `X-Session-Id` header so a
  proxy can keep account affinity across wire dialects. Account failover in
  such a proxy can still move a request to a cold cache.

## What changes the prefix

The static system prefix stays stable, unchanged wire encoding is memoized,
and provider-native reasoning is replayed as received. These still change
earlier input and can cost cache reuse:

- compaction, attachment pruning and old tool-result pruning;
- tool-availability changes (`/tools`, an MCP server's list, a model switch
  that changes whether `web_search` can run) and effort changes. Turn
  progress never changes the offered tools: `activity` is offered from the
  first request, and a tool at its per-turn limit stays offered and refuses
  the call;
- forks, which get a new session identity (resumed sessions keep theirs).

A new day, a changed approval mode or any other harness note does not: the
note is appended as a new message and the earlier ones stay in place. That
holds on every wire; on Anthropic it is true because only leading system
messages are lifted into the top-level `system` block. Instruction
files and memory are read once at start, so editing one, or saving a memory,
costs nothing until a new or restarted session.

µAgent sends no keepalive requests to hold a cache warm, also not between
`fusion` skill handoffs: they cost a model call and cannot guarantee a hit.

Hit rate alone is not the goal: redundant context also costs tokens and can
hurt results. Change pruning or prompt semantics only after measuring
quality, latency and cost.

## Measurement

`Usage` splits prompt tokens into disjoint fresh input, cache reads and cache
writes; OpenAI reports cached tokens inside input and Anthropic reports them
separately. The read share is `cache_read / (input + cache_read + cache_write)`.
Missing usage is unavailable evidence, not a miss. `/cost`, `/status` and the
web message details use the same accounting.

To compare a change, record route, context size, cache reads and writes,
time to first token, completion time and task outcome before and after. Keep
raw prompts and credentials out of committed measurements.
