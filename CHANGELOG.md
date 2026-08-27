# Changelog

## Unreleased

### Added

- The composer answers the two gestures every other agent CLI has: Shift+Enter
  (or Alt+Enter) keeps a draft open on a new line, and Tab completes a command
  from the rows that appear under the draft as soon as it starts with `/`. The
  rows live inside the block the composer erases, so they never reach
  scrollback, and Enter still submits whichever way the terminal spells it.
- `/init`, `/review [TARGET]` and `/diff` run as ordinary turns, with `/resume`
  and `/permissions` as aliases for `/sessions` and `/yolo`.
- A call that needs approval offers three answers instead of yes or no: once,
  always for this tool — and, for a shell call, that command's first word —
  for the rest of the session, or a refusal in words, which denies the call and
  reaches the model as steering rather than a bare no.
- An idle Ctrl+C asks before it exits: the status row says `ctrl+c again to
  quit`, and the confirming press still leaves through the signal path, so the
  terminal is restored the same way and the shell still sees 130. While a turn
  or a child process is running, SIGINT keeps killing what it always killed.
- A delegated child reports why it stopped. The headless envelope gained a
  `stop` object — the reason in a vocabulary a caller can branch on
  (`max_steps`, `max_tool_calls`, `turn_deadline`, `turn_cost`,
  `session_budget`, `repeated_calls`, `completed`), what it consumed, and the
  limits that were in force. `subagent` runs its child with `--json` and
  renders that, so a parent sees `[child stopped: max_tool_calls after 1
  step(s) … rerun with that ceiling raised, or use this partial result as it
  is]` instead of guessing from prose. A child that produces no envelope has
  its raw output returned and labelled as raw.
