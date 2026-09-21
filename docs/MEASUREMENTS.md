# Measurements and estimates

µAgent keeps observed provider usage separate from values it must predict
before a request is sent.

## Exact or provider reported values

- Tool schema size is the UTF-8 byte length of the exact serialized JSON sent
  for the active tool array. The Tools view reports bytes, not inferred tokens.
- Input, output, reasoning, cache, search and cost totals come from provider
  usage fields. A missing value remains unreported; µAgent does not infer cost.
- Request, model and tool durations use monotonic clocks. Side agent durations
  are shown separately because concurrent durations cannot be summed into wall
  time.
- Background and delegated usage is tagged with its parent turn. It is added to
  that turn and the session after the child finishes, including completion that
  happens after the parent response.

## Explicit estimates and policies

- **Estimated context tokens:** before a provider returns usage, µAgent
  estimates the serialized request size and divides bytes by
  `kEstimatedBytesPerToken` (currently 4). Tokenizers vary by provider and
  model, so the terminal and web UI label this value `est. ctx`. It drives
  proactive compaction and request guards; it is never used as billing usage.
- **Response reserve:** when no output token cap is configured, compaction
  reserves one quarter of the advertised context window for the response via
  `kDefaultResponseReserveDivisor`. This is a safety policy, not an observed
  model limit. `UAGENT_MAX_OUTPUT_TOKENS` replaces it with an explicit cap.
- **Automatic compaction threshold:** `UAGENT_AUTO_COMPACT_PCT` defaults to 85
  percent. It is a configurable policy threshold. `UAGENT_AUTO_COMPACT_TOKENS`
  can provide an explicit absolute threshold.
- **Streaming batches:** the web worker coalesces adjacent text deltas up to 8
  KiB or 12 ms and publishes usage at most every 100 ms. These named transport
  policies reduce browser work; terminal events and final text remain exact.
- **Provider retries:** chat and side requests allow three total attempts for
  classified transient failures. The shared fallback is capped exponential
  backoff with jitter. A valid provider `Retry-After` value is treated as the
  minimum delay and receives up to 250 ms of jitter. Chat retries stop at the
  remaining turn deadline, and side retries stop at their owning tool deadline.
  A streamed request is replayed only before visible text, tool calls,
  annotations or usage arrive.
  A provider can still have processed a request before returning a transient
  failure, so retries can repeat upstream compute or cost; these APIs do not
  expose a portable idempotency contract for model generation.
- **Repeated tool recovery:** the third identical valid call adds a model-facing
  advisory and the sixth adds a direct instruction. The twelfth stops the paid
  loop. Deterministically rejected calls stop on the third equivalent rejection
  because unchanged invalid input cannot produce new evidence.

Capacity limits, timeouts and retention caps are operational policies. Their
defaults and bounds live in the configuration registry and are documented in
`OPERATIONS.md`; they are not presented as measurements.
