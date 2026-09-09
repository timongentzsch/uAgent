# Native web interface

Run `uagent --web` and open the printed URL. Enter the single-use pairing code in
the browser. The master remains in that terminal; another `uagent --web` invocation
reuses the same master and prints a fresh code. Launching from another directory
never starts another server or changes a worker's directory.

The sidebar lists the current OS user's saved sessions across directories. Select
one to read its history without starting an agent. Resume it explicitly, or create
a conversation using an accessible absolute directory on the host. Four workers
can run simultaneously. Close an idle or interrupted conversation to release its worker slot.
Closing the browser leaves workers running; stopping the master cancels and reaps
them. Restarting never automatically resubmits prompts, approvals or tools.

The interface uses black, white and neutral grays in both appearance modes. The
composer shows the active `provider/model:variant:effort` route, omitting unset
suffixes exactly as the CLI does. Select it to choose from the native provider
catalog without submitting or clearing the draft. The selector groups the full
catalog by provider, with effort and variant in the same popup. `/variant` and `/effort`
use the existing route rules and update the same label.

## Execution and persistence

The master serves embedded files, maintains bounded metadata/event caches, and
spawns persistent workers of the same executable over versioned JSON stdio. It
never bootstraps an agent or imports project configuration. Each worker runs the
existing `ApplicationChannel`, agent, providers, tools, project-trust checks and
process supervisor. Interrupts, steering and correlated human replies can arrive
while a turn runs. Normal prompts stay serialized. Structured slash-command
results open a shared full-content viewer. Model selection, context inspection,
configuration, permissions, forks and activity controls are native commands; they do not create chat turns.
Only a real turn changes Send to Send guidance. Clicking the context counter
opens Raw context; `/context` prints the prepared model context in the CLI.

A submitted message appears immediately with its timestamp and delivery state.
The composer clears that submitted draft while allowing the next draft to be
edited. A stable request ID reconciles the optimistic row with `message.changed`
and the native receipt, including acknowledgements that arrive over SSE before
HTTP returns. Unconfirmed rows survive a tab reload in session storage; reconnect
queries receipts and saved history without resubmitting. Explicit rejection
restores the draft. Native user messages are saved when added to the conversation,
before the response request, rather than waiting for the answer.

Completed assistant/activity messages increment a persisted incoming counter.
Each browser stores its last-read counter per conversation. A sidebar dot marks
new messages while another conversation is selected, the view is hidden, or the
reader has scrolled away from the latest message. Reconnect compares counters;
model/settings changes alone do not create unread messages. The sidebar shows
last-updated dates instead of an idle status label. A separate green dot identifies
a live terminal or web session, including an idle one; its label distinguishes the
two. Offline browsers hide the presence dot until they reconnect. Row hover and
keyboard focus include the conversation menu.

One shared OS-lock helper owns the master and each session writer through
separate companion files. Lock inodes are never replaced or unlinked. Independent
CLI/web conversations can run in the same canonical directory; each retains its
own process, approvals, history and interrupts. A saved conversation still has
only one writer. Sessions share project files, so simultaneous edits need the
same coordination as any other concurrent work in a directory.

Writer leases optionally publish a PID and process start identity under the lock,
then clear that advisory record before releasing ownership. Presence never probes
by acquiring a competing lock. The host checks identities once per second while
SSE clients are connected, sharing its shutdown thread; directory stamps avoid
rereading history headers on unchanged ticks. Changes use the existing metadata
SSE events, without browser polling or a second supervisor. An updated CLI is
required to publish terminal presence. Process exit or a mismatched start identity
invalidates a stale record; the descriptor lock remains the writer authority.

