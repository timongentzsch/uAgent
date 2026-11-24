# Web client

This page explains the Preact client in `web/src/`: how it is built into the
binary, how its code is layered, which state it owns, how it follows the
host's event stream, and how it talks to the pages it frames. The host side
of the same protocol is in [Web host](web-host.md); routes are in
[HTTP API](../interfaces/http-api.md).

## Responsibility

The client submits commands and folds the host's and runtimes' events into
a view. Conversation, execution, approval and accounting state are the
runtime's; the client owns navigation, drafts, editing, gestures, scroll
position, theme and zoom.

## Processes and threads

**Build.** `npm run build` in `web/` type-checks, runs `vite build` into
`dist.next`, writes third-party notices, and `scripts/promote-dist.js`
gzips every text asset (level 9) and writes the list of files to embed,
`.assets`, last. `cmake/embed_web.cmake` reads that list and generates
`web_assets.cc`: one `Asset` per file with its bytes (gzipped where a
matching `.gz` exists), plain size, MIME type and ETag. A `.gz` whose source
hash does not match `.gzip-manifest.json` fails the configure step. With
`UAGENT_EMBED_WEB=OFF` only `design-helper.js` is embedded. The sheet code
(`web/sheet/`) and its engine go to `dist/share/sheet` and are installed
beside the binary, not embedded. Tests point the host at a fresh build with
`UAGENT_INTERNAL_WEB_DIST`.

**Page.** `index.html` runs `/theme.js` and draws a static shell before
`app/app.tsx` mounts. Heavy renderers (Mermaid, KaTeX, highlight.js, the
browser viewer, xterm) are lazy chunks that the `renderer-assets` Vite
plugin keeps out of the precache.

**Service worker** (`web/src/sw.ts`, Workbox `injectManifest`): precaches the
build manifest, serves `/index.html` for navigation to `/`, keeps lazy
renderer files in one cache across builds and, on activate, deletes those
the current build does not list. It never caches `/api/` traffic and has no offline
queue. It shows a notification for every `push` and for `ATTENTION`
messages from the page, and routes a click to `/#session=<id>&decision=<id>`.

## Data and formats

Layers, with the boundaries `web/tests/boundaries.test.js` enforces:

| Directory | Holds | May not import |
| --- | --- | --- |
| `state/` | host connection, event reducer, snapshots, API client | any `.tsx`, `features/`, `app/` |
| `shared/` | primitives, styles, storage, formatting, motion | `features/`, `app/` |
| `features/<name>/` | one screen or pane each | another feature |
| `app/` | the shell: routing, menus, modals, command hooks | (unrestricted) |

A string starting `/api/` may appear only in `state/api.ts`, which also
fixes `protocol` 2 for every command.

What the host owns and the page projects: the catalogue (sessions, spaces,
pins, schedules, terminals, devices), each conversation's snapshot
(`metadata`, `state`, `view.nodes`, `pending` decision), command outcomes
and the `attention_id` of what needs a person.

What the page owns, in `localStorage` through `shared/storage.ts`, with
keys named once in `shared/storage-keys.ts`: unsent drafts, the last
catalogue, spaces, pins and hosts (to paint the first frame), read counts
and unread marks, theme, zoom, panes and display preferences. Per tab, in
`sessionStorage`: the page shown, outgoing messages and transcript scroll
bookmarks. When storage is denied or full, values live in memory for the
visit. Signing out clears the catalogue, drafts and outgoing rows.
Transcripts are never stored: snapshots live in memory (`snapshot-store.ts`),
the selected one plus at most 4 background views (`retainedBackgroundViews`).

## Protocol

**Connecting** (`state/use-host.ts`). At start the page asks for
`GET /api/sessions` (with `refresh` for a rescan) and, if it listed the open
conversation before, its snapshot in parallel. It then asks
`GET /api/receipts/<id>` for each outgoing row not still waiting to send,
and opens an `EventSource` at `<epoch>:<cursor>` from the list. Transport
open is not readiness: mutations stay disabled until `event: ready` arrives
with the same epoch and a cursor not below the one asked for. `resync`, an
unparsable frame, or `ready` from another epoch starts a full refresh. A
frame with another protocol version or epoch stops the stream and reports
that the app and host differ.

**Reconnecting.** When the stream drops and the page holds a catalogue, it
reopens at the `lastEventId` it applied last and keeps its catalogue and
snapshots; only a start, a rescan or a lost cursor reads them again. A
stream closed by a non-200 answer retries the whole refresh with jittered
backoff from 1 s up to 30 s (`reconnectBaseDelayMs`,
`reconnectMaxDelayMs`). A 401 signs the device out; any other failure keeps
the app on screen. `online`, `pageshow` from the back-forward cache, and a
return to visibility with no stream open all reconnect.

