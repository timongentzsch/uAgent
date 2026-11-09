# Module review — November 2026

Review for PR #331, based on `7131c8c1` and its refactor commits. This is a
module-by-module boundary review of first-party C++ and TypeScript, not a claim
that every line or every possible execution has been verified. The inventory
contains 313 C++ source/header files and 161 TypeScript/TSX files, including
entry points and declarations; CSS, generated output, dependencies and test
files are outside those counts. Every production module is represented below.
Only the named files and functions received the focused review described here;
other files in those modules remain inventory coverage. Test names identify
specific evidence, not coverage percentages.

The review asks five questions at each boundary:

- **Ownership:** who can change the state, and who releases resources?
- **Errors:** how do failure, cancellation and partial success reach callers?
- **Concurrency:** what admits work, orders replies, cancels it and joins it?
- **API:** do callers use the same types, identities and policy?
- **Tests:** do assertions observe behavior, including adverse ordering?

[State ownership](../STATE_MACHINES.md) inventories the implicit and explicit
machines. This record adds review depth, decisions and proof limits. A retained
implementation means no change was justified by this examination; it does not
mean the entire module has no defects.

## Findings resolved in this PR

| Finding | Change and behavioral evidence |
| --- | --- |
| Mutation exclusion depended on a render | `useAction` admits synchronously; tests exercise same-turn and post-render rejection, independent owners, failure and retry. |
| History reads were incorrectly treated like mutations during the refactor | Explicit concurrent mode with a pending count. Existing history-ordering test caught the regression; added tests settle reads in reverse order and fail one while another remains pending. |
| Upload admission still depended on a render | `useDraftUploads` tracks active conversation IDs synchronously. A test failed before the fix: the duplicate remained pending instead of being refused. The test also permits a different conversation, fails the original upload, checks cleanup, and retries. |
| An older stylesheet response could overwrite a newer refresh | A request revision gates publication after the response body finishes. The regression failed with `old` instead of `new` before the fix. It also covers HTTP/network/body failures, retention of prior CSS, and clearing CSS. |
| Page wheel handler retained its initial transform | Shared wheel binding uses current page transform; browser checks successive anchors, unit checks listener cleanup and surface-specific pan policy. |
| Related navigation and receipt decisions were scattered | One Files location record and pure outgoing-receipt transition. Tests retain false ARIA state and receipt precedence. |
| Lifecycle and socket admission checks were duplicated | `LifecycleBusy` and `WebSocketDevice` centralize shared predicates without moving lock ownership or adding broader exclusions. |
| Batcher unit evidence exercised no timer thread | Added `TestTransientBatcherTimerAndShutdown`: wait for actual timed delivery, stop/join, flush, repeat cleanup, and assert exactly `abc`. Existing synchronous ordering test remains. |

No universal state-machine framework or new cross-module service was added.
The small upload admission set is separate from action busy/error state because
one shell must permit independent conversations to upload simultaneously.

## C++ review coverage

Paths are relative to the repository. Functions listed after a file delimit a
focused read; they do not imply a full-file review.

