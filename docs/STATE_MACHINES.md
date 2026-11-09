# State ownership

This map covers first-party C++ and TypeScript state-machine families. It
includes explicit enums, implicit protocols expressed by flags and callbacks,
and streaming parsers. Static configuration flags and ordinary editable values
are not state machines by themselves. Multiple instances of a component share
the same machine; they do not share its mutable state.

The C++ runtime owns execution, permissions, accounting and durable changes.
Clients submit commands and project its events. Local UI state owns navigation,
editing and interaction. See [Architecture](ARCHITECTURE.md),
[Persistence](PERSISTENCE.md) and [Web](WEB.md) for the surrounding contracts.

## Review decisions

| Area | Decision | Why |
| --- | --- | --- |
| Files navigation | One `FileLocation` record; named open, browse, changes and reveal transitions | Related fields change together. Reveal replaces prior modes; closing a file preserves its folder view. |
| UI mutations | `useAction` admits one operation synchronously by default; history reads opt into concurrency | Disabling a rendered button alone cannot prevent two calls in the same event turn. Library saves and pairing use this owner. |
| Outgoing receipts | Pure `outgoingAfter` transition, separate from transcript confirmation | An accepted queued message remains recallable until the runtime consumes it. Unknown receipts must not be presented as success. |
| Reconnection | Share refresh completion; retain separate resume and catalogue error boundaries | Transport-open is not caught-up. Replacing this with a single connected boolean would weaken readiness. |
| Wheel zoom | One binding in `shared/zoom.ts`, with surface-owned pan policy | Modifier-wheel handling is shared; page scrolling, bounded images and an unbounded canvas differ. Page listeners read current scale. |
| Session mutation exclusions | `HostSession::LifecycleBusy` | Catalogue refresh, move, reconnect discovery and routing agree about closing/updating/deleting. Connecting remains a separate caller decision. |
| WebSocket admission | `Master::WebSocketDevice` | Browser and terminal sockets share origin/device checks and lock ownership. Their capacity and relay lifetimes remain separate. |
| Independent terminal opens | Retain their owner | Folder requests can arrive while another shell is opening. A single-action guard would discard legitimate work. |
| Configuration saves | Retain request ordering and an in-flight count | Saves for independent keys may overlap. This is not the single-action protocol. |
| Generic resource loading | Retain `useResource` and caller identity checks | Keeping the previous value is useful during refresh; resource identity must still be checked when switching files. |
| Activity, turn and MCP lifecycles | Retain their existing enums and owners | These are already explicit. Their I/O, cancellation and delivery dimensions cannot safely be flattened into one enum. |
| Transcript scrolling and gesture input | Retain independent interaction dimensions | Following, restoring, touching and gliding overlap. A large Cartesian-product enum would add states and obscure browser-specific invariants. |

The review used a repository-wide source inventory, state-field and transition
searches, duplicate-block and symbol-reference scans, then caller and test
review for the changed boundaries. Scans are candidate discovery, not a proof
that every path is exercised. No unused exported TypeScript symbol was
identified by the reference scan. Similar-looking provider adapters, parsers
and cancellation loops were not treated as equivalent without evidence.

## Runtime and protocol machines

“Explicit” means a state/flow type already names the lifecycle. “Implicit” means
flags, resource ownership, queued records or ordered callbacks encode it. A
mixed machine has both.