**Reducing** (`state/apply-event.ts`, `store.ts`). Host frames
(`settings.changed`, `pins.changed`, `spaces.changed`,
`terminals.changed`, `scheduled.changed`) patch the catalogue. A `gap` or
`moved` refreshes. A session frame at or below the snapshot's cursor is dropped, and
so is one from another runtime `generation` unless it is lifecycle
metadata. `node` frames place or append transcript nodes, `state` replaces
the snapshot's state and pending decision, `deleted` forgets a session.
Changes are written to the snapshot store once per
animation frame, and only for the selected or watched conversations.

**Agent replies.** A child's delivery uses the ordinary message renderer at
its recorded sequence in the parent chat. Its sender opens the child
conversation. An assignment and any replies returned directly by the tool
use the normal expandable call row. A request addressed to the parent stays
in history with its details. A live child with `needs_you` exposes a direct
review action even when its work is folded.

**Outgoing messages.** A message gets a client `request_id` and an outgoing
row. `api.command` posts it and, if the reply is `pending`, waits up to
30 s (`commandReceiptWaitMs`) for the outcome on the stream. `outgoingAfter`
maps an outcome to a status: `unknown` is "Not confirmed", not accepted is
"Not sent", `pending` is "Awaiting confirmation", else "Sent"; queued
guidance keeps its status until the transcript consumes it. A row is
removed when a snapshot's view holds a node with its `request_id`. Plain
text typed while offline waits ("Waiting for connection") and goes out
under the `request_id` it was given; attachments and slash commands need a
connection. After a reconnect the page asks for receipts and never resends
on its own. **Retry** on a refused or unconfirmed row is a person's choice
and sends the text again under a new `request_id`.

**Framed pages** (`shared/page-frame.tsx`). Previews and sheets load from
the preview address in a sandboxed opaque origin. The host inlines
`design-helper.js` into each HTML page. Both sides post
`{uagentDesign: 1, type, …}` with target `"*"` and check `event.source`.
The page sends `ready`, `drawn`, `error` and editing results; the app
sends `select`, `edit`, `format`, `markdown`, `find`, `tweak`, `input`,
`suggest` and `do`. A sheet takes the app's colours and font as
`--sheet-*` tweaks, again whenever theme or zoom changes.

**Own stylesheet.** `GET /api/user.css` is loaded after the app's styles and
read again on `ready` and on each `turn_status` frame, revalidated by ETag.

## Invariants and limits

- Components derive conversation and execution state from the shared
  snapshot; no component keeps a second machine for them.
- A receipt that times out is not a rejection; the page does not resend on
  its own.
- Every colour, spacing, size, weight, radius and motion value comes from
  `shared/styles/tokens.css`; only `shared/styles/` sets the control, row
  and spacing tokens (`web/tests/css-tokens.test.js`).
- Screens compose `shared/ui.tsx` primitives; a raw `<button>`, `<select>`,
  `<input>` or `<textarea>` in `features/` or `app/` needs a `// raw:`
  reason (`web/tests/primitives.test.js`).
- `web/tests/layout.test.js` with `layout-rules.js` requires `.truncate`
  for ellipsis, button sizes from variants, fields in `InputRow`, the
  shared `Bar`, `Cluster` and `Actions`, and allows exactly eight
  container and media query widths.

## Failure modes

- Host unreachable: the last catalogue stays on screen, controls are
  disabled, and the stream is retried. Only a 401 shows the pairing form.
- Cursor evicted or host restarted: `resync`, then a full read.
- A message the host refused or never confirmed stays at its row with the
  reason and a retry.
- Storage denied: the app works from memory; nothing persists past the
  visit.
- Offline: the cached shell loads, but nothing can be read or sent.

## Source and tests

| Path | What |
| --- | --- |
| `web/src/state/use-host.ts` | connection, refresh, reconnect, outgoing |
| `web/src/state/apply-event.ts`, `web/src/state/store.ts` | event reducer, `outgoingAfter` |
| `web/src/state/api.ts` | routes, `command`, receipts |
| `web/src/state/snapshot-store.ts` | in-memory snapshots |
| `web/src/shared/storage.ts`, `web/src/shared/storage-keys.ts` | browser storage |
| `web/src/shared/page-frame.tsx`, `web/public/design-helper.js` | framed pages |
| `web/src/sw.ts`, `web/src/shared/pwa.ts` | service worker, updates |
| `web/vite.config.js`, `web/scripts/promote-dist.js`, `cmake/embed_web.cmake` | build and embed |
| `web/tests/boundaries.test.js` | layer rules |
| `web/tests/layout.test.js`, `web/tests/css-tokens.test.js`, `web/tests/primitives.test.js` | layout and tokens |
| `web/tests/store.test.js`, `web/tests/storage.test.js` | reducer, storage |
| `web/tests/stream-reload.spec.js`, `web/tests/send-flow.spec.js`, `web/tests/reload.spec.js` | reconnect, sending, reload |