Format-3 session files remain the conversation authority. Read-only inspection is
separate from workspace-checked resume. Saves happen after completed/interrupted
turns and background updates, without waiting for another prompt. Optional bounded
display metadata retains actual readable reasoning, tool status/timing, receipts,
stable message IDs, timestamps, the route actually used, per-response usage and
attachment references and durable request/reply links. This metadata never enters provider messages.
Archived retained content is projected separately from pruned model context.
Missing legacy facts are labelled as unavailable, rather than inferred. Browser
history does not expose system/memory/internal message bodies. Captured output is
read by recorded call ID in bounded pages from the private artifact directory.
“Tool input/output” opens the complete logical tool call and returned ToolResult.
“Show full message” expands truncated message text. Expanding folded tool details
loads truncated tool output in place, with retry and reuse across toggles.
Tool limits still apply; a referenced full process log is a separate artifact.

Assistant menus offer **HTTP request/response**: the exact serialized provider
request, including system context, tools and parameters, and the response payload
before SSE/JSON parsing. Each retry retains its own status, timestamp and headers.
Bodies are streamed into private files; only bounded metadata enters app events.
Headers containing credentials and URL query strings are redacted. Body contents
remain verbatim and can contain sensitive conversation material. The viewer loads
16 KiB pages on demand and offers original-body copy/download. **Readable**
indents JSON and decodes text values: newlines become line breaks, quote escapes
become quotes, and nested JSON strings such as tool arguments are expanded up to
four levels. This is a readable text projection, not a JSON serialization.
Numbers and duplicate keys retain their lexical form; real backslashes in paths
and regexes remain, and nonprinting control characters stay visibly escaped.
For streamed responses, **Events** applies the same readable formatting inside
numbered frames without repeating `data:` on each line. **Source** shows the
unchanged captured body for every body type. Copy/download always use the
original body, independently of the selected display mode. Event names, IDs, retry fields,
comments, unknown fields, non-JSON data and unfinished final frames remain visible.
The projection follows [WHATWG SSE framing](https://html.spec.whatwg.org/multipage/server-sent-events.html#parsing-an-event-stream),
including CR/LF variants and multiline data, while retaining incomplete captures
that a live EventSource would discard at EOF. Response capture is capped at 64 MiB, headers at 16 KiB; interrupted,
truncated and expired captures are labelled. Existing background-artifact retention
settings apply. HTTP capture covers the agent's model loop, including compaction,
not arbitrary HTTP requests made by tools.

The context counter matches the CLI, for example `ctx 4.6k/1.3M · 99% left`.
It uses the native model window and rounds remaining percentages down; an unknown
window shows only usage, and usage beyond the limit shows 0% left. Hover shows
exact counts. The counter opens the latest actual model request; before a first request,
it prepares an explicitly unsent preview without invoking the provider. Historical
assistant messages open their own captured exchange. Reading never modifies model
context. `/http [INDEX [request|response]]` reads the same latest model-call attempts
from the CLI, defaulting to the last request. `/context` continues to print the
prepared current context.

The capture boundary follows libcurl's documented
[write callback](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html) for decoded
response payload and [debug callback](https://curl.se/libcurl/c/CURLOPT_DEBUGFUNCTION.html)
for incoming/outgoing HTTP headers. It preserves original SSE framing at the
application payload level; it is not a TLS or compressed packet capture.

## Conversation controls and statistics

The geometric mark is shared by the interface and generated PWA icons. The
composer toolbar sits below the text input: attach, canonical
`provider/model:variant:effort`, and send. Model, effort and variant controls use
the native catalog and validators. Appearance defaults to System and follows OS
changes live; explicit Light/Dark preferences persist. Display scale and independent
message/input text scale are global browser preferences. Native sliders support
50–200% display scale and 50–300% text scale in 1% steps. The controls use the
browser's [range input](https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/input/range)
for keyboard and touch operation. User requests keep their inverted colors and fill the transcript width.
All message content is left aligned. Enter sends the message; Shift+Enter inserts
a newline. Composing input and held-key repeats never trigger a send. Request/response
links remain in native history metadata without a repeated label in the chat. Tool calls
and outputs share a collapsible row joined by call ID within the request;
parallel, incomplete and unmatched results remain visible. Model-call metadata
and HTTP captures remain available in the joined row’s menu. Tool details start
folded; opening a truncated result loads its complete retained output through the
existing paged reader, with local loading/retry and a cache across fold toggles.
There is no separate “Show full tool output” control outside the folded details.

Conversation menus in the header and sidebar offer Fork, Rename, Statistics, Close and
Delete. Names are applied at serialized command boundaries. A custom name survives
empty-draft activation, worker shutdown and host restart. Stop and close a worker before deleting its conversation. Deletion takes
the session writer lease and removes only its snapshot, draft marker, event
sidecar, owned attachment directory and referenced tool/HTTP exchange files; it does not touch project files.

Fork creates an independent snapshot of the completed conversation, including its
recorded model selection, permission override, active/archived context and owned assets.
Older snapshots without a recorded selection use the current model configuration.
The source stays unchanged; no process, pending approval or remembered grant is
cloned. Fork lineage is shown in conversation statistics, and new usage is counted
separately. `/fork [NAME]` uses the same snapshot operation and continues in the
fork when the terminal has no owned background activity. Otherwise it saves the
fork and leaves that terminal with its jobs; `/sessions` can open the fork later.

The permission selector offers Use default, Ask and YOLO. Session changes apply
at the next approval boundary; an already pending approval still needs its own
answer. Mandatory-human checks remain mandatory. `/permissions [default|ask|yolo]`
uses the same atomic policy. Settings exposes the user default plus an advanced
editor generated from the native configuration registry: types, bounds, defaults,
configured values, available active values, source and reload timing. Secrets are
masked and can be replaced by direct human input. User/project writes use the
existing validated atomic configuration proposal path. Trusted-project scope is
available for an idle active conversation. Runtime-backed options reload at the
next user turn; restart-only and shadowed changes say so. `/config` lists the same
schema; `/config user KEY=VALUE` and `/config project unset KEY` edit it.

Each message has a local timestamp (full date on hover), quick copy and a
statistics menu. Expanded thinking uses the theme's muted grey, including its
Markdown and code, in both light and dark appearances. Assistant facts preserve the actual route, reported token/cache
usage, request duration, TTFT and request-average throughput. Tool results show
the tool, call ID, status and duration. Missing legacy facts stay unrecorded.

Conversation counters are accumulated at native request/turn boundaries and
saved with the existing format-3 display metadata. They do not depend on the
visible history window or the bounded event journal, and survive compaction.
Model calls include internal compaction requests and unsuccessful requests;
completed-turn tool counts match the CLI, and tool time sums result durations
(which can overlap). A checkpoint records totals to that point; a killed process
can lose activity since the last save. Legacy sessions label counters as partial.
Provider usage coverage can also be incomplete when requests supply no usage.

TTFT is the time from the transport request loop starting to first nonempty text
or reasoning, including retries and backoff. It excludes local request encoding.
Hosted-tool activity keeps semantic-progress timeouts alive without counting as
a token. Throughput is reported output plus reasoning tokens divided by total
request-loop time, including TTFT, network delay and retries; it is not a pure
decode rate. Means use only recorded samples, weighted by matching request time
for throughput. Timestamps and statistics never enter model context.

The separation of full-conversation statistics from the visible/compacted
transcript follows the [DeepSeek Harness session-statistics design](https://github.com/deepseek-ai/deepseek-harness/blob/c389f96bf3a9b6807cb71ed6bdad5849be0df6d8/packages/session/session-stats/README.md).
This implementation uses incremental checkpoint counters instead of introducing
another authoritative event log. Its TTFT and conservative request-average rate
are documented above so they are not confused with decode-only measurements.

## Background work and CLI parity

A compact Activity row above the composer shows the real foreground phase and
running agent/command counts. The sidebar uses the same native phase and counts.
Expanding shows task labels, progress, elapsed time and stop controls. Completed
and idle rows are collapsed together; failures remain visible. Details show a
non-consuming output snapshot, or the retained child conversation and counters.
A running child accepts guidance; an idle child accepts an explicit follow-up.
Background completions add one linked history entry without adding extra model
messages or repeatedly appending progress logs.

`ProcessSupervisor` owns processes, I/O and completion delivery. Its existing
wake pipe publishes `activities.changed` when the bounded projection changes.
No second supervisor or status-polling loop was added. A timer paints elapsed
time only while activity details are open; output refreshes are driven by events.
Human inspection uses the transcript snapshot, never the consuming activity-tool
output queue or its interaction lock. Completion remains deliverable exactly
once to its existing owner. Collaborators record their owning conversation,
so two sessions in the same folder cannot inspect or control each other's children.

The CLI uses the same controls and projection:

- `/ps` lists background work; `/ps ID output` inspects and `/ps ID stop` stops one.
- `/agents` includes retained collaborators; `/agents ID output`, `stop`,
  `message TEXT` and `followup TEXT` act on that child.
- `/trace CALL_ID` pages the same full tool exchange. Plain `/trace` keeps the
  existing latest trace view.

Listing, inspection, guidance and stop remain available while the foreground
turn runs. Starting a follow-up requires an idle foreground, preserving the API
client's single-owner contract. No model request is made just to inspect work.

The presentation follows the expandable subagent entries documented by
[VS Code](https://code.visualstudio.com/docs/agents/run/subagents) and the
attention/running/completed grouping in Claude Code's research-preview
[agent view](https://code.claude.com/docs/en/agent-view). These are product
precedents, not experimental evidence that one layout is optimal. The
[Magentic-UI study, sections 7.4 and 8.2](https://arxiv.org/html/2507.22358v1)
reports a 12-person qualitative study in which long histories and frequent
interruptions could overwhelm users. Our compact row, optional details and
completion-only history entries are design inferences from those findings.
Status changes use polite announcements without moving keyboard focus, following
[WCAG status-message guidance](https://www.w3.org/WAI/WCAG21/Understanding/status-messages.html).

## Library and scheduled tasks

The sidebar includes Library and Scheduled. Both use the shared native
management commands; see [management controls](MANAGEMENT.md) for scope,
revision checks, CLI examples, timing, permissions and recovery behavior.

## Attachments

Paste, drop or select any regular file, within the existing 8 MiB/file, eight-file
submission, session and global storage limits. Files are persisted privately before
an opaque asset ID is returned. A prompt submits its text and ordered IDs together;
IDs from another conversation do not resolve. The original filename and MIME
metadata survive restart. Raster signatures permit inline previews for PNG, JPEG,
GIF and WebP; all other bytes, including HTML/SVG, download as octet-stream.
The worker shares CLI attachment inspection and provider encoding. Supported
images/documents are sent through native provider capabilities; unknown binary
files remain available by host path to tools. File-only prompts work. Asset
reads require the paired cookie and are never cached by the service worker.

Uploads not claimed by a prompt expire after 24 hours, checked at startup and on
subsequent uploads. Claimed originals survive restart. A crash during submission
can retain an unused claimed image; storage remains bounded. Existing format-3
model content can also contain inline image data, so disk usage includes both
original source files and the normal session snapshot. These limits are separate.
Historical CLI images without web asset references have an explicit unavailable-
browser-preview label. Processed images are pruned from model context by the
existing agent policy; keeping browser source files does not resend them on
every turn. Explicitly reattaching a retained asset sends its original bytes.

The design was compared with public Codex source at
[`d6489472`](https://github.com/openai/codex/tree/d6489472f3c15e87d2d7763a5fde033545c530f8):

- [`UserInput`](https://github.com/openai/codex/blob/d6489472f3c15e87d2d7763a5fde033545c530f8/codex-rs/protocol/src/user_input.rs)
  separates typed text and local/pre-encoded images from UI text markers.
- [`AttachmentStore`](https://github.com/openai/codex/blob/d6489472f3c15e87d2d7763a5fde033545c530f8/codex-rs/attachment-store/src/lib.rs)
  requires persistence before returning a durable reference and redacts attachment
  URLs in debug output. Its inline implementation returns data URLs.
- [`image_preparation`](https://github.com/openai/codex/blob/d6489472f3c15e87d2d7763a5fde033545c530f8/codex-rs/core/src/image_preparation.rs)
  rejects remote HTTP image inputs, prepares model-specific image representations
  and emits explicit placeholders for processing failures.
- [`image_rollout` tests](https://github.com/openai/codex/blob/d6489472f3c15e87d2d7763a5fde033545c530f8/codex-rs/core/tests/suite/image_rollout.rs)
  check submission/retention/resume behavior. Our regression checks original bytes
  across upload, restart, explicit resubmission and the mock-provider request.

We adopt these boundaries with the existing native attachment pipeline and a
local asset directory. No Codex storage framework or private desktop-app behavior
is assumed; no upstream source was copied.

## Phone access and installation

The native listener binds only `127.0.0.1`. For a phone, configure a stable trusted
HTTPS origin and a local reverse proxy that preserves that Host header:

```sh
uagent --web --web-origin https://your-host.example
```

`UAGENT_WEB_PORT` / `--web-port` select the loopback port (default 8080).
`UAGENT_WEB_ORIGIN` / `--web-origin` name the exact external HTTPS origin, without
a path or trailing slash. These settings belong in user configuration, not a
project. An already-running master's settings take precedence.

[Tailscale Serve](https://tailscale.com/docs/features/tailscale-serve) is one
private HTTPS proxy option. Obtain its stable tailnet HTTPS hostname and use that
exact origin; route it to the native loopback port. The phone must be able to
reach that origin when opening the app. No automatic proxy or firewall changes
are made by µAgent. Forwarded headers are not trusted to infer the origin.

Install through the browser's install control. On iOS, use Safari → Share → Add
to Home Screen, then open the installed app and pair it separately if required.
A public manifest, local icons and a custom service worker provide the shell.
Only explicit build-manifest assets enter the precache. APIs, SSE, uploaded
images, credentials and drafts do not. Offline controls are disabled; reconnect
and visibility changes reload authenticated state before enabling commands.
Drafts remain in the current browser view across reconnects/session switching;
closing or manually reloading that view discards them. Updates are offered
explicitly and cannot force-reload a view with drafts or a pending decision.

## Pairing and notifications

The private `~/.uagent/web` directory contains the master lock, authenticated
atomic discovery record, draft identities and device credentials. Pairing codes
have 96 bits of OS randomness, expire after five minutes, are single-use, and are
rate-limited. Devices receive independently revocable, 30-day HttpOnly/SameSite
cookies; HTTPS cookies also use Secure. Host/Origin checks protect commands,
uploads and pairing. Device access is to this OS user's whole catalogue; this is
not a multi-user hosting service. Do not expose the loopback listener directly.

Notifications are opt-in per paired device. A no-push build offers connected-view
notifications and clearly describes their limits. It cannot deliver after the
browser view is suspended/disconnected. In-app attention badges work without
notification permission.

For native background Web Push, build with `-DUAGENT_WEB_PUSH=ON` and set a valid
`UAGENT_WEB_PUSH_CONTACT` (`mailto:you@example.com` or an HTTPS contact URL) in user
configuration. This adds OpenSSL 3 libcrypto; it is OFF by default. On macOS, a
push-enabled binary needs its matching libcrypto installation at runtime; normal
no-push releases do not gain that dependency. Enable notifications from Settings
in the supported browser/installed app. Settings also offers a test, mute, device
revocation and logout. Background push and connected-view notifications are not
both sent for a subscribed device.

The sender uses OpenSSL EVP for P-256 ECDH/ES256, HKDF-SHA256 and AES-128-GCM and
libcurl for bounded HTTPS requests. Payloads contain only an event ID and session
ID; the notification text is generic. Delivery cannot approve or execute
anything. Clicking opens fresh authenticated state. Supported endpoints are
Google FCM, Mozilla and Apple push origins; arbitrary URLs, credentials, unsafe
ports, redirects and private/reserved connection addresses are rejected. DNS
resolution alone is insufficient: each actual connection address is checked.
Browser push services require outbound internet even for a private app origin.

Protocol references are [RFC 8291](https://datatracker.ietf.org/doc/html/rfc8291),
[RFC 8292](https://datatracker.ietf.org/doc/html/rfc8292),
[OpenSSL EVP](https://docs.openssl.org/3.5/man7/EVP_PKEY-EC/) and
[libcurl's connection callback](https://curl.se/libcurl/c/CURLOPT_OPENSOCKETFUNCTION.html).
[WebKit's Home Screen push requirements](https://webkit.org/blog/13878/web-push-for-web-apps-on-ios-and-ipados/)
explain the iOS install/permission flow. These sources support the implementation
choices, not a claim of guaranteed notification delivery.

## Bounds and development

| Resource | Bound |
| --- | --- |
| Persistent agent workers | 4 |
| HTTP threads / pending requests / SSE views | 12 / 24 / 4 |
| IPC frame / command / individual live event | 1 MiB / 64 KiB / 64 KiB |
| Worker output queue / master event replay | 4 MiB each |
| Master replay / per-worker live events | 2,048 / 512 (also byte bounded) |
| Active history page / browser retained DOM messages | 64 / 256 plus bounded live preview |
| Projected history page / raw body page | 384 KiB / 16 KiB |
| Session file / catalogue header / catalogue entries | 64 MiB / 16 KiB / 4,096 |
| Display facts | 4 MiB, 4,096 records, 64 KiB per record |
| Image / images per prompt | 8 MiB / 8 |
| Session source assets / global source assets | 64 MiB (64 files) / 512 MiB |
| Paired devices / subscriptions | 16 each |
| Push queue / TTL / attempts / request timeout | 64 / 60 s / 2 / 5 s |
| Command deduplication / pending worker commands | 256 recent device/request IDs / 32 per worker |
| Browser transcript cache | Selected session plus at most 4 active workers |

The public protocol has epoch/sequence cursors, session IDs and worker
generations. Snapshots and replay cursors are captured under the same master
state lock. Expired replay, buffer gaps and protocol mismatch require a fresh
snapshot. Replay contains observations, never executable commands.

Normal native builds embed checked-in `web/dist` and need no npm, Node or Python
application runtime. CLI-only builds use `-DUAGENT_WEB=OFF`. To change the UI:

```sh
npm ci --prefix web
npm run test --prefix web
npm run format:check --prefix web
npm run build --prefix web
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release
npm exec --prefix web -- playwright install chromium webkit
npm run test:browser --prefix web
```

The frontend and service worker use strict TypeScript and Preact. Native builds
embed the checked-in output without Node or Python runtime dependencies.
`use-host.ts` owns SSE, snapshots, receipts and read markers; `app.tsx` composes
feature modules. Shared `types.ts` describes the protocol boundary. Optional
views, Markdown, KaTeX, highlighting and their styles load on demand.

The host owns runtime state. The browser checks protocol version, epoch,
generation and cursor before applying snapshots, live events or older pages.
Retired streams and invalidated loads cannot publish late results. Drafts,
optimistic messages and expanded content belong to their conversation. Receipts
reconcile delivery without automatic resubmission; no browser status polling or
second event authority is introduced.

`ui.tsx`, `loading.tsx` and `popover.tsx` share controls, loading geometry and
native top-layer positioning. Loading surfaces use
[aria-busy](https://developer.mozilla.org/en-US/docs/Web/Accessibility/ARIA/Reference/Attributes/aria-busy)
and a single accessible status. Controls support keyboard, touch and reduced
motion. Appearance, read markers and scroll position are presentation state.

[Lucide](https://lucide.dev/guide/preact/getting-started) supplies imported SVG
icons with no icon font or network requests; generated notices retain its
[licenses](https://lucide.dev/license). Markdown HTML and math trust are disabled,
remote images are inert, and links use a scheme allowlist. Renderer fonts ship
locally. `npm run icons --prefix web` regenerates the shared mark's PNG sizes.
The [injectManifest service worker](https://vite-pwa-org.netlify.app/guide/inject-manifest)
precaches the public shell and provides explicit updates and notifications.

CLI `FmtCount`/`FmtBytes` and web `quantities.ts` share decimal display units:
`1k`, `1.5M`, `kB`, `MB`. Raw data and exports retain exact values. Provider
caching and shared token accounting are described in [CACHING.md](CACHING.md).

CI checks formatting, strict types, logic tests, byte-reproducible assets and
notices, browser behavior, push-enabled native builds and CLI-only builds.
Browser tests cover delayed delivery, reconnects, stale streams, conversation
switching, loading/retry, popup anchoring, raw downloads, mobile sizes and large
history windows. Native push tests compare RFC 8291 ciphertext and independently
verify VAPID signatures. Mock providers avoid paid model calls. These checks do
not establish physical-device interoperability or every network interleaving.

## Measured footprint

The [baseline](../benchmarks/baselines/web.json) records macOS arm64 Release/LTO
measurements on 2026-09-09. They describe this build and workload, not other hosts.
The earlier CLI reference is revision `5ceaad97`.

| Executable | Stripped bytes |
| --- | ---: |
| Earlier CLI reference | 2,168,808 |
| Current CLI-only | 2,423,816 |
| Default web | 3,820,360 |
| Optional push | 3,858,856 |

The default build adds no dynamic library dependency. Push adds OpenSSL libcrypto
and 38,496 stripped bytes. The default idle master used 9,296 KiB RSS / 19 threads;
with two completed workers, 10,736 KiB / 21 threads. Workers used 11,904 and 11,888
KiB / 7 threads each. Configured push without subscriptions used 12,736 KiB / 20
threads while idle. Both idle samples recorded 0.00 seconds CPU over three seconds,
limited by process-counter resolution. CLI execution starts no web threads.

The complete bundle is 857,416 raw bytes / 464,317 gzip level-6 bytes. The server
serves identity bytes; gzip is a comparison, not measured transfer compression.
The baseline also records initial JS/CSS, lazy assets and service-worker precache.
CI caps initial JS at 56/21 KiB raw/gzip, CSS at 16/4.5 KiB and total assets at
850/500 KiB. Native review triggers are 4 MiB stripped and 32 MiB idle master RSS.
CLI growth is 255,008 bytes over the earlier reference, exceeding the original
128 KiB review trigger; the shared management, configuration, fork and HTTP
inspection capabilities account for additional CLI code. No budget was raised.

The 2,000-message browser fixture must open its 64-message window within three
seconds and retain no more than 256 rendered history blocks. Ten browser tests
cover Chromium and WebKit, including phone layouts and 200% display / 300% text.
Performance on a physical phone remains unmeasured.

```sh
python3 benchmarks/web.py build/release/uagent --output /tmp/web-footprint.json
npm run size --prefix web
npm run notices --prefix web
```

Pass `--baseline PATH` and `--cli-only PATH` for executable comparisons. Add
`--push-contact mailto:benchmark@example.invalid` to measure the sender thread;
this registers no subscriptions and sends no notifications.

## Remaining device validation

Automated browser emulation is not a real-device interoperability test. Installed
iOS/Android background push, trusted HTTPS phone routing, mobile keyboards and
screen readers still require the physical-device checklist before claiming full
mobile acceptance. Test foreground/background/suspended delivery, mute/revoke,
expired endpoints, click-to-refresh, standalone relaunch, rotation, keyboard
visibility, focus restoration and draft-safe updates on each target device.