| Machine | Owner | Form and important transitions/invariants |
| --- | --- | --- |
| Agent turn and step | `src/agent/turn.cc`, `turn_internal.h`, `turn_response.cc` | Mixed: prepare → model → tools/recovery → next step or finish; outcome and stop reason are distinct. |
| Retry and context recovery | `src/agent/request.cc`, `compaction.cc`, `include/api/retry.h` | Mixed: classify failure → retry/degrade/compact/stop; semantic progress and cancellation constrain replay. |
| Turn budgets and repeated-call recovery | `src/agent/turn_budgets.cc` | Implicit counters/deadlines plus typed faults; pre-call token bounds differ from post-call limits. |
| Transcript projection | `src/agent/view_reducer.cc`, `transcript_tree.cc` | Implicit event reduction: running nodes become terminal; parent/order identity survives checkpoints and history loading. |
| Activity process lifecycle | `include/agent/activity_supervisor.h`, `src/agent/activity_supervisor.cc`, `activity.cc` | Mixed: starting/running/stopping/draining/delivery; process exit, output EOF, background request and delivered result are separate facts. |
| Activity wake and delivery | `src/agent/activity_drain.cc`, `mail_delivery.cc`, `include/core/steering.h` | Implicit queues: completion or mail arrival wakes work; delivery must not be confused with process exit. |
| Guidance and interruption | `src/core/steering.cc`, `src/app/session_worker.cc` | Implicit: queued → recalled/inserted/consumed; after-turn messages wait through the current turn. Foreground abort does not cancel every background job. |
| Parent/child communication | `src/app/agent_link.cc`, `src/agent/child_agent.cc`, `src/core/mailbox.cc` | Implicit pending/connecting/acknowledged delivery and child lifetime; preserve ownership and duplicate handling. |
| Session worker lifecycle | `include/app/session_host.h`, `src/app/session_supervisor.cc`, `session_router.cc` | Mixed: saved/draft → starting → live/failed/interrupted → closing; worker generation rejects stale frames. |
| Session move/delete/update | `src/app/session_move.cc`, `session_router.cc`, `session_host.cc` | Mixed: validate → reserve lifecycle → perform operation → publish or restore status; callers retain lock and rollback responsibilities. |
| Command receipt | `src/app/session_command.cc`, `outcome_store.cc`, `src/web/master_command.cc` | Mixed: new/pending/completed/evicted; identical retry, conflicting reuse and unknown receipt are different outcomes. |
| Snapshot/replay subscription | `src/app/replay_log.cc`, `session_snapshot.cc`, `src/web/master_stream.cc` | Implicit epoch/cursor/generation protocol: snapshot → replay → ready, or resync on a gap. |
| Scheduled task/run | `src/app/schedule.cc`, `session_schedules.cc` | Implicit persisted statuses: due/queued/active/terminal, pause/resume and recovery; schedule revision and run state are independent. |
| Approval and question | `src/app/approvals.cc`, `permissions.cc`, `session_worker.cc` | Mixed: proposal/request → waiting → answer/refusal/cancellation; answer identity and policy remain runtime-owned. |
| Configuration proposal | `src/app/config_proposal.cc`, `config_control.cc`, `src/core/effective_config.cc` | Implicit prepare/validate/commit/expire/conflict, followed by reload or restart effects; preserve direct-user versus agent policy. |
| Library/asset mutation | `src/app/library.cc`, `asset_store.cc`, `session_assets.cc`, `import.cc` | Implicit revision and ownership protocol: stage/read → validate revision → write/import/claim or conflict; attachment claiming is distinct from uploading. |
| Conversation persistence | `src/agent/session_store.cc`, `conversation.cc`, `src/core/private_store.cc` | Implicit read/validate/replace and archive/checkpoint lifecycles; partial state cannot replace valid live state. |
| Browser broker lease | `src/browser/broker.cc` | Implicit live/busy/release-pending slots and waiters; owner heartbeat and operation lifetime constrain release. |
| Browser runtime and handover | `include/browser/runtime.h`, `src/browser/runtime*.cc` | Implicit start/attach/load/control/handover/stop, profile setup and selected page; preserve lease and process ownership. |
| Browser action waiting | `src/tools/browser.cc`, `src/browser/runtime_act.cc` | Implicit observation/action/wait/reobserve, cancellation and deadlines; a successful transport request is not successful page action. |
| MCP startup and discovery | `include/mcp/server.h`, `src/mcp/register.cc`, `discover.cc`, `server.cc` | Explicit startup plus implicit tool discovery/subscription/cache refresh; partial discovery is not readiness. |
| MCP request transport | `include/mcp/rpc.h`, `src/mcp/rpc.cc` | Explicit pending/ready/closed plus partial writes and line buffering; incomplete writes require connection shutdown. |
| Provider stream decoding | `src/api/wire_stream.cc`, `include/api/stream.h`, `wire.h`, `tool_protocol.h` | Mixed per-provider event/block/tool-call assembly; chunk boundaries cannot change the result. Hosted-tool phase differs from model text. |
| HTTP attempt and retry | `src/api/client.cc`, `exchange.cc`, `src/providers/policy.cc` | Implicit attempt/header/body/completion/cancel and retry policy; preserve remote-error classification and accounting. |
| SSE and session framing | `src/transport/sse.cc`, `session.cc` | Implicit byte parsers: partial line/frame → complete message or bounded failure; CR/LF and chunk splits matter. |
| Remote framebuffer filter | `src/web/rfb_filter.cc`, `web/src/features/browser/rfb.ts` | Implicit protocol handshake/message framing and input policy; retain partial-frame buffering. |
| Terminal host/viewer | `src/web/terminal.cc`, `master_terminal.cc`, `src/app/session_terminal*.cc` | Mixed process/socket/replay/input modes: attach/replaced/exit; output replay and process lifetime differ. |
| CLI input and composer | `src/ui/input_decoder.cc`, `interactive.cc`, `composer.cc` | Implicit text/escape/sequence/paste/editor/history/search; paste completion, overflow and escape timeout are separate. |
| Streaming terminal presentation | `src/ui/markdown*.cc`, `presentation.cc`, `live_region.cc`, `src/core/turn_status.cc` | Implicit fence/table/math/text and committed/live output; parser carry and redraw state must survive chunks. |
| Shutdown and resource ownership | `src/app/runtime.cc`, `src/core/events.cc`, `debug.cc`, `signals.cc`, `include/core/fd.h`, `lease.h` | Implicit owned/released, running/stopping/drained; destructors, wake descriptors and joins provide the transition boundary. |