| Module | Examined boundary | Ownership, errors, concurrency and API decision | Test evidence and limits |
| --- | --- | --- | --- |
| `core` | `include/core/fd.h`, `lease.h`; `src/core/private_store.cc`, `steering.cc` | Descriptor ownership is move-only; file leases retain stable lock files and distinguish advisory presence from writer authority. Private stores validate before replacement. Steering takes queued messages under its mutex and wakes after enqueue. Retain these separate lifetimes; do not replace them with generic booleans. | Filesystem/activity tests exercise ownership and queues; this review does not prove power-loss durability or every syscall failure. |
| `transport` | `src/transport/session.cc`, `sse.cc` | Owned pipes, bounded frames and writes, cancellable polling; SSE carries CR and partial fields between chunks and retains sticky errors. Keep transport framing separate from provider decoding. | `TestSseChunkPartitions` and `TestSseFraming` cover partitions and framing. No parser change; no new fuzz campaign performed. |
| `api` | `include/api/retry.h`, `exchange.h`; `src/api/client.cc`: `Chat`, `WaitForRetry`, `Fetch`, `Prepare` | Per-request exchange ownership and per-client curl lifetime remain explicit. Retry is conditional on replay safety; attempt limits and total deadlines are different. Cancellation and HTTP/JSON failures stay typed in results. Retain provider-progress checks rather than sharing the less restrictive JSON retry policy. | Protocol retry cases and stream partition cases are relevant. Hermetic providers cannot prove behavior of every live service or extreme clock arithmetic. |
| `providers` | `src/providers/selection.cc` | Model selection normalizes route identity, preserves compatible metadata, and treats catalogue discovery as optional. Search workers claim distinct result indices and are joined before publication. Keep per-worker `Api` instances and configured-feature precedence. | Provider integration suite covers mock routes. Catalogue feature drift requires live compatibility checks outside this review. |
| `agent` | `include/agent/activity_supervisor.h`; `src/agent/activity_supervisor.cc`: transition graph, reservations, destructor/start/register; `dispatch.cc`: result budgeting and event projection | The supervisor owns I/O lifetime; interaction and session locks have different scopes. Exit, output drain and result delivery remain separate states. Reservations release capacity through RAII. Result budgeting preserves original diagnostics while limiting model-facing text. No consolidation across these contracts. | Read `TestActivityStateGraph`; it checks legal/illegal transitions, retained terminal inspection and labels. Activity suite adds real process cases. This was not a line-by-line compaction, transcript or memory-store review. |
| `app` | `outcome_store.cc`/header; `transient_batcher.cc`/header; `session_worker.cc`: `DeliverEvent`, `Send`, `Close`; earlier PR lifecycle callers `session_host.cc`, `session_move.cc`, `session_router.cc` | Receipts pin waiting results independently of cache eviction and fail pending work explicitly on disconnect. Batcher serialization preserves text/event order; shutdown stops its thread before flushing. Worker close must not hold its mutex while joining the batcher. Lifecycle helper does not take over mutation rollback. | Receipt lifetime/backpressure tests and synchronous batcher assertions; new threaded test closes a concrete gap. Schedule/config/asset subsystems have integration coverage, but their full implementations were not reread line by line here. |
| `browser` | `src/browser/broker.cc`: state/slot ownership, acquisition, execution and shutdown | Slot lock precedes broker lock; slow runtime calls leave the broker lock. Lease tokens reject a former owner, busy state gates viewer control, and shutdown closes admission before stopping slots. Retain broker/runtime separation. | Read `TestBrowserScheduling`: injected clock tests FIFO, cancellation, stale owner, expiry and shutdown without Chrome. Actual profile and handover behavior also depends on browser integration. |
| `mcp` | `include/mcp/rpc.h`; `src/mcp/invoke.cc`; `server.cc`: shutdown, reaping, bounded reads and spawn descriptor setup | Runtime owns servers and logging threads. Shutdown uses bounded EOF/TERM/KILL stages; RPC identity and notification handling stay distinct. Continuations retain arguments and share a deadline; cancellation returns typed tool cancellation. Do not merge startup, notification and invocation states. | Read contract-helper assertions for protocol identity, schemas, trust and subscription IDs; integration suite covers optional/required startup and continuations. No external MCP service certification. |
| `media` | `src/media/attachments.cc`: MIME tables and raster/video/audio/SVG recognition | One MIME table supplies extension naming; byte signatures can override declarations where intended. Recognition is bounded and separate from conversion. Retain explicit format distinctions. | Attachment encoding suite covers formats/conversions; review depth here is recognition, not every platform converter or malformed-media case. |
| `tools` | `src/tools/tool.cc`: selection, approval, schemas and projections; `policy.cc`: argument-type/size/required-field validation | Typed results and issue codes distinguish invalid arguments, denied policy and execution failures. Approval cannot be weakened by argument-specific policy. Keep MCP-owned schemas distinct from builtin normalization. | Read `TestToolExecutionPolicy` assertions for unknown/nested fields, limits and schema preservation. Integration tests exercise execution; not every tool implementation was individually traced. |
| `cli` and entry | `src/cli/options.cc`; `src/main.cc`: mode dispatch, `Fail`, `ShowSystemPrompt`, `WebMain` | Flag table owns parsing/help; malformed and incompatible modes fail before launch. Web mode admits host settings rather than inheriting session execution settings. Startup failure output follows the requested envelope. | Protocol/CLI and runtime integration suites are relevant. Manual interactive setup and installed-package behavior are separate checks. |
| `ui` | `src/ui/live_region.cc`; input-decoder tests | Live region owns committed text, pending tail and cursor projection. Resize and tail adoption preserve scrollback; presentation remains separate from execution. No worker/conversation authority moves into rendering. | Read decoder cases for chunked paste, escape deadlines and multibyte Meta input. UI integration supplies terminal behavior; no screen-reader user study or exhaustive terminal-emulator matrix. |
| `web` | `src/web/master_auth.cc`: device lookup and socket admission; `master_stream.cc`: snapshot/search and event-stream admission; earlier PR browser/terminal socket callers | Master owns devices; socket origin/device checks share one locked admission path. Streams retain epoch/cursor identity, bounded connection replacement and revocation checks. Snapshot/search delegate runtime data ownership. | Native web integration covers routes/auth/replay; browser suites cover client behavior. Push delivery on physical devices and reverse-proxy deployment remain separate validation. |