- `subagent` takes `max_seconds`, `max_cost` and `memory`. A caller may tighten
  any of them freely; loosening is bounded by the host ceiling
  (`UAGENT_SUBAGENT_TIMEOUT`, the session budget, the session's memory switch)
  and a clamp is reported in the result rather than applied silently. Memory
  can be denied to a child but never granted beyond what the session allows.
- `uagent_info topic=routes` reports the model routes and named providers this
  build can reach, the selection grammar that names one, and whether each
  carries a credential — never the credential itself. The route table was the
  one part of the configuration the agent could not read: `UAGENT_PROVIDERS` is
  a composite secret, so `topic=config` returns its metadata and withholds its
  value, leaving a model to guess a selection. A guess that resolves to the
  wrong endpoint fails as an authentication error rather than as the typo it
  is, which is what 37 `invalid local API key` and 12 `unknown provider`
  failures in this machine's history look like.
- `uagent_configure` can write `UAGENT_PROVIDERS`, the one composite secret,
  without a credential ever passing through a tool argument. An `api_key` must
  be an exact environment-variable reference, which the config loader expands
  at startup; `base_url` must be an http(s) endpoint carrying no credential,
  query or fragment; any other interpolation is refused. The approval preview
  diffs the pretty-printed JSON with every literal key already on disk
  redacted, so a route can be added or renamed while the secret stays where the
  user put it. Setting a plain secret is still refused, but unsetting one is
  not: removing a credential needs no credential.
- `delete_file` removes a regular file and reports the removed lines as a
  receipt, so an approval prompt shows what is about to be lost. Directories,
  non-regular files and missing paths are refused by the same path policy the
  other file tools use.
- `write_file` and `edit_file` preview the diff they would apply at the
  approval prompt instead of a one-line summary, so consent is given against
  the change rather than against its description.
- Math in answers renders to Unicode through a shared transliteration table:
  Greek, operators, super- and subscripts, fractions and roots. Unknown
  commands survive verbatim rather than being dropped.
- A session whose executable has been replaced on disk says so at the next
  turn, which closes the loop after an improvement round is installed.
- `benchmarks/eval.py` scores end-to-end agent behavior against declarative
  scenarios in `benchmarks/scenarios/*.json` — answer content and shape, files
  read, forbidden tools, model rounds, batch width, deduplication, request
  bytes, workspace immutability — and compares the result with a committed
  baseline. CTest gates it, so a change that costs rounds, drops a batch or
  loses an answer fails the build instead of being argued about. The same
  scenarios replay against a live route with `--run --model`.
- `uagent --emit-reference` now also emits `system-prompt.md` and `tools.md`,
  covering every advertised tool with its schema size and the sessions that pay
  for it, and the manifest carries a prompt and tool-schema digest. The bytes that
  steer the model get the same drift gate as the bytes that configure it: a
  reworded prompt section or a changed tool schema is a reviewable diff.
- `UAGENT_PROMPT_OVERLAY` names a JSON file that replaces base-prompt sections
  for an experiment, so two prompt variants can be measured without rebuilding.
  It changes prompt text only — tools, approvals, capabilities and limits stay
  host-owned — and an absent or malformed overlay leaves the shipped prompt
  byte for byte.
- `tests/integration.py` and `uagent_tests` both select cases with `--test`,
  `-k` and `--list`, and a failing `CHECK` names its file as well as its line.
  Integration cases are discovered from their module in source order and core
  cases from the one list that declares them, so a case that exists runs, and
  no second registration can disagree with the first.
- The `self-improve` skill runs one measured improvement iteration: real-session
  evidence first, then Pareto-constrained proposals across hardware, tokens,
  readability, capability, timing and generality, a slop scan, and committed
  baselines as the gate.
- `benchmarks/slopscan.py` finds slop that already exists in the tree — code
  after an unconditional return, a declaration nothing calls, a block repeated
  in two files, a doc naming a file that is gone — because centralising is what
  creates those and none of them appear in the next diff. Counts are baselined
  and the audit gates them.
- `benchmarks/audit.py` prints that dashboard for one build and fails on a
  regression, including an anti-overfitting check that compares the scenario
  suite's tool mix with the mix real sessions actually used.
- `benchmarks/session_metrics.py` now also reports non-ok results by tool and
  status, time spent per tool, and context filled per tool, so an iteration
  starts from where the cost actually is.
- Eval scenarios carry a `regression` or `capability` tier, and the report adds
  rounds that asked for nothing and the failure-category vector.
- `uagent_info` topic `prompt` reports the system prompt actually in effect:
  base digest and size, the sections an overlay may replace, which conditional
  capability sections are active, and the overlay identity. It reports the
  surface rather than copying message zero back into the transcript.

- `uagent_configure` persists a change to µAgent's own configuration through a
  typed request against registered settings only. µAgent parses the current
  file line-preservingly, applies the edit, re-parses the candidate to confirm
  every value reads back as written and no unrelated setting moved, then shows
  an exact redacted diff. The commit is a compare-and-swap against the bytes
  the human approved, so an external edit in between is rejected rather than
  merged. Credentials are refused, unknown keys are refused, and project scope
  cannot grant its own trust. A project-scope commit re-records the workspace
  trust snapshot, carrying the previously approved `.mcp.json` over unchanged,
  so an approved edit does not make µAgent forget trust it already has and
  cannot silently extend trust to a swapped-in server list.
- Atomic writes now fsync the parent directory after the rename, so a replaced
  file survives power loss rather than only being replaced atomically.
- One authoritative registry describes every `UAGENT_*` setting: default,
  bounds, category, reload policy and sensitivity. Runtime getters, the config
  loader, diagnostics and the generated documentation all read the same
  descriptors, and a getter naming an unregistered setting fails to compile.
- `uagent_info` answers questions about the running binary — status, flags,
  slash commands, configuration schema with effective values and provenance,
  and the live tool surface. It is inspect-only, assembled on demand, and
  reports secrets only as set or unset.
- `/status` gives the everyday view (version, route, effort, approval mode,
  budgets, restart-required settings) and `/debug-config` explains where each
  active value came from. `/context` remains the deep next-request view.
- `uagent --emit-reference DIR` generates release-matched skill references from
  the same registries, with a manifest naming the version and a schema hash.
  CI regenerates and fails on drift, so documentation cannot silently rot.
- Process hardening at startup: core dumps disabled, `PR_SET_DUMPABLE` cleared
  on Linux, and `LD_*`/`DYLD_*` loader-injection variables removed before any
  child process can inherit them.

### Fixed

- `read_path` no longer decodes bytes that are not text. A PNG or an object
  file was read line by line and answered with pages of replacement characters
  that cost context and said nothing. It is now refused by name, and the
  refusal points at `attach`, which hands the same bytes over as an attachment.
- A failed process keeps its log. Completed logs under the result cap were
  deleted as disposable, which holds only when the caller received the whole
  thing — and a failure is summarised on its way back, bounded to 2,048 bytes
  for a delegated child. A child failing with 2–8 KB of output therefore had
  the remainder destroyed at the moment someone needed it. Failure is now a
  second reason to keep the log as a private artifact, whatever its size, and
  the pointer to it is printed outside the summarised diagnostics rather than
  inside them, where it was being squeezed away.
- A foreground `subagent` is no longer cut off after 30 seconds. It inherited
  `UAGENT_TOOL_TIMEOUT` like any other call, so `background=false` died at the
  per-call budget however long the work took, which made the `max_steps` and
  `max_tool_calls` ceilings unreachable: no child runs 100 model rounds in
  thirty seconds. It is now bounded by `max_seconds` when given and by the turn
  otherwise, as `run`, `scratch` and `activity` already were.
- An `activity` wait lasts as long as it was asked to. The tool inherited
  `UAGENT_TOOL_TIMEOUT`, thirty seconds by default, so a `wait_ms` of five
  minutes — which the schema offers and the caller chose — was cut at thirty
  seconds and cost a model round every half minute; one session in this
  repository's history spent fourteen rounds and seven minutes of wall clock
  that way, in streaks of up to four. Waiting holds no process and no lock, so
  the budget that stops a runaway command had nothing to protect here, while
  `run` and `scratch`, which do occupy a process, were already exempt. The turn
  deadline still bounds a wait, and Escape and queued steering still return
  from one immediately; a truncation now names the deadline that caused it.

- The `edit_file` approval prompt shows the edit that will happen. It applied
  the edits itself to render the diff, so it disagreed with the edit in six
  ways: it could not see a stripped line-number prefix or a normalised line
  ending, it silently dropped an edit that was already applied and everything
  after it, and where the edit refuses outright — an `old` that is absent, one
  that matches several times without `replace_all`, a result past the byte
  limit — it showed a diff of the preceding edits as though they would be
  written. Both paths now go through one function, and a refusal is previewed
  as the refusal.

- An `activity` wait says what ended it. A tool call may not outlive
  `UAGENT_TOOL_TIMEOUT`, 30 seconds by default, so a longer `wait_ms` was cut
  short and still reported as a plain timeout — indistinguishable from the
  requested wait elapsing, and an invitation to give up on an activity that was
  running normally. All three wait paths now report how long they actually
  waited, whether the tool timeout truncated it, and that calling again
  continues. The readiness-marker path was the worst of them: on expiry it
  returned the log with no indication that the marker had never appeared.

### Changed

- Stopping an activity is an operation, not a tool. `activity_stop(id)` became
  `activity(operation=stop, id=...)`, beside the list, poll, wait, write and
  resize operations that already own the same id, capability set and detached
  visibility. It was the last part of the domain reachable under a second name,
  and one schema fewer is one fewer to choose wrongly between. Stop is still a
  mutation and is still approved as one.
- The interactive surface reads like the tools it sits beside. The banner
  states the version and working directory, the startup context row shows three
  sources and a count instead of every path it knows, and the first prompt is
  preceded by the few commands worth knowing. The idle row carries the
  headroom as `NN% left` and a `/help for shortcuts` hint, the working row says
  `Working` and `Esc to interrupt`, and command descriptions took the wording
  Codex and opencode already share.
- A blocking wait names what it waits on. The working row shows `wait ·
  activity 42` for as long as the call blocks, `N activity(s)` became `1
  activity` or `2 activities`, and a detached command says it keeps running
  while you work rather than that its completion is observational.
- The request carries less of itself. Memory moved out of the system message —
  the prefix a provider caches, and the place authority lives — into the
  runtime context, where a change costs only what follows it. The Capabilities
  section stopped restating the tool schemas it introduces, and `adapt_system`,
  `uagent_info` and `subagent` stopped saying the same thing twice in one
  description. Hermetic system prompt 2,993 to 2,712 chars, schemas 8,534 to
  8,193, always-on 5,295 to 5,101 B; every scenario sends 1,139 fewer
  characters at an unchanged score. What a shorter description does to a
  model's behaviour is not visible to a scripted scenario and needs a live A/B.
- `benchmarks/audit.py` reports the bill a used machine pays beside the
  hermetic floor. The probe runs with `--no-memory` in an empty HOME, so stored
  memories and installed skills — charged on every real request — were
  invisible to it. A second probe copies them into a throwaway HOME; the row is
  reported and never gated, because one machine's memories cannot baseline
  another's.
- `benchmarks/slopscan.py` exits non-zero on a count above its baseline without
  being asked to. The old `--check` flag meant a caller who wired the scan into
  a script and forgot it got a silent pass, which is the one failure a gate
  cannot afford. CI now runs the scan, so the baseline holds without anyone
  remembering to run it.
- `slopscan.py --self-test` runs every check against `tests/fixtures/slop`,
  which plants exactly one instance of each, and fails unless each check finds
  precisely its own. Every real count is zero, and a check that had quietly
  stopped matching anything would print the same thing.
- The repeated-explanation check now joins each run of prose before looking for
  a sentence. It matched within single lines, and prose here wraps at about 79
  columns, so nearly nothing it was meant to catch could reach it. A run ends
  at a blank line or at any list item, heading, table row or quote, because
  joining a bullet list produces sentences spanning two bullets that nobody
  wrote, and fenced code is skipped rather than read as prose.
- `ruff` enforces `ARG` and `ERA` from `pyproject.toml`, the set the improvement
  loop already applied by hand, so CI and the loop check the same thing.
- Session journals record a digest of each tool call's arguments and the error
  code of each failed result. Values still never reach the journal, so a later
  reader can tell an identical repeated call from a new one, and name which
  failure a tool hit, without anything leaking.
- `uagent_info` is withheld from lean delegated children, which saves about a
  kilobyte of schema on every one of their requests. A child is briefed for a
  task rather than left to introspect the harness.
- Diff receipts show whole lines and a realistic edit in full, capped at 400
  lines rather than 80, so a review is not truncated where it matters while a
  very large replace still cannot take the scrollback with it.
- The rolling activity ticker and the styled-block renderer moved to
  `include/core/style.h`, leaving one implementation of chasing a live edge.
- A repeated `grep` with identical arguments collapses to a receipt when its
  result is byte-identical and still in recent context, matching `read_path`.
- `uagent_configure` is registered only where a person can actually approve it.
  The approver already denied every non-interactive call, so a piped or
  delegated run was paying about a kilobyte of schema per request for a
  guaranteed refusal.
- Prompt authoring moved from `include/agent/protocol.h` to
  `include/agent/prompt.h` plus `src/agent/prompt.cc`, leaving the header with
  tool-call parsing alone. Rewording the prompt now rebuilds one translation
  unit instead of nine.
- `/effort` and `/variant` now persist to the same saved selection `/model`
  writes. Previously both changed only the live session while the bundled skill
  documented them as persistent; a session with nothing saved says so instead.
- The release-installed skill tree outranks the mutable `~/.uagent/skills`
  copy, so a stale user-level copy can no longer shadow release-matched
  content. A workspace skill still overrides both.
- The `uagent-config` skill is a router: it selects one generated reference or
  `uagent_info` per question instead of loading a 243-line document every time.
- Editing `~/.uagent/.config`, a project config, the trust store or `.mcp.json`
  through the built-in file tools now requires an explicit human decision that
  `--yolo`, `UAGENT_APPROVAL=yolo` and `/yolo` cannot bypass, and that a
  headless or delegated run denies rather than assuming.

### Performance

- Turn-time hot paths are unchanged: across 15 interleaved Release runs every
  benchmark delta sits inside the baseline's own interquartile spread.
  Descriptors are `constexpr` and each binding resolves its descriptor at
  compile time, so config load performs no lookups.
- Startup is unchanged within measurement drift; repeated 120-sample
  interleaved runs disagreed on the sign of a sub-0.25 ms difference, so no
  regression is claimed. The binary grows 201 KB, of which 14 KB is
  relocatable descriptor data. No static initializers were added.

## v0.8.0 - 2026-08-25

### Added

- Routes now declare `wire_api` independently as `chat_completions`,
  `responses`, or `anthropic_messages`. Canonical conversation, function-call,
  citation, usage, retry, and rendering state stays provider-neutral while
  request encoding, headers, endpoints, and streamed event decoding live in
  focused wire adapters.
- OpenAI Responses and Anthropic Messages support fragmented answer/reasoning
  streams, native function calls and results, opaque reasoning replay, hosted
  search results and citations, normalized usage, structured errors, and
  non-mutating retries. Anthropic `pause_turn` continuations replay encrypted
  search data and are bounded per turn.
- `UAGENT_PROVIDERS` accepts explicit `wire_api` and `hosted_tools` metadata at
  provider or model scope. The same metadata is preserved by named-route
  switching and delegated children; unknown transport/protocol values are
  rejected rather than guessed.
- CodeQL analyzes C++ on main, pull requests, and a weekly schedule. Tag release
  jobs create one deterministic checksum manifest, sign it keylessly with
  Sigstore, attest every published asset through GitHub OIDC, and scope elevated
  permissions to that trusted job.

### Changed

- `UAGENT_WEB_SEARCH_BACKEND=auto` prefers native hosted search only when the
  active Responses or Anthropic route explicitly declares `web_search`, then
  falls back to the separately configured OpenRouter search function. `openrouter`
  forces only that separate route and `off` withholds both; model names,
  provider labels, and endpoint URLs never imply hosted-tool support.
- Binary archives include the complete release-matched skill tree under
  `share/uagent/skills`. Discovery is relocatable from the executable, source
  installs still refresh the user copy, and CI compares every packaged skill
  file with the tracked source before publication.
- The 4,351-line Python integration suite is now a 52-line ordered runner plus
  domain modules. The 1,773-line C++ tool suite is split across its six existing
  behavioral boundaries. Automated AST/body comparison found all original
  tests unchanged and in the same deterministic group order.
- Nine-run Release medians against v0.7.0 on the same host kept common hot paths
  within 1.5% (TTY Markdown -0.3%, plain SSE +1.0%, headless SSE +1.0%); schema
  size stayed 6,420 bytes and the binary grew 2.8%. A cached 128-message Chat
  payload encoded in about 8 µs; full Chat, Responses, and Anthropic adapter
  encodes took about 131, 266, and 234 µs. Hermetic quality, request count,
  tool count, usage, and roughly 10.3 MiB peak RSS were unchanged.

### Fixed

- Ordinary terminal input can no longer grow the decoder queue without bound.
  An over-limit unread burst is dropped atomically, bracketed paste remains
  streamed and bounded, and the decoder immediately accepts fresh input after
  the overflow is acknowledged.
- Chat Completions payload caching strips opaque Responses/Anthropic replay
  state after a route switch. Responses continuations preserve completed output
  item order, avoiding duplicate or reordered function calls when native search
  and client tools occur in one response.
- Binary packaging disables macOS AppleDouble metadata, rejects unsafe archive
  member types and paths, and verifies the installed executable and exact skill
  contents before upload.

### Security

- New releases publish GitHub build-provenance attestations and a keyless
  Sigstore bundle for `SHA256SUMS`; the composite Action verifies repository,
  workflow, tag ref, hosted builder, attestation, and selected digest before
  extraction. The immutable v0.7.0 path retains its original checksum-only
  verification.

## v0.7.0 - 2026-08-25

### Fixed

- Tool-originated attachment messages identify the originating call beside each
  path, so images queued by parallel `attach` calls remain unambiguous on the
  next model inference.
- Activity polling is semantic rather than a blind identical-call counter.
  Productive reads reset the state; two consecutive no-change polls prompt one
  bounded `wait`; a third terminates cleanly. Mixed batches with useful work do
  not count as a polling loop.
- The SIGCHLD wake dispatcher no longer writes its marker byte to unused file
  descriptor slots. Under a PTY those zero-valued slots were stdin, so enough
  child exits could inject control bytes into a long tool-call record.
- Persistent-composer output is record-framed across arbitrary pipe reads.
  Long multiline calls remain complete and cyan on every physical line, live
  Markdown tails stay replaceable, and completed records enter scrollback
  atomically rather than being mistaken for streaming text.
- Equivalent deterministic tool rejections now stop after the third round,
  before a fourth model request. Repetition checks use canonical execution
  arguments, so irrelevant provider-materialized fields cannot disguise a
  retry or turn a non-blocking activity into a wait.
- Activity input now distinguishes invalid dimensions, a non-PTY activity, a
  closed input descriptor, descriptor duplication failure, and `ioctl`, write,
  or signal failure, with the matching error category and actionable message.
- Delegated-child failures report the configured route, failure stage, bounded
  head/tail diagnostics, and a remedy. The harness explicitly preserves the
  selected provider, model, pricing, and privacy policy instead of silently
  falling back.
- Delegated children no longer duplicate the parent's always-on memory block,
  which produced a misleading truncation warning in child logs.
- Public `tool.call` JSONL no longer exposes raw argument values. The new
  `uagent.event.v2` projection carries only argument keys and JSON types, a
  normalized operation, and structured validation issue metadata;
  sensitive debug/internal history remains complete.

### Changed

- `web_search` now accepts one required `queries` array instead of two
  overlapping singular/plural inputs.
- Tool argument validation now reports stable issue codes and fields. Provider
  arguments remain unchanged in history, while a separate canonical copy drives
  validation, approval, policy, and execution.
- Whole-file replacement is now the dedicated `write_file` operation;
  `edit_file` accepts only ordered exact replacements. Empty writes remain valid,
  and neither schema exposes mutually exclusive optional operations.
- `activity` now requires an explicit `list`, `poll`, `wait`, `write`, or
  `resize` operation. Irrelevant provider-materialized fields are removed only
  from the execution copy, so field presence can no longer turn a wait into a
  write or resize.
- `attach` no longer carries a per-turn call cap of its own. Four was a limit
  on the wrong axis: it withdrew the tool from the schema once the count was
  reached, while queued count and total byte limits already refuse with a
  reason.
- Seven-run Release medians against v0.6.0 on the same host showed no hot-path
  regression above 0.7%: TTY Markdown was -0.2%, plain SSE +0.7%, and headless
  SSE -0.3%. The executable grew 3.3% and the built-in schema grew 298 bytes
  (4.9%) for the explicit file/activity contracts.

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