## Browser and local interaction machines

| Machine | Owner | Important transitions/invariants |
| --- | --- | --- |
| Host authentication and connectivity | `web/src/state/use-host.ts` | Unknown/authenticated/revoked, offline/open/catching-up/ready, retry/rescan/resume; mutation readiness waits for the ready watermark. |
| Event and snapshot projection | `web/src/state/apply-event.ts`, `store.ts`, `snapshot-store.ts` | Apply ordered events, merge history and replace snapshots; retired streams and stale snapshots cannot win. |
| Outgoing commands and receipts | `web/src/app/use-commands.ts`, `web/src/state/api.ts`, `store.ts` | Waiting/sending/queued/awaiting/sent/refused/unconfirmed; receipt timeout is not rejection or permission to replay. |
| Transcript paging and scrolling | `web/src/state/use-transcript-history.ts`, `node-reader.ts`, `transcript-bookmarks.ts` | Follow/anchor/restore/glide, pending history and search jumps; touch/wheel interruption and session identity control restoration. |
| Shared async resources | `web/src/shared/use-resource.ts` | Load/reload/settled/error/aborted; retain prior value while replacing a request, discard superseded answers. |
| Shared async actions | `web/src/shared/use-action.ts` | Idle → running → success/error; overlap returns false by default; concurrent reads count all pending work, and independent hook instances remain independent. |
| Files navigation and citation search | `web/src/features/files/files.tsx` | Folder/file/source/diff/canvas and narrow-pane selection; reveal resets modes, missing citations search, Back preserves the underlying folder view. |
| Preview and design messaging | `web/src/shared/page-frame.tsx`, `web/src/features/files/page.tsx`, `canvas.tsx` | Load/identify/resize/error/reload, select/hover/pick/comment, tweak/theme and fullscreen; messages belong to the current frame/source. |
| Pan, pinch and wheel | `web/src/shared/zoom.ts`, `zoom-surface.tsx`, `web/src/features/browser/gestures.ts`, `touch.tsx` | Pointer admission → drag/pinch → release/cancel; retain surface-specific bounds, tap suppression and Safari handling. |
| Drawing and annotation | `web/src/shared/annotate.tsx` | Pen/pin, press/draw/edit/save, independent zoom; a two-pointer pinch must not become a drawing stroke. |
| Library document editor | `web/src/features/library/document.tsx` | Load/draft/edit/save/conflict, rename/move/delete dialogs; successful save clears draft, revision conflicts preserve edits. |
| Library selection and import | `web/src/features/library/library.tsx`, `import.tsx`, `artifact.tsx` | Filter/select/create/import and refresh; selected item identity and per-item import progress remain local. |
| Settings and provider editing | `web/src/features/settings/config/use-configuration.ts`, `providers-field.tsx` | Load/edit/save/reject, ordered concurrent answers and accumulated restart effects; hidden keys survive rename. |
| Pairing and device management | `web/src/features/pairing/pairing.tsx`, `settings/panes/devices.tsx` | Code entry/submission/expiry/authenticated, revoke/logout confirmation; failed pairing leaves the shell retryable. |
| Browser controls | `web/src/features/browser/browser.tsx` | Poll/status refresh, takeover/done/stop/profile actions and viewer attachment; control ownership and rendering connectivity differ. |
| Terminal panel | `web/src/features/terminal/terminal.tsx` | Connect/reconnect/replaced/exit and tab open/select/close; independent folder-open requests must not be dropped. |
| Composer and attachments | `web/src/features/composer/composer.tsx`, `web/src/app/use-draft-uploads.ts` | Draft/edit/upload/submit, composition, command/mention completion and queued guidance; unsent work survives selection changes. |
| Approval/question UI | `web/src/shared/decision.tsx`, `ask.tsx` | Edit/answer/send, question steps/review/image upload; only runtime acknowledgement settles the decision. |
| Navigation and layer dismissal | `web/src/shared/dismiss.ts`, `navigation.ts`, `web/src/app/app.tsx` | Push/pop/close/deferred navigation; one Back reaches a visible layer without overwriting session history. |
| Motion and sheet gestures | `web/src/shared/motion.ts`, `swipe.ts`, `sheet.tsx` | Enter/visible/exit, swipe admission/drag/release/cancel; timings remain in the shared motion system. |
| Search and inspection | `web/src/features/search/spotlight.tsx`, `chat/find-bar.tsx`, `chat/inspector.tsx` | Query/load/result/select/close, older-page requests and row disclosure; selection identity controls stale results. |
| Local persistence | `web/src/shared/storage.ts`, `device-prefs.ts`, `web/src/state/use-host.ts` | Native storage → in-memory fallback, debounce/flush, persisted drafts/catalogue; denied storage must not break the app. |
| Install/update/notifications | `web/src/app/use-boot.ts`, `web/src/shared/pwa.ts`, `web/src/state/push.ts`, `web/src/sw.ts` | Worker install/wait/activate, install prompt and push permission/subscription; pending work blocks unsafe reload. |

## Validation and future edits

For changes to these owners, test transitions and observable invariants rather
than matching the implementation's private flags. Use the existing native
unit/integration suites and browser scenarios listed in [Testing](TESTING.md).
The cleanup adds action-admission, receipt-precedence, wheel-binding and
successive zoom-anchor regressions; Files checks preserve unpressed ARIA state.

Do not replace independent dimensions with a universal state-machine framework.
An enum is useful when alternatives are mutually exclusive. A queue, scoped
resource or record is often clearer when work can overlap. Share the decision
that callers actually have in common, including its cancellation and error
contract, rather than merely sharing similar syntax.