The batcher intentionally calls its delivery sink while serializing events.
The inspected sink enters worker state and publishes frames; it must not call
back into the batcher. Moving that call outside the lock without an ordered
queue would allow completion to overtake text. This remains a documented lock
constraint, not evidence that arbitrary callbacks are safe there.

## TypeScript review coverage

Paths below are under `web/src/`; test paths are under `web/tests/`.

| Module | Examined boundary | Ownership, errors, concurrency and API decision | Test evidence and limits |
| --- | --- | --- | --- |
| `app` | `use-draft-uploads.ts`, `use-side-conversations.ts`; `app.tsx` upload wiring | Drafts belong to conversation IDs; upload admission now agrees before and after render. Partial successes remain attached; only this batch's pending IDs are removed. Side-conversation publication checks request identity. Host operations continue through `state/api.ts`. | New real-hook upload test plus existing `attach-mentions.spec.js` end-to-end naming/send assertions. Side-conversation same-turn admission is not exhaustively proven by those tests. |
| `state` | `snapshot-store.ts`, `node-reader.ts`; earlier PR `use-host.ts`, `store.ts`, `apply-event.ts`; `use-transcript-history.ts` anchor/owner setup | Snapshot store owns subscriptions; components select projections. Host reconnect readiness is not socket-open. Receipt transitions preserve unknown/refused/queued distinctions. History keeps its own scroll intent and allows independently ordered reads. | Store transition tests, history-ordering and anchor browser tests. Full stream permutations are not a formal model check. |
| `shared` | `use-action.ts`, `use-resource.ts`, `user-style.ts`, `storage.ts`, `wake-lock.ts`, `pwa.ts`; earlier PR `zoom.ts`, `zoom-surface.tsx` | Action admission and resource cancellation have separate contracts. Storage falls back to memory on denial. Stylesheet publication is latest-request-owned. Wake-lock cleanup handles a late grant; PWA watch cleanup removes listeners. Retain per-surface gesture policy. | Added action and stylesheet regressions; read storage fallback assertions. Source boundary tests validate import policy, not runtime semantics. Wake-lock and PWA behavior remain browser/platform dependent. |
| `features/browser` | `rfb.ts`, `frame.tsx`; broker boundary above | noVNC private access is isolated and pointer sends require connected control. Loading/live geometry shares a frame; disabled controls remain present. Host remains control authority. | Browser/handover specs exercise the integration. noVNC upgrades require these checks; types alone do not prove compatibility with private fields. |
| `features/chat` | Earlier PR `chat.tsx` history action; `state/use-transcript-history.ts` shared owner | Concurrent history responses must be merged by identity, not globally serialized. Read busy count and viewport-follow intent remain independent. | Existing reverse-response history test caught an actual refactor regression; new mixed-failure action test supplements it. This row does not imply a full reread of every message renderer. |
| `features/composer` | `mention.ts`; app upload boundary and attachment callers | Pure naming/token parsing stays separate from upload ownership. Missing attachments degrade visibly; filename deduplication must precede upload. | Read sequential duplicate/rename/mention/send and removed-attachment browser tests. Added same-turn upload test supplies the ordering those sequential cases lacked. |
| `features/files` | Earlier PR `files.tsx`, `page.tsx`, `canvas.tsx`; `tree.tsx` resource/selection handling | One location owner updates modes together; surface transforms remain local. Tree resources suppress obsolete publication through `useResource`; semantic selection stays explicit. | Files ARIA checks and successive wheel-anchor tests. Remote filesystem churn and every preview renderer are not exhaustively covered. |
| `features/library` | `document.tsx`: loading, draft restoration, revision and action handling; earlier PR `library.tsx` | Tab drafts and host revisions solve different problems. Shared mutation owner prevents duplicate save admission; stale revision conflicts must remain host-visible. No generic editor framework added. | Library management browser tests cover open/move/import and geometry; action tests cover the shared mutation contract. Not every conflict interleaving is tested. |
| `features/overview` | `overview.tsx`: resource, projections and submit | Status derives from shared attention state. Folder facts use resource lifetime; session creation remains the supplied command owner's responsibility. No second execution machine added. | Covered by integrated navigation/UI suite, not a dedicated exhaustive overview test. Retained old resource values and error presentation deserve care in future loading changes. |
| `features/pairing` | `pairing.tsx` | Link code is removed from the URL; one action owns pairing. Forbidden responses have specific visible state; other failures reach the reporter. | Shared action regressions and host authentication suite; fresh installed-PWA pairing is a separate platform check. |
| `features/scheduled` | `scheduled.tsx`: drafts, preview effect, revision checks and action closure | Schedule definitions, local editor dirtiness and run status remain distinct. Debounced preview checks cancellation and schedule identity; save includes revision. Runtime owns scheduling. | Read management test that saves, runs, observes completion, changes a check and pauses. DST/timezone runtime coverage is outside this UI boundary review. |
| `features/search` | `spotlight.tsx`: query effect and result identity | Debounced request has an abort owner; hits are keyed to query text before display. Local navigation remains separate from host search. Search failures currently fall back silently; this is retained behavior, not a proven ideal error UX. | Search browser suite; no new failure-presentation behavior introduced. |
| `features/settings` | `config/use-configuration.ts` | Concurrent saves are intentional. Request/shown sequence prevents older snapshots replacing newer ones; pending count covers every save and restart reasons accumulate. Retain this owner rather than using exclusive action admission. | Existing configuration integration plus full browser suite. Same-key concurrent saves and every load/failure ordering are not exhaustively modeled. |
| `features/sidebar` | `folding.ts` and shared `stored` contract | Folding is page-local state persisted for later reads; subscriptions notify this page. Do not claim live cross-tab synchronization. Host sessions remain authoritative. | Storage unit cases test native reads/fallback; navigation tests exercise folded views. Cross-tab live folding is not a supported guarantee established here. |
| `features/terminal` | `terminal.tsx`: view creation/cleanup, socket connect/close and retry | View owns xterm, listeners and observers; socket effect owns retry timer. Ended/replaced stop reconnection. Independent open requests must not share an exclusive action gate. | Terminal host/browser tests; shell/platform semantics and every late socket callback ordering remain outside exhaustive proof. |
| Entry/service worker | `sw.ts`: routing, renderer cache, activation, notification admission; shared PWA watcher | Cache admits build assets rather than API traffic or mutations. Worker update and page lifetime are distinct; notification delivery does not execute commands. | Build/typecheck and reload/browser tests. Physical push delivery, background OS suspension and cache eviction are not established by hermetic tests. |

