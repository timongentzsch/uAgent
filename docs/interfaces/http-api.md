# HTTP API

The web host (`uagent --web`) serves its client over HTTP, Server-Sent Events
and two WebSockets. The caller is the bundled web client; any program that
holds a paired device's cookie may call the same routes. Commands and event
frames carry the protocol version `v` (`kProtocol`, 2). How the host works
behind these routes is in [Web host](../architecture/web-host.md).

## Authentication

Every `/api/` route except `/api/auth` and `/api/instance` needs the
`uagent_device` cookie of an active device (else 401). `Host` must name the
configured origin or `127.0.0.1:<port>`, a request carrying `Origin` must
match it, and every non-GET request must carry it (else 403); the full rules
are in [Web host](../architecture/web-host.md#protocol).

| Route | Request | Response |
| --- | --- | --- |
| `POST /api/auth` | `{code, name}`, at most 4 KiB | `{v, device}` and `Set-Cookie: uagent_device=…`; 403 bad or expired code, 409 at 16 devices, 429 over 12 tries a minute |
| `POST /api/instance` | header `X-Uagent-Instance: <secret from discovery.json>` | `{v, epoch, url, pairing_code}`; makes a new pairing code |

## Catalogue and conversations

`GET /api/sessions` (`?refresh` rescans the disk) returns `v`, `epoch`,
`cursor` (integer), `sessions` (metadata), `spaces`, `pins`, `scheduled`,
`terminals` (null with `web.terminal` off), `devices` (`{id, name}`),
`device` (the caller's), `commands`, `detail`, `thinking`, `capabilities`.

Session metadata (`SessionHost::Metadata`): `id`, `generation`, `cwd`,
`host`, `parent`, `title`, `model`, `status` (`draft`, `saved`,
`starting`, `interrupted`, `failed`, `closing`, `updating`, `deleting`,
`waiting`, `running`, `processing`, `idle`), `presence` (`active` or `""`),
`turn_active`, `needs_you` (a decision waits on a person, not on a parent
agent) with its `pending_prompt`, `updated` (ms), and
display fields that are not contract.

`GET /api/sessions/<id>` (`<id>` 16 to 64 hex digits) returns a snapshot:
`v`, `epoch`, `cursor`, `metadata`, `state`, `pending`, and `live_truncated`
for a running conversation or `live: []` for a saved one. A saved
conversation's `state` has `view` (one page of transcript rows), `usage`,
`context_tokens`, `context_window`, `model`, `turns`, `statistics`, `http`,
`route`, `permissions`; a running one's is the runtime's own state. Query
parameters select other reads:

| Query | Returns |
| --- | --- |
| `before=<n>` | the page of rows before row `n`, from the saved file |
| `detail=<id>[&raw][&offset=<n>]` | one row's full detail or (`raw`) HTTP exchange, paged |
| `http=<id or latest>&part=request\|response[&offset=<n>]` | a captured request or response body, paged, with `exchange` |

A paged body is `{text, next, bytes, more}`; read on with `offset=next`.
A file that does not load is 422.

## Commands

`POST /api/command` takes one JSON object and answers with a receipt.

| Field | Type | Meaning |
| --- | --- | --- |
| `v` | integer | 2 |
| `kind` | string | the command, below |
| `request_id` | string | 16 to 64 lowercase hex digits, new per command |
| `session_id`, `generation` | string | the conversation, for its commands; `generation` from its metadata |
| `host` | string | a remote host id, for `create`, `pins`, `terminal`, `config`, `restart_conversations` |
| … | | the kind's own fields |

The body may be 64 KiB, or 1 MiB (`kFrameBytes`) for `memory`, `skills`
and `instructions`. A body that is not a command with a valid `request_id`,
or `recall`/`insert` without `target_id`, is 400. A conversation command
without `generation` starts the runtime first; one with a stale
`generation` is refused.

| Acts on | Kinds |
| --- | --- |
| devices, push | `logout`, `revoke_device` (`device_id`), `test_notification` (`session_id`) |
| host | `restart_host`, `restart_conversations` (`cwd`, `target_id`), `update` (`action` `show`, `check`, `apply` with `version`, `chrome`), `config` without `session_id`, `terminal` (`action` `open` with `cwd`/`folder`, `rename` with `id`, `title`, `close` with `id`) |
| places | `space` (`action`, `cwd`, `name`), `pins` (`pins`: `[{kind, id}]`) |
| library | `memory`, `skills`, `schedule`, `import`, `instructions`, `permission_rules`, `models`, `provider`, `openrouter`: the [`--control`](headless.md#--control-json-) request; its reply is `result` |
| browser | `browser` (`action` `takeover`, `done`, `stop`, `create_profile`, `select_profile`, `setup_profile`, `configure`, `viewport`; `instance_id`, `interaction_id`, `name`, `profile_id`, `viewport`, `parallel_capacity`) |
| a conversation's lifecycle (host) | `create` (`cwd` or `space`), `activate`, `close`, `delete` (only when stopped), `move` (`cwd` or `space`) |
| a conversation (runtime) | `submit` and `steer` (`text`, `attachment_ids`, `queue`), `reply` (`interaction_id`, `text`, `cancelled`), `interrupt`, `recall` and `insert` (`target_id`), `rename` (`title`), `keep`, `refresh`, `permissions` (`mode`), `activity`, `tools`, `model`, `config`, `context`, `context_parts` (what fills the context, below; refused while a turn runs), `revert` (`turn`, `path`), `changes` (`turn`, `path`: the file's change in that turn as a unified diff), `fork` (`argument` or `message_id`), `share`, `side` (`operation`: `create` with `text` starts a side chat; `merge`, sent to a side chat, sums it up for the conversation it branched from) |

A runtime command is forwarded with a host-made id
(`SessionHost::ExecuteCommand`); `rename`, `keep`, `delete` and `fork` of a
stopped conversation are done by the host on the file. `attachment_ids`
names uploads (ids or `{id, name}`, at most 8, `kUploadCount`), which the
host resolves before forwarding. `escalate`, which hands an agent's
decision from its parent agent to the person, is sent by the agent link and
the parent's tools, not by a client.

`context_parts` returns `Agent::ContextBreakdown()`: `tokens` (the
estimate `context_tokens` reports), `bytes` and `parts`, the estimated
request bytes of `tool_results`, `replies`, `messages`, `files`, `notes`,
`tools`, `mcp`, `system`, `instructions`, `memory` and `skills`, which sum
to `bytes`; `tools` (`name`, schema `bytes`, `offered`, `server` for an MCP
tool); `window`; `compacts_at`, the `context_tokens` at which automatic
compaction starts (0 for none); `skills` (how many are listed) and
`instructions` (the file's name). A part's tokens are its share of `bytes`
times `tokens`.

The receipt has `request_id`, `accepted` (false with HTTP 409 and an
`error`), `pending` (the runtime has not answered within 2 s), `result`
when the kind has one, and `session` (metadata) for `create` and
`activate`. A pending receipt resolves through the runtime's `outcome`
frame on the event stream, or `GET /api/receipts/<request_id>`, which
returns the stored receipt or `{request_id, unknown: true}`. The same
`request_id` with the same body returns the stored receipt, with another
body 409. The host keeps 256 receipts and evicts only answered ones; with
all pending, a new command gets 429.

## Event stream

`GET /api/events?cursor=<epoch>:<n>` (or `Last-Event-ID`) is
`text/event-stream`. `update` carries one frame as `id: <epoch>:<sequence>`.
`ready` (`{"epoch", "cursor"}`, the cursor an integer) follows the replay
up to the connect-time cursor; a client should not mutate before it.
`resync` (`{}`) ends the stream when the epoch differs, the cursor is ahead
or frames after it were evicted: read `GET /api/sessions` again and
reconnect from its `epoch` and `cursor`. `: heartbeat` follows 15 s of
silence. Every frame has `v`, `epoch`, `sequence`, `session_id`,
`generation` (both `""` host-wide) and `kind`; one a person should hear
about also has `attention_id`.

```text
id: 6f0c…:42
event: update
data: {"epoch":"6f0c…","generation":"","kind":"settings.changed","level":"default","sequence":42,"session_id":"","thinking":false,"v":2}
```

| `kind` | From | Fields |
| --- | --- | --- |
| `event` | runtime | `type`, `data`, `time`: a runtime event the host does not fold into rows |
| `node` | host | `node` (a whole transcript row), or `id` with `set`/`append` |
| `state`, `turn_status`, `outcome`, `gap`, `error` | runtime | as on the session protocol ([Overview](../architecture/overview.md#protocol)) |
| `metadata`, `activated`, `deactivated`, `closed` | host | `metadata` |
| `deleted`, `moved` | host | none |
| `settings.changed` | host | `level`, `thinking` |
| `pins.changed`, `spaces.changed`, `terminals.changed`, `scheduled.changed` | host | `pins`, `spaces`, `terminals`, `scheduled` |
| `management.changed` | host | none: reread memories, skills, schedules |

The `type` values and row shapes follow the runtime and the client and are
not contract; treat unknown kinds as a reason to reread.

## Files, previews and assets

Under `/api/sessions/<id>/`; an unknown conversation, or a path outside the folder or hidden, is 404.

| Route | Request | Response |
| --- | --- | --- |
| `GET files?list=<dir>` | | `entries` (`name`, `dir`, `size`; pages add `kind`, `artboard`, `skip`, `count`), `more`, `kind` (the folder's) |
| `GET files?stamps=<dir>` | | `stamps`: each file's name and its size, inode and modification time as one string |
| `GET files?read=<path>` | | `text` (at most 1 MiB, `kFileViewBytes`), `bytes`, `more`, `revision` |
| `GET files?find=<q>` | | `paths`, `more` |
| `GET files?changes=` / `?diff=<path>` | | `changes` (`path`, `state`), `more`, `repository` / `text`, `more` |
| `GET files?raw=<path>[&download]` | | the bytes; only images are inline |
| `GET files?png=<path>` / `?pdf=<path>` | `w`, `h`, `scale`, `part` | a page drawn by the agent's browser; 503 without one |
| `POST files` | `{path, text, revision}`, `{path, text, create: true}`, or `{path, moves}` | `{revision, stamp}` or `{moved}`; 409 when the file changed |
| `POST attachments?name=<name>` | the file as the body, ≤ 8 MiB | `{id, name, mime, image, bytes}` |
| `GET assets/<asset id>[?download]` | | the stored file |
| `POST preview` | | `{base: "/preview/<48 hex>/"}` |

`GET /preview/<token>/<path>` needs no cookie. `?deck&w&h` returns a
folder's slides as one page, `?sheet` wraps a `.csv` or `.xlsx` in a sheet
page, and `~sheet/<file>` serves the sheet engine.

## Other routes

| Route | Response |
| --- | --- |
| `GET /api/folders?path=<p>[&host=<id>]` | `entries` (directories), `more`, `path`, `parent` |
| `GET /api/folder?path=<p>[&host=<id>]` | `spend`, `changed`, and `git` for a local folder; 404 when absent |
| `GET /api/search?q=<≥2 chars>` | `hits` (`session_id`, `title`, `rows`, …), `scanned`, `truncated` |
| `GET /api/search?q=…&session_id=<id>` | `hits` (up to 2,000 rows), `truncated` |
| `GET /api/library` | `places` (`path`, `name`, `workspace`, `session`), `items`, `limits` |
| `POST /api/library/delete` | `{kind, key, place}` or `{kind: "file", key, session_id}`; `{}` |
| `GET /api/hosts` | `hosts`: `id`, `address`, `phase`, `error`, `installing`, `version`, `round_trip_ms` |
| `POST /api/hosts` | `{action: add, address}` or `{action: connect\|reconnect\|disconnect, id}`, ≤ 4 KiB; `{id}` |
| `GET /api/hosts/<32 hex>/ping` | `{round_trip_ms}`; 404 for an unknown host, 503 when it does not answer |
| `POST /api/push/subscriptions` | body: a browser `PushSubscription` (`endpoint`, `keys.p256dh`, `keys.auth`), ≤ 8 KiB; replies with the capabilities: `push`, `push_reason`, `subscribed`, `vapid_public_key`; 422 when refused |
| `DELETE /api/push/subscriptions` | no body; the capabilities |
| `GET /api/user.css` | the person's stylesheet, `text/css`, ETag |
| `GET /api/browser/status?instance_id=<0-4>` | the browser service status plus `controller`, `leased`; 503 when the service does not answer; only with `UAGENT_BROWSER_DATA` |

## WebSockets

Both check `Origin` and the cookie, else close with policy violation.

- `/api/terminals/<id>`: binary frames carry shell output out and input
  in; text frames in are `{"type": "resize", "rows", "cols"}`; text frames
  out are `{"type": "exit", "code"}` and `{"type": "replaced"}`. A message
  over 64 KiB, or any other text, ends the socket. A newer socket for the
  same shell replaces the older.
- `/api/browser/viewer?instance_id=<0-4>`: binary frames carry an RFB (VNC)
  session. Display messages always pass; keys, pointer and clipboard pass
  only while this device is the controller. One socket per instance.

## Errors

A refused request is `{"error": "<text>"}` with 400, 401 (no or revoked
device), 403 (Host or Origin), 404, 409 (conflict, stale generation,
protocol), 413 (size), 415, 422, 429, 500, 502 or 503 (remote host or
browser unavailable). A command refusal is a receipt with `accepted: false`
and 409.

## Compatibility

`v` is the protocol version, shared with the session protocol. A command
whose `v` is not 2 is refused with 409 ("refresh the app"); the client
refuses a reply or frame whose `v` differs. Client and host ship in one
binary, so routes, kinds and fields change with it. Contract: the
authentication, the command envelope and receipt, the cursor, `ready`,
`resync` and the frame header fields. Not contract: `event` payloads,
`node` rows, `state`, library and folder facts, error texts.

## Source and tests

| Path | What |
| --- | --- |
| `src/web/master.cc`, `src/web/master_auth.cc` | route registration, Origin rule, auth |
| `src/web/master_command.cc`, `src/app/session_router.cc`, `src/app/session_command.cc` | commands, receipts, kinds |
| `src/web/master_stream.cc`, `src/app/replay_log.cc` | events, snapshot, search, uploads |
| `src/app/session_host.cc`, `src/app/session_snapshot.cc` | metadata, catalogue, snapshots |
| `src/web/master_files.cc`, `src/app/workspace_files.cc`, `src/web/master_library.cc`, `src/web/master_remote.cc`, `src/web/master_terminal.cc`, `src/web/browser_viewer.cc` | files, previews, library, hosts, sockets |
| `web/src/state/api.ts`, `web/src/shared/types.ts` | the client's routes and shapes |
| `tests/integration_web/auth.py`, `tests/integration_web/commands.py`, `tests/integration_web/recovery.py`, `tests/integration_web/files.py`, `tests/integration_web/preview.py`, `tests/integration_web/terminal.py` | pairing, commands, retries and `resync`, files, previews, terminals |
