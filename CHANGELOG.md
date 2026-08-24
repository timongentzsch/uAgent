# Changelog

## Unreleased

### Fixed

- Long multiline tool calls retain their action colour across persistent-composer
  repaints and are no longer shortened at 2,048 characters. Rejected calls also
  show their attempted arguments instead of a bare tool name.

### Changed

- `attach` no longer carries a per-turn call cap of its own. Four was a limit
  on the wrong axis: it withdrew the tool from the schema once the count was
  reached, so a model handling five screenshots lost the tool rather than being
  told why. What one request may carry is already bounded by the queued-count
  ceiling (`UAGENT_PENDING_ATTACHMENTS`) and the total byte budget
  (`UAGENT_ATTACHMENT_MB`), both of which refuse with a reason.

## v0.6.0 - 2026-08-24

### Fixed

- Ctrl+C, SIGTERM or SIGHUP out of the interactive composer left the terminal
  in raw mode: the handler reset colour and bracketed paste but never restored
  the line discipline, so the surviving shell had no echo and no line editing
  until `stty sane`. The composer now publishes its saved and raw `termios` to
  the signal layer, which restores the cooked settings before `_exit`, and
  writes its escapes to the real terminal rather than into the REPL's own
  stdout pipe, where they were discarded on the way out.
- Ctrl+Z suspended the agent in raw mode and resumed it without repainting.
  SIGTSTP now drops raw mode and bracketed paste before stopping, and SIGCONT
  re-arms both and asks the event loop for a full repaint.
- Terminal escapes in model, tool and file content were only neutralized when
  stdout was a TTY, so a redirected transcript kept them verbatim and replayed
  them at whoever later ran `cat` on it. Sanitizing is now unconditional.
- Four `std::filesystem::exists` calls used the throwing overload. Under
  `-fno-exceptions` a lookup error there — EACCES on a parent directory, ELOOP,
  ENAMETOOLONG — was an `abort()` mid-turn with no session save.
- An OSC or DCS reply from the terminal — a colour query answer, a clipboard
  payload — surrendered its introducer as an unknown sequence and then typed
  its payload into the composer. String sequences are now decoded to their BEL
  or ST terminator, bounded, and discarded past the bound; X10 mouse reports
  (`ESC [ M b x y`) no longer leak their three coordinate bytes as text.
- A table header was reprinted by erasing as many rows as its *markdown source*
  occupied. `**bold**` is eight columns of source and four on screen, so a
  header whose markup crossed a wrap boundary ate a line of transcript above
  the table. Rows are now counted from the rendered line.
- A turn interrupted while its tools were running recorded the outcome but not
  the error, so a headless run cancelled at that moment exited 0 and reported
  whatever partial answer existed. It now exits 1 with `interrupted`, like
  every other interruption.

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

- `NO_COLOR`, `TERM=dumb` and `CLICOLOR_FORCE` are honoured. Colour and
  terminal capability are now separate questions: suppressing colour no longer
  suppresses cursor control, spinners or inline images, and forcing it works
  down a pipe.
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

- File descriptors are owned by a move-only `Fd` type instead of a bare `int`
  paired with a `close` on every early return, and the process supervisor's I/O
  thread is a `std::jthread` whose stop token carries cancellation: a
  `std::stop_callback` wakes the poll, so the shutdown handshake is one call
  rather than a flag plus a manual pipe write.
- Mutex-guarded state carries `UAGENT_GUARDED_BY` annotations, documenting lock ownership, and the build adds `-Wconversion -Wsign-conversion
  -Wshadow -Wold-style-cast`. clang-tidy runs the `bugprone`, `performance`,
  `misc` and `clang-analyzer` families rather than a hand-picked few.
- The streaming markdown renderer bounds the three buffers a model could grow
  without limit — the current line, an unterminated math span, and table rows —
  degrading to plain output at the bound instead of accumulating. Measuring
  rendered rows costs 2.6% on the TTY markdown benchmark.
- `Agent::DrainBackground` splits into the memory-receipt pass and the activity
  delivery pass (146 lines to 35).
- A second fuzz target covers the composer's terminal input decoder: arbitrary
  bytes fed whole and in fragments must decode identically.
- Two constructor parameters and one enumerator were renamed so `-Wshadow` is
  clean on GCC as well as Clang; GCC also warns for a parameter that shadows
  its own field and for an enumerator that shadows a namespace-scope constant.
- `Agent::Turn` was a 385-line function holding two dozen locals; the step loop
  now hands a `TurnLoop` to named phases (`PrepareStep`, `HandleFailedResponse`,
  `HandleEmptyResponse`, `ExecuteToolCalls`, …) that report what the loop does
  next. Behavior is unchanged; the recoveries are individually readable.
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