## Test quality and validation

Reviewed tests were judged by observable outcomes, independent owners, failure
recovery and controlled ordering. The new browser probes mount the real hooks
in Preact and delay network answers; they do not copy the implementation. The
stylesheet test delays the body as well as the response and restores globals.
The native batcher test waits on a promise with a deadline rather than assuming
that sleeping long enough proves delivery. Import/source contracts are useful
architecture guards, but are explicitly not behavioral proof.

Validation for the follow-up changes is recorded in the PR description with
its commit and CI run. Earlier results must not be presented as checks of a
later commit. Full local suites, hosted sanitizer/static-analysis jobs, live
provider compatibility, manual device checks, merge and deployment are distinct
claims. No merge or deployment is part of this review.

## Remaining proof boundaries

The table is the coverage ledger: unlisted internals received inventory-level
coverage, not detailed semantic review. This pass does not certify the whole
codebase as flawless, security-audited or free of races. In particular, full
memory/compaction algorithms, every persistence crash point, all schedule time
transitions and all UI async permutations would require separate, deeper
reviews. No code was deleted merely to hit a line-count target; ownership and
behavior take precedence over net deletion.

## Documentation follow-up

Reviewed all 31 tracked Markdown files against current repository behavior,
including root guides, deployment and benchmark notes, bundled skill references,
and test documentation. Historical changelog entries and dated benchmark results
remain historical records; external service behavior was not remeasured.

Corrected browser field visibility in security text and its diagram, the renamed
configuration tool, terminal-output privacy limits, approval-prefix semantics,
per-turn tool availability, headless resume instructions, plain-mode motion,
stylesheet storage, web build order and CI selection. Volatile bundle sizes and
browser-spec lists now point to their measurement or configuration source.

Validation: 144 local Markdown links and heading targets resolved; referenced
source paths and configuration-variable names were checked against the tree.
The updated SVG was rendered and inspected. Repository lint/source-contract
checks and `git diff --check` passed. This follow-up changes documentation only;
the production/test validation above remains tied to its recorded source commit.
