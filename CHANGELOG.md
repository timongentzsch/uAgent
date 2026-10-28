# Changelog

## Unreleased

### Upgrade notes

- **Yolo raises the sandbox's `default` level to `relaxed`.** While a session is in yolo, its commands read anywhere and write the home folder, except `~/.uagent`; the credential stores, the browser profile and µAgent's settings stay hidden. Ask and auto keep `default`, an explicit `readonly`, `relaxed` or `off` is unchanged, and a lead agent stays `readonly`. `/status` names the level in effect (`relaxed (yolo)`). A background agent can now be put in yolo too, from `/permissions`, `--yolo` or the web's control; it still starts in auto. Once you write to a background agent yourself, starting its turn or into one that runs, that turn's approvals and questions come to you instead of its lead agent.
- **Tool results have one contract, and their bracket notes are for the model only.** A result is its output, facts (exit, lines added and removed, a count, why it was refused) and a hint the model reads after the output: `[exit code N]`, `[path lines a-b of N]`, `[ripgrep · N result lines]`, `[captured log: …]`, the activity and `[Activity result: …]` headers, the web search and fetch notes, `[image saved: …]` and the `[running]`/`[started]`/`[detached]` lines now come at the end of what the model reads, and no row shows them. On the wire, tool rows and activity and memory receipts carry `state` (`ok`, `failed`, `cancelled` or `running`) instead of `status` (`success`, `completed`, `complete`), `result` (the facts and their `word`, such as `exit 2 · 1.4s`), `body_kind` instead of `view.output`, and `error_code`; `change` holds only the diff, without its `Edited path (+N -M)` line; `exchange_path` and the snapshot's `?artifact` query are gone. Conversations saved before show their tool rows without facts, and a row whose facts are missing shows no text.
- **The transcript reaches clients as one tree of nodes.** Every row of a view, a history page and a `block` patch is a node `{id, parent, kind, headline, result, state, routine, time, body}`. `kind` is `turn` (a message of yours), `message` (with its `role`), `work`, `group`, `call`, `thinking`, `notice` or `footer`, instead of `user`, `assistant`, `attachment`, `tool_result`, `activity`, `turn_summary` and `compaction`, and text is in `body.text`, a change's diff in a `diff` body. A reply's thinking is a node of its own; a turn's work (`Worked · 6 steps · edited 1 file · 1 error · 2m 4s`) and a run of like calls (`Explored · 3 calls`) are nodes the host makes, so `activity.group` is gone. A new node goes before the first node under it, else in the order it arrived, and streamed text appends to `body.text`. Gone from rows: `text_bytes`, `content_revision`, `content_complete`, the three `reasoning_*` counters, `reply_to`, `turn_root`, `detail_id`, `call_id`, `occurrence_id`, `response_id`, `name`, `arguments`, `view` and `error_code`; `?detail=` takes a node's id only. History pages end on whole turns, and search hits name their node's `id` and kind (the `history` tool's `message_id` is now `id`). Conversations saved before show their turns' work without counts and their calls ungrouped.
- **Actions that carry their own payload are tools of their own,** so their schema requires that payload and a provider that enforces schemas sends it: `thread` `spawn` is `thread_spawn` (`title` and `objective` required), `subagent` `operation=fanout` is `subagent_fanout` (`items`, `prompt`, `finding`), `uagent` `configure` is `uagent_configure` (`scope`, `changes`) and `set_instructions` is `uagent_instructions` (`audience`, `scope`, `text`), and `session` `operation=message` is `session_message` (`prompt`, with `session_id` or `broadcast`). `uagent` only inspects and `session` only lists; neither takes `action` or `operation` any more, and `thread` requires `session_id`. Update scripts, toolsets and allowlists that name the old shapes.
- **Commands no longer read your other folders.** Below the `relaxed` level, commands cannot read the home folder's visible entries, `~/.uagent/spaces` or neighbouring workspaces, only their own workspace and tool folders. Add folders with `sandbox.read`.
- **Session ids changed.** The session and workspace hash was corrected, so conversations saved before are not found.
- **Threads saved before this version are ordinary conversations.** The old `thread` header is not read.
- **No compatibility with older saved data.** Conversations missing display metadata, context window, last prompt, delivered mail or object attachments are not loaded; a panel saved as `coordinator.json` is not read (use `panel.json`).
- **The panel's chat with members is gone.** `add_member`, `panel.chatTurns` (`UAGENT_PANEL_CHAT_TURNS`) and "Name, ..." addressing are removed; the moderator asks read-only subagents instead. Saved members are not converted.
- **Names and formats from v1.4 and earlier are no longer read.** Rename `COORDINATOR.md` to `PANEL.md`, set `sandbox.level` instead of `sandbox.enabled`, and note `UAGENT_SANDBOX` takes only `strict`, `default`, `relaxed` or `off`. A format 2 settings file needs one run of v1.5 first.
- **Settings and tasks from v1.3.1 or earlier are no longer read.** Run v1.4 or v1.5 once to rewrite format 1 settings and tasks saved with `"permissions": "prompt"`.
- **The self-directive is gone.** `adapt_system`, `prompt.adaptSystem` (`UAGENT_ADAPT_SYSTEM`), `/instructions clear` and the web's Clear are removed.
- **Removed commands:** `/debug-config` (use `/config KEY`, which now also shows the conversation's own value), `/models` (use `/model QUERY` or `/model all`), the aliases `/new`, `/ctx`, `/commands` and `/resume` (`/reset`, `/context`, `/help`, `/sessions`), and `uagent config export` and `uagent config import` (copy `~/.uagent/config/settings.json`).
- **Removed settings:** `approval.allowAbove`, `approval.denyAbove` and `approval.vetoAbove` (set `decision_bars` on the provider), `mcp.roots` (set `roots` per server in `.mcp.json`), `project.trustConfig` (use `--trust-project-config`), `memory.redactKeywords`, `terminal.markdown` and `terminal.reducedMotion` (use `--plain`); `UAGENT_INTERNAL_TOOL_RUN_ALLOWLIST` is gone.
- **Background work is called activity everywhere.** The turn's status line is now `turn_status` on the wire (frame kind, session state and metadata field; event `turn.status`), activity rows have `kind` `command`, `subagent`, `memory` or `detached` (was `agent`), result headers read `[Activity result: <kind> <id> ...]`, mail says `[activity finished`, the status row counts `act:N`, and the activity logs live in `~/.uagent/activity/` (was `bg/`). Old logs under `bg/` are left in place.
- **The `strict` sandbox level is gone.** Use `sandbox.network` off (`UAGENT_SANDBOX_NET=0`) with `default`. A saved or exported `strict`, or any value that is no level, refuses every command and says so, rather than becoming `default` with the network open.
- **A `-p` run is saved like any conversation**, so `-c -p` continues the latest one and scripts leave one history entry per run.
- **Headless `--json` reports this run's usage only**, not what a continued conversation spent before; a conversation another runtime holds gets an error envelope.

### Changes

- **Every call reads as a verb and a target, with its result in a few words** (`Ran npm test · exit 1 · 4.2s`, `Edited a.ts · +3 −1`, `Searched /x/ in src · 12 lines`, `denied`, `rejected: …`). `thread` says what it did (Messaged, Stopped, Closed, Diffed, Deleted) and `plan` too (Showed the plan, Set, Added to, Cleared, Updated step); an MCP tool reads "Called <Tool>" with its first short argument, never its raw JSON. A skill loaded is a row like any other.
- **The host builds the transcript once for every client.** Live rows and saved history were built by two builders, and a turn's "Worked" summary was counted in the web and again in the terminal; one builder now makes the same nodes for both from what was recorded, and counts a turn's work once.
- **A refused call reads as what it would have done** (`Ran cd /tmp/fli && …`), with the error beneath, instead of as its raw JSON arguments.
- A reply that is only blank lines (some models send one before their calls) is no longer drawn as an empty row with its own time and ⋯ menu.
- An answer's time always shows; only its ⋯ menu waits for the pointer. The line was reserved either way.
- **Failed calls fold into a turn's work** instead of standing open below it; the summary counts them (`Worked · 28 steps · edited 3 files · 4 errors`). Cancelled calls and files handed to you keep their rows.
- **The file tools treat the sandbox's writable roots as the work area**: writing a script to `/tmp` (or `$TMPDIR`, the package caches, a `sandbox.write` root) no longer asks, as the same write by a sandboxed command never did. Hidden stores, closed folders and µAgent's own configuration keep their checks.
- **The composer is one row while the draft is one line** on a wide enough screen: attach, model, approval and send sit beside the input, and the second row comes back once the text wraps. Phones keep two rows.
- **On a phone the terminal is its own full-screen view**, and the keyboard shrinks it instead of covering it; Add to chat steps back to the draft. The web UI is a little denser everywhere: one spacing scale (padding 8px, gaps 6px, as phones had), 36px roomy rows and headers, prose line height 1.5. Touch targets stay 44px.
- **Each model call records which provider served it.** A router such as OpenRouter picks the upstream per request; its name and the generation id now go in the debug trace's `model_response`, the `/http` capture and the message's statistics ("served by …"). The conversation's statistics and `/cost` count model calls and rejected tool calls per serving provider, so a provider that mangles tool calls stands out.
- **A tool call missing a field its action needs is stopped before it runs, and says what arrived.** `thread`, `subagent`, `activity`, `memory`, `session`, `plan`, `history`, `uagent` and `browser` declare the fields each action needs; their descriptions list them, and a call without one is told every missing field and what it carried (`` `title` and `objective` are required for spawn; this call carried only `action` ``). Before, most tools checked their own fields only once the call ran, so repeating the call never ended the turn. `activity` `write` takes a left-out `chars` as an empty write.
- **A terminal in the web interface.** A panel under the chat (the header's Terminal button, Ctrl+`) holds tabs of your own shell on the host, unsandboxed, for paired devices only: in the conversation's folder, your home folder without one, or a folder from Files. Shells keep running through a reload and end when closed, when they exit or after `web.terminalIdleMinutes` (60) unseen and silent. The agent never sees them; **Add to chat** puts the selection or the screen into the message. `web.terminal` (`UAGENT_WEB_TERMINAL`) turns it off.
- **The agent adds an endpoint without dropping the others.** A `configure` call for `UAGENT_PROVIDERS` names only the providers it adds or changes, and `null` removes one; the rest, and keys the agent is never shown, are kept. Before, the value replaced every provider, so adding one deleted the endpoint the conversation was using.
- **A tool call missing a field its action needs is stopped before it runs, and says what arrived.** `thread`, `subagent`, `activity`, `memory`, `plan`, `history` and `browser` declare the fields each action needs; their descriptions list them, and a call without one is told every missing field and what it carried (`` `text` is required for message; this call carried only `action` and `session_id` ``), as a call missing a field its schema requires is. Before, most tools checked their own fields only once the call ran, so repeating the call never ended the turn. `activity` `write` takes a left-out `chars` as an empty write.
- **The agent adds an endpoint without dropping the others.** A `uagent_configure` call for `UAGENT_PROVIDERS` names only the providers it adds or changes, and `null` removes one; the rest, and keys the agent is never shown, are kept. Before, the value replaced every provider, so adding one deleted the endpoint the conversation was using.
- **More limits are settings,** for you and for the agent: `activities.maxConcurrent` (commands and subagents at once), `tools.concurrency`, `tools.readFileKb`, `tools.editFileMb`, `tools.listEntries`, `tools.grepResults`, `activities.logMb`, `subagent.callsPerTurn`, `webSearch.maxResults`, `webSearch.maxUses`, `webSearch.callsPerTurn`, `webSearch.timeout`, `memory.max`, `memory.maxBytes`, `memory.alwaysBytes`, `memory.idleHours`, `skills.max`, `instructions.maxKb`, `mcp.maxServers`, `mcp.maxTools`, `attachments.maxCount`, `attachments.maxImagePx`, `history.files`, `runtime.idleMinutes` and `limits.repeatedCallStop`. Log retention is one pair, `logs.keepDays` (14) and `logs.keepFiles` (200), replacing the separate 14/50, 7/200, 7/100 and 7-day values for sessions, activity, MCP logs and terminals; session traces and MCP logs now keep more files than before.
- **The agent's Chrome blocks `passwords.google.com`**, where a saved password could be revealed; manage logins from your own browser.
- **`/model QUERY` lists the models before it asks for a number.** The list came after the question, once the command ended, so the picker asked about models it had not shown; "searching all model catalogs" also shows while the search runs.
- **Browser results for the page just seen carry only what changed** (`- old`, `+ new` lines); a navigation or `read` gives the full page. On a 42-action form this cut cost-weighted tokens by 15–17%.
- The directory a conversation runs in is a folder everywhere it is shown or told to the model (the setting scope reads "This folder"); "workspace" now means only the group µAgent keeps under `~/.uagent/spaces`. Setting keys, scope values, file names and the `--trust-project-config` flag are unchanged.
- The web host gzips API JSON over 1 KiB for clients that accept it (a conversation snapshot is about an eighth of its size), and answers `If-None-Match` for `index.html`, `sw.js` and other unhashed files with 304.
- After a dropped connection the web app reopens the event stream where it stopped and keeps what it holds; it reads the list and conversation again only when the host cannot replay.
- Mermaid diagrams no longer ship the ELK layout engine (about 450 KB smaller download); a diagram that asks for ELK is laid out by dagre.
- A code block is highlighted when it closes, not on every streamed frame, and highlighted blocks are cached. The rendered-markdown cache holds 1M characters, not 4M.
- The service worker keeps one renderer cache across updates and removes only files the new build no longer lists; the conversation list is saved to local storage once it settles.
- Search: Ctrl+K (⌘K) opens a search panel replacing the command palette and sidebar filter; it finds conversations, messages (with occurrences marked), actions, commands and settings. `>` lists commands only.
- Find in conversation (Ctrl+F) marks every occurrence and steps through them with Enter and Shift+Enter.
- The web host answers `GET /api/search?q=...` across conversations, tool output included; a panel's `history search` uses it and now finds compacted rows.
- A lead agent (formerly moderator) has `run` in a new `readonly` sandbox level: it reads the workspace, writes only temporary files, has no network and needs no approval; subagents inherit it. `sandbox.level=readonly` can be chosen for any conversation.
- Panel terminology follows Claude Code: lead agent, subagents, background agents, Plan pane. Commands, settings, files and tool names are unchanged.
- The lead agent works like a research lead: it has `subagent`, `web_search` and `web_fetch`, fans questions out in batches, can have findings refuted, and records results in its plan. Panel subagents get only the panel's tools and need no approval.
- `subagent_fanout` runs one child per item and then refuters for each finding, returning what held, what was refuted and what stayed unchecked. With `refuters=0` findings are reported as unchecked.
- `subagent` takes a `schema` (invalid answers are sent back twice at most) and a `step`; a batch runs up to six children at once.
- `subagent.model` (`UAGENT_SUBAGENT_MODEL`) sets the route for subagents whose call names none.
- The panel's `state` tool is now `plan`, with steps (id, title, status, result) besides goals, decisions and open questions; the web board shows the plan with each step's threads and subagents.
- The lead agent is no longer told to send every question to subagents, and no longer starts with a warning that its workspace is not writable.
- Deleting a conversation removes its lock, a lead agent's plan and its subagents' transcripts; its background agents stay.
- Web: **New -> Workspace...** opens the workspace at its lead agent.
- A refused approval reads "this action was not approved"; `uagent panel -p` declines for nobody and prints only the panel's last answer.
- A command failing a name lookup while the sandbox denies the network now ends with the sandbox's note that the network is closed.
- On macOS the sandbox lets commands write the per-user temp folder, which stopped `git` printing cache errors.
- The system prompts say that `$...$` and `$$...$$` math renders.
- A background command's result reaches the model once; if the conversation is idle, it starts a turn. Guidance sent while `run` waits (`yield_ms: 0`) moves the command to the background.
- A runtime lets go one idle period after its last input; a child's SIGCHLD no longer restarts the period.
- Web: reloads paint the app frame first, restore Library and Scheduled from samples, and keep the app on screen while the host is unreachable; no spinner shows for loads under 150 ms.
- Web: dialogs fade in with their content; statistics show input (cached and cache-written included), output and cache hit rate; a subagent on the board shows its tokens.
- Web: zoom in Settings -> This browser is a list of steps (50 to 200%); a saved zoom between steps snaps to the nearer one.
- Web: running-time counters in tool rows, the activity list and the viewer count up each second; a failed diagram renderer offers Retry.
- Web: a saved conversation's composer shows the model and approval mode it resumes with.
- Web: statistics open for rows loaded with "Load older retained messages".
- Web: a narrow tool row keeps its name beside its chevron and a running turn's state uses the status line's width.
- Settings lists each named provider on one line and edits it in a sheet, including hosted tools (`hosted_tools`), shown as "Using <tool>" while they run.
- A provider-run tool no longer turns streamed input into a nameless call on Anthropic Messages.
- Statistics read full usage everywhere; `/cost` shows cache writes and per-route hit rates; titles, compaction and memory extraction are billed under their own route.
- The context estimate adds what the provider last reported, so auto-compaction no longer waits for twice the intended size.
- The agent reads the same memory store as the Library; a conversation run with `--no-memory` is never extracted.
- The offered tools change only with configuration, so starting background work no longer rewrites the prompt cache.
- A panel's daily spend counts what earlier threads spend today and warns when the route reports no cost.
- `browser` reads the page console and requests, and can right-click, double-click, drag, press key combinations, go forward and reload. Late downloads are reported with the next result.
- The agent's Chrome no longer reads as a test browser (no `navigator.webdriver`, software WebGL, key-by-key typing, `TZ` and `LANGUAGE` passed on, common fonts); PDF links download so the agent can read them.
- The terminal draws tool rows from the same transcript rows as the web, live and on resume.
- Minimal detail: a reply with calls after it folds into the turn's work row.
- `UAGENT_PROVIDERS` and other environment settings resolve `$NAME` references like saved settings.
- `install.sh` asks whether µAgent runs in the container or here (`UAGENT_INSTALL=container|here`); on Linux it can install Chrome, TigerVNC and the Chrome policy.
- The README compares µAgent's footprint with Claude Code, Codex CLI and omp; `benchmarks/compare_agents.py --omp` measures omp.
- An approval's choices read the same in the terminal and the web; a typed `/rename` follows the web's title rules.
- Titles and workspace names are limited to 256 and 64 characters, counted as characters.
- A client joining a running conversation no longer gets stale state frames after its snapshot.
- A long conversation no longer forgets its model, approval mode and fork origin.
- A mail's author travels with it when two mails say the same thing.
- Case-insensitive matching folds only ASCII letters.
- The project trust store and session links, memory receipts, job records and mail are read only up to 16 MiB and only as regular files; a full receipt table evicts the oldest completed command.
- A folder created while the host sets up file watches (such as `.uagent/`) is noticed.
- Sandbox: a file tool's failed edit in a closed folder no longer returns lines of the file under yolo; writing there asks like reading does.
- Sandbox: `sandbox.read=~/folder` opens the home folder's `folder`; a `~/bin` on `PATH` is readable; `~/go` opens while `GOPATH` is unset; `~/Library/Python` stays readable on macOS; `~/.git-credentials` is hidden with the other credential stores.
- Sandbox: a linked git worktree whose repository is in a closed folder (a sibling worktree, a lead agent's thread worktree) can use git there: its git directories open for reading, and a commit may write the worktree's git directory and the shared `objects`, `refs` and `logs`.
- `/status` and `/context` list the folders the sandbox opens for reading; a policy too large for the sandbox says it counts closed folders as well as writable roots.
- The runtime context ends with a closing line so the model no longer reads the memory index as the whole request.
- The terminal status line counts commands running behind the turn (backgrounded or detached) as `bg:` and only the ones the turn waits on as foreground; backgrounded commands used to be counted as foreground.
- Text cut from the middle is cut at one rule everywhere: the cap includes the marker, the marker reads `[N bytes omitted]`, and a character is never split.

## v1.5.0 - 2026-10-20

### Upgrade notes

- **No model is chosen for you.** The default model and the fallback to an endpoint's first model are gone. Without a model, the terminal and web open the model picker; `-p` and `--json` exit with `no model set`, so set `model` (`UAGENT_MODEL`) for scripts.
- **Background work runs on the conversation's model.** Title, memory extraction and subagents use it unless their setting names another; `title.model` is empty by default. With `image.model` empty, images go to the first model of the endpoint that accepts them.
- **Web search off the conversation's endpoint needs `webSearch.model`.** `web_search` goes through the first endpoint declaring `search_plugin` only when `webSearch.model` is set. `webSearch.backend` is `auto` or `off`; a saved `openrouter` reads as `auto`.
- **The settings file is format 3.** `openrouter.provider`, `openrouter.variant`, `pdf.engine` and their `UAGENT_*` variables are gone; they migrate to `providers.openrouter.options.provider` and `...options.pdf_engine` on first read. `$NAME` values cannot move and are reported; set `options` and `supports` yourself in `UAGENT_PROVIDERS`.
- **A provider's `protocol` is replaced by `supports`.** On rewrite, `"protocol": "openrouter"` becomes the matching feature list; `"protocol": "anthropic"` is dropped.
- **A routing variant is part of the model id.** `/variant` and the web's Variant field are gone: write `openrouter/vendor/model:nitro:high`.
- **An address declares what it is.** `UAGENT_BASE_URL` or a provider at `openrouter.ai` gets OpenRouter's dialect, and `api.openai.com` OpenAI's.
- **Every request to an endpoint declaring `prompt_cache_control` asks for prompt caching**, not only `anthropic/...` models.
- **Model catalogues under `~/.uagent/models/` are fetched again once.**
- **Commands, MCP servers and file tools no longer see saved logins.** `~/.uagent/config`, `~/.aws`, `~/.config/gcloud`, `~/.azure`, `~/.kube`, `~/.gnupg`, `~/.netrc` and `~/.docker/config.json` are hidden; `sandbox.reveal` lifts one and `sandbox.hide` adds your own.
- **MCP servers run under the sandbox.** A server needing more takes `sandbox.write`, or `"sandbox": false` in its `.mcp.json` entry.
- **`sandbox.enabled` is now `sandbox.level`** (`UAGENT_SANDBOX`): `strict`, `default`, `relaxed` or `off`; saved `false` becomes `off` and `true` `default`.
- **Auto approval asks a decision model, or asks you.** An endpoint must declare `decisions` (OpenRouter does); without one every call goes to you, and some calls allowed before now ask.
- **Auto approval's settings changed.** `approval.reviewerUrl` (`UAGENT_PERMISSION_URL`) is gone; use `options.decisions_url`. `approval.reviewerModel` takes `[endpoint/]model`; `approval.allowAbove`, `denyAbove` and `vetoAbove` override the bars.
- **Decision requests ask for providers that retain nothing.** A model with no such provider answers 404; set `decisions_private: false` in the endpoint's `options` to accept its retention.

### Added

- Web: **Files** in a conversation's header shows its folder read-only beside the chat, with search, highlighted code, rendered markdown and images.
- Web: the sidebar, a panel's board, Files and its file list resize by dragging or arrow keys; widths are kept per device.
- `browser` `upload` gives a conversation file to a file field or chooser; downloads land in the conversation's `downloads` folder.
- Auto approval shows why it asked: the risks seen, their probabilities, the deciding bar and endpoint errors in words. `tests/reviewer_eval/` holds labelled calls and a bar-finding script.
- A provider named like a well-known endpoint states only what differs; other endpoints list features in `supports` and extras in `options`.
- Sandbox levels: `strict` also denies the network; `relaxed` makes the home folder writable except `~/.uagent` and hidden stores. Explicit `sandbox.network` or `sandbox.write` still wins.
- On Linux, a sandboxed command's changes to `.mcp.json` or `.git/config` and `.git/hooks` are reverted when it ends, and the model and you are told.
- `sandbox.hideEnv` (`UAGENT_SANDBOX_HIDE_ENV`) names environment variables that commands, MCP servers and delegated agents never inherit.

### Changed

- `browser` looks list controls in open shadow roots and same-site frames, say when a control is disabled, required or expanded, and mask shadow-root password fields in screenshots.
- Web: settings resets are undo arrows, saved secrets show as dots, workspace avatars sit on their own line, and tool input wraps.
- Request fields formerly tied to the OpenRouter protocol follow declared endpoint features; diagnostics carry `features` instead of `protocol`.
- Web push and model requests refuse the same private address ranges.
- Long answers, large command output and busy event streams cost less, and the web host's worker pool no longer starves under many event streams.
- The terminal repaints at most once a frame, no longer lists an `@path` directory on every repaint, and holds its spinner still with reduced motion.
- SECURITY.md describes the sandbox by level.

### Fixed

- `browser` in the Docker image works with current Chrome again; a native Linux host with the policy must reinstall it (`install.sh` prints the command).
- `run` no longer refuses commands starting with `python`, `python3`, `pip` or `pip3`.
- `browser` `type` sets lists, date, time and color fields and refuses unsupported values; several actions from one look work; approvals cover an action per site and show its target.
- `browser` handles alert, confirm and prompt dialogs, hovers before clicking, names what covers an element, and refuses password fields (use `fill_saved` or `request_human`).
- The Docker image carries the bundled skills and documentation.
- A message with files sent during a turn is taken as guidance instead of refused as busy.
- A panel's threads start in the folder itself outside a git repository.
- Panel members answer from the start, discussions close with the moderator's answer, and moderator messages into a thread no longer count toward the restart limit.
- `web_fetch` reads PDFs, and a 404 points to `web_search`.
- The automatic review judges a call against the session's first request as well; `read_path` and `grep` read µAgent's own output (such as `~/.uagent/artifacts`) without asking.
- Subagents started in one batch run together and share a session budget equally; those the turn waits for are listed and stoppable in the web.
- A turn stopped for repeating a call answers from what it has; the debug trace records `repeated_call` only for real repeats.
- A text-only model is known as one before its first image is rejected; the web model picker accepts catalogue models on endpoints no provider names; a failed vision read is tried once per turn.
- The terminal streams styled text with far fewer bytes, and a running tool call's raw detail is paged.

## v1.4.0 - 2026-10-15

### Upgrade notes

- **Spaces are now called workspaces; nothing moves.** The folder is still `~/.uagent/spaces/`.
- **The coordinator is now the panel, and its lead voice the moderator.** `uagent coord` and `/coord` are `uagent panel` and `/panel`; settings are `panel.*` and variables `UAGENT_PANEL_...` (`UAGENT_COORDINATOR_...` is no longer read); instructions are `PANEL.md` (`COORDINATOR.md` is still read where no `PANEL.md` exists).
- **Web: workspaces and folders are listed apart, and a place's name is its panel.** Each place is a header with members' avatars, **+** for a new conversation and a menu; hovering a member shows its card with **Ask**, **Full prompt** and rename/remove.
- **A workspace has its own name and can be renamed and deleted.** Deleting removes its conversations and its folder with every file.
- **A chat member is called what its conversation is.** Renaming the conversation renames the member and tells the others.
- **Text files from versions before 1.3.1 are no longer read** (`~/.uagent/.config`, a project's `.uagent/.config`, `model-preference.json`). Start 1.3.1 once first, or set the values again with `/config`.
- **The settings file names settings in words and holds typed values.** `settings.json` is rewritten to `{"format": 2, "all": {...}, "projects": {...}}` on first read, with `settings.schema.json` beside it; `uagent config import` takes either format. Invalid names or values are reported.
- **`UAGENT_SUBAGENT_MODEL`, `--subagent-model` and `UAGENT_COORDINATOR_MODEL` are gone.** Subagents and threads run on the starting conversation's model unless the call names one.
- **`OPENROUTER_MODEL` and `OPENROUTER_EFFORT` are no longer read.** Use `UAGENT_MODEL` and `UAGENT_REASONING_EFFORT`.

### Added

- `/skills import` and `/memory import` import Claude Code and Codex skills and memories (also in the Library's Import button); `$import` condenses Codex's memory summary.
- `browser`: `open` takes `path` to show a self-contained `.html` or `.svg` from the workspace.
- Web: a bare name in the folder field opens a space under `~/.uagent/spaces/<name>`, created on first use.
- `browser`: `type` fills a field in one call (`x`, `y`, `view_id`, `replace`, `key`); `open` with `new_tab`, `tabs` with `close`; `press` knows arrow keys, Delete, Space, PageUp, PageDown, Home and End.
- `browser`: `read` returns text from an offset or only matching lines and links; `open` accepts the same; `click` and `type` take an element number.
- `scratch` takes `code` and writes and runs the script in one call.
- A coordinator's conversation is a chat: `thread` with `add_member` brings in named members with a persona and optional model, `delete` removes one, and "Ada, ..." asks one alone. Each participant gets `coordinator.chatTurns` (3) turns per message. The web shows avatars, typing, author names and "Show prompt".

### Changed

- The model catalogue is cached under `~/.uagent/models/` for a day, so the first screen no longer waits for it.
- A session can no longer end up with two runtimes; `/restart` waits for the runtime to exit and says if it did not.
- Mail that wakes a session survives the session being killed; duplicate reports are delivered.
- Usage, cost and budgets include what a failed model call was billed.
- Settings: variable references are validated, a conflicting save is refused with "try again", providers are edited as one block each with keys never shown, and "Show the file" hides secrets.
- `uagent coord -p` says on standard error why a run ended without an answer.
- `subagent`: an empty `name`, `description` or `directive` counts as not given.
- Commands, scratch scripts and MCP servers are no longer told uagent's own settings.
- A turn stopped by its step, tool-call or time limit answers once more from what it has; `-p` prints it and exits 0.
- Web and terminal: a running turn's time is counted ("Working - 42s") and minimal detail shows how long a finished turn took.
- `browser`: actions return the page as numbered text without a screenshot (`observe` still returns one), and results carry up to 12,000 characters of text with how to read on.
- The approval for a command outside the sandbox says it can reach saved logins, browser sessions and settings.

### Fixed

- `browser`: a second browser service on one machine starts on a free X display; a user-help request survives while its conversation runs; Enter, Backspace and line breaks work.
- `memory`: `list` and `search` accept empty `key` and `content`.
- `subagent`: `limits.seconds` stops background children; stopping a subagent stops what it started and counts its spend.
- A new conversation is titled by its own model on endpoints that cannot take the title model's name.
- `run`: long output keeps its beginning and end.
- `activity`: `wait` accepts the id of a detached command.
- Web: a shared file stays visible at the `minimal` level.
- `scratch`: a result names what the run left in the workspace root.
- Sandbox: a workspace that cannot be granted (such as the home directory) is reported at startup, and a refused write names the writable roots.

## v1.3.1 - 2026-10-06

### Fixed

- Keys typed ahead are no longer lost when the terminal redraws the conversation, as `/verbosity` does.

## v1.3.0 - 2026-10-06

### Upgrade notes

- **Settings are saved by µAgent, not in text files you edit.** They live in `~/.uagent/config/settings.json`, changed with `/config`, the web's Settings or the `uagent` tool, and moved with `uagent config export` and `uagent config import FILE`. `~/.uagent/.config` is imported once; a project's `.uagent/.config` is imported if it matches what was trusted or with `--trust-project-config`. `UAGENT_CONFIG_FILE` is gone.
- **Yolo no longer turns the command sandbox off.** Setups that relied on `--yolo` or `UAGENT_APPROVAL=yolo` for unconfined commands also need `UAGENT_SANDBOX=0`.
- A subagent runs commands under its delegating conversation's sandbox, and a coordinator's thread cannot be put in yolo.
- A scheduled task's `prompt` permission is now `ask` (a saved `prompt` still asks), and a run keeps its task's mode after a host restart.
- `/model`, `/effort` and `/variant` apply only to the conversation they are typed in. For new conversations use `/model X --default`, the web's "Also use for new conversations", or `UAGENT_MODEL`.
- A conversation that never chose a model follows the configured one.
- `/config` names the scope a value comes from (All conversations, This project, This conversation, Environment, Command line).
- Removed: `/verbose` (use `/verbosity full`), `UAGENT_STEERING`, `UAGENT_OPENROUTER_FALLBACKS` (a pinned provider no longer falls back), `UAGENT_WEB_SEARCH_EFFORT` (use `UAGENT_WEB_SEARCH_MODEL=vendor/model:low`) and `UAGENT_PRUNE_SUPERSEDED_READS`.
- Tool parameters simplified: `run` and `scratch` lose the optional `description`, and `session` takes one `session_id`.
- Removed: `/link` and `/peers` and token links between sessions. Web: typed `/quit` no longer closes the runtime; use Close session.
- `UAGENT_FIRST_EVENT_TIMEOUT` and `UAGENT_STREAM_IDLE_TIMEOUT` merged into `UAGENT_STREAM_TIMEOUT` (300 s).
- Now fixed at their old defaults: `UAGENT_TOOL_CONCURRENCY`, `UAGENT_MAX_BACKGROUND_JOBS`, `UAGENT_MCP_STARTUP_GRACE`, `UAGENT_MEMORY_IDLE_SECONDS`, `UAGENT_WEB_SEARCH_ENGINE`, `UAGENT_WEB_SEARCH_CONTEXT_SIZE`, `UAGENT_IMAGE_DETAIL`.
- Set per provider now: `UAGENT_WIRE_API`, `UAGENT_HOSTED_TOOLS`, `UAGENT_MODEL_FEATURES` and `UAGENT_PROVIDER_PROTOCOL` are the `wire_api`, `hosted_tools`, `features` and `protocol` keys in `UAGENT_PROVIDERS`; a `UAGENT_BASE_URL` endpoint speaks Chat Completions only.
- No longer settings: `UAGENT_TOOLSET`, `UAGENT_HEADLESS_PROGRESS`, `UAGENT_USAGE_FILE`, `UAGENT_AUTO_COMPACT_TOKENS`, `UAGENT_PROMPT_OVERLAY` (use `--prompt-overlay` in evals). A config file cannot set `UAGENT_INTERNAL_*`.

### Added

- Verbosity: `UAGENT_VERBOSITY` (`minimal`, `default`, `full`) for terminal and web, changed with `/verbosity LEVEL` or Detail in the web menu; `--verbosity` pins one terminal.
- Web: tool calls fold into groups as in the terminal; the conversation menu exports, compacts and restarts (`/restart`).
- `uagent --plain` (`UAGENT_PLAIN=1`) for screen readers and `UAGENT_REDUCED_MOTION=1`; see docs/ACCESSIBILITY.md. The web app meets WCAG 2.2 AA.
- Undo: `/changes` lists files the last turn's edit, write and delete tools changed, and `/undo [FILE]` restores them; shell changes are not tracked.
- Approvals name their risks (runs commands, makes changes, uses the network, outside this folder).
- "Queue next": a steer sent with `queue: true` runs as its own turn after the running one; the web has it as Alt+Enter.
- `uagent --web` prints a pairing link; the web pairs from `#pair=<code>`.
- Terminal: the `/` menu with fuzzy matching and "did you mean" hints, and a status row with route, approval mode, context left and cost.
- Web: one "N need you" count across folders, shown in the tab title and app badge, with answers in place.
- Web: command palette (Ctrl/⌘+K), `?` for shortcuts, Alt+Up/Down to step through conversations.
- Web: after a stopped turn the status line shows **Stopped** with **Continue**.
- Web: the composer keeps one shape while idle, running, stopped and waiting on a decision.
- Web: Settings, Display has **Animations** (Off or System).
- Web: a turn that changed files ends with a receipt that opens the files with **Undo** and **Undo all**.

### Changed

- Settings have one scope model in terminal and web: All conversations, This project, Environment and command line, and This conversation (`/config conversation KEY=VALUE`).
- A session's runtime stops after 15 minutes idle (coordinators too, was 10) and restarts on the next message or setting change; the web composer stays, with no Resume step.
- `ask` takes up to 8 questions and headers up to 24 characters; the web answers a page at a time.
- Web: every dialog closes on a tap outside it; a reconnecting client reads its backlog in batches.
- A changed setting applies from the next message; a restart is asked only for route, credentials, sandbox, tool set, memory and MCP.
- Web: the `/` list omits commands that have their own control (`/attach`, `/model`, `/effort`, `/yolo` and others); they still run when typed. `/tell` is gone.
- Web: Settings is arranged as All conversations, This project, This browser and Host, with **Reset all to defaults** and **Remove all overrides**; settings have plain names, also printed by `/config` and `/debug-config`.
- Web: coordinator threads nest under their folder, approvals are cards with **Deny**, **Allow for session** and **Allow once**, failed sends show **Retry**, YOLO mode is red, and the light theme's diff colors meet WCAG AA.

### Fixed

- Starting a stopped conversation's runtime reports why it failed at once, queues commands that arrive during the start, and stops a silent runtime.
- A model's malformed tool call or other fault (empty response, tool markup in prose, call without a name) is answered with a correctable error instead of ending the turn.
- `scratch` runs any script in its folder; a rejected enum value names the allowed ones.
- Browser: only the agent's tab stays open when a turn ends, idle Chrome is not kept running by watching, and the current tab is remembered across Chrome starts.
- A coordinator and its threads can message each other with the `session` tool, and a thread's finished turn reaches its coordinator with the first 1,500 characters of its answer.
- Web: messages from other sessions show as event rows; file diffs are shown whole; a command killed by `pkill -f` says so; Safari focus, banner and mention-list fixes.
- Stream errors sent as a bare string keep their text on the Responses and Anthropic dialects; the `session` tool lists sessions without a prompt.
- Data races found by ThreadSanitizer were fixed.


## v1.2.0 - 2026-09-30

### Upgrade notes

- Messages between sessions and subagents now live in `~/.uagent/mail/`. Guidance still waiting in the old `~/.uagent/sessions/inbox/` is not delivered.
- A coordinator's `thread delete` closes a running session first; `thread stop` still only interrupts its turn.
- `system-prompt.json`, `/prompt` and the `prompt` control kind are gone; use `AGENTS.md` and the Instructions screen instead.
- 48 internal limits are no longer settings and the old names are not read; only `UAGENT_HISTORY_DAYS` remains for retention. The child delegation depth variable is `UAGENT_INTERNAL_DEPTH`.
- Persistent subagents (`persistent=true`), agent teams (`UAGENT_TEAM`, `broadcast`, `hops`) and the `guide` session command are removed. `~/.uagent/collaborators/` is unused and can be deleted.

### Added

- Coordinator per folder (`uagent coord`, `uagent coord -p`, folder icon in the web): delegates work to sandboxed threads within a thread cap and daily spend limit, and settles approvals Auto mode cannot.
- `/coord`, `/board` and `/open` reach the coordinator from any terminal session.
- `ask` tool: the agent asks you 1 to 4 questions with options; answer by choice, text or image. A thread's questions go to its coordinator first.
- `ask` options can include an image or a monospace preview, shown as cards in the web.
- Image viewer zoom: -, +, Fit, Cmd/Ctrl+wheel, keyboard shortcuts and pinch. Annotation zooms the same way.
- Slash commands with a screen (`/config`, `/permissions`, `/mcp`, `/tools`, `/rename`, `/memory`, `/skills`, `/schedule`) open it in the web UI when typed without an argument.

### Changed

- Sessions, coordinators, threads and subagents message each other through durable mailboxes that wake the recipient at once; floods, loops and oversized messages are refused with a reason.
- Web panels open as sheets from the edge (from below on a phone) with the same focus, Escape and back behaviour as dialogs.
- Instructions replace system-prompt documents: sessions read `AGENTS.md` (yours in `~/.uagent/`, then the project's), coordinators also `COORDINATOR.md`. Edit them on the Instructions screen, with `/instructions`, or via `uagent set_instructions` with approval.
- Rewind no longer loses work: **Edit from here** and **Fork from here** in a message's menu, and `/rewind N` does the same. The original conversation and files on disk are unchanged.
- Delegated children are ordinary saved sessions in the workspace history, hidden from the session list.

### Fixed

- A coordinator can delete sessions; a socket left by a crashed runtime no longer blocks deletion.
- Phone: the sessions drawer slides in again and sheets no longer sit off the edge at small zoom.
- The web session list updates without refresh when a coordinator creates or deletes sessions.
- Files the agent attached mid-turn no longer count as your messages, so `/fork N`, `/rewind N` and `/share` numbers line up.
- Mouse-wheel scrolling in WebKit browsers is no longer swallowed when content above changes size.
- Reloading the web page during a streaming answer no longer freezes it, and a running tool keeps its row.
- Tall images fit the viewer; `/sessions` in a wide window focuses search.

## v1.1.1 - 2026-09-28

### Fixed

- The web host restarting itself after a settings change no longer races its own stop signal. v1.1.0 was tagged but not published because of this.

## v1.1.0 - 2026-09-27

### Upgrade notes

- `UAGENT_APPROVAL` accepts `ask`, `auto` or `yolo`; the old `prompt` spelling and the legacy integer permission override are no longer read.
- The web host also reads `UAGENT_WEB_BIND`, `UAGENT_BROWSER_DATA` and `UAGENT_BROWSER_IDLE_MINUTES` from the config file.
- Running sessions restart on upgrade and installed web apps must reload.
- An empty setting (`KEY=`) now means "use the default" everywhere.
- `scratch` no longer takes `code` or `packages`: write the script under `.uagent/scratch` with `write_file` (dependencies in a PEP 723 header) and run it with `scratch`.

### Added

- Image annotation in the web viewer: pen strokes and numbered pins with notes, attached as a marked-up copy.
- Uniform settings: values show as text with **Reset** when changed, environment-set values are locked, **Reset all to defaults** keeps API keys, and changes needing a restart offer one.
- Terminal parity: `/config` lists changed settings and resets a scope, `/restart`, `/mcp [retry|on|off NAME]`, `/rename TITLE`, `/permissions rules` and `forget`.
- Settings → **MCP servers**: state, tools and failure reason per server, with retry and on/off switch.
- Chrome stops after `UAGENT_BROWSER_IDLE_MINUTES` (default 15) without browser work and restarts on the next action.
- Tool rows read as a headline with a status dot; diffs, last output lines and links stay visible. Consecutive calls of one intent fold into Explored, Researched, Verified or Edited.
- Shared files preview inline: images, sandboxed HTML and PDFs on desktop.
- Attachments look the same everywhere: image thumbnails open a viewer, other files show as cards with type and size.
- Closing the browser hands control back to the agent, and the browser can be watched without taking control.
- Web Push is built into release archives and the Docker image.
- Timestamps follow per-device settings: 12-hour, 24-hour or locale clock; smart, relative or absolute stamps.
- The browser performs `/rewind` and `/share`, recalls prompts with Up/Down, stops a turn with Esc and backgrounds a command from the activity panel.
- `$fusion` coordinates one opt-in retained sidekick through blocking handoffs; usage is charged to the lead.
- Terminal and web clients share one session runtime; closing a client or restarting the web host detaches without stopping the session.
- `scratch` accepts script arguments; `grep` supports literal searches and listing only matching file names.
- Image attachments accept HEIC/HEIF (converted to JPEG) and SVG (rasterized to PNG); unconvertible files fail with a clear error.
- New sessions are titled in the background from the first prompt; a rename always wins.
- A work button under the input counts running commands and sidekicks and opens their list.
- Settings open as sidebar sections (General, Models, Permissions, Agent, Devices, Advanced, Developer); phones drill in from a list.
- Web conversations and drafts are cached in IndexedDB for offline reading, with pinned downloads and cleanup on logout.
- Mermaid fences render locally.

### Changed

- The agent can use the browser's saved logins but never read them: with the sandbox on, commands and file tools cannot reach the browser data; screenshots mask password fields; `deploy/chrome-policy.json` blocks Chrome's password pages, `view-source:` and DevTools.
- Dialogs, sheets, popovers and toasts share motion tokens, and the system back gesture closes the topmost one. Settings is a full-screen sheet on phones.
- On phones the browser opens as one full-screen sheet with touch control (tap, drag, hold-drag, two-finger right-click, pinch) and **Take over**/**Done**.
- Browser actions wait for the page to settle, return a fresh screenshot, follow new tabs, add `back`, and flag suspected bot checks; screenshot pixels match click coordinates after scrolling.
- Phone loading and resuming keep the layout still; Return on the phone keyboard writes a new line.
- Approvals ask one plain question; the terminal status row shows elapsed time, hints, running agents, context left and approval mode.
- `/verbose` is a terminal view setting; full tool output is always recorded.
- `-DUAGENT_WEB=OFF` builds leave out the browser.
- Web steering queues guidance without aborting the turn. One status LED replaces the presence dot and activity glyphs; Markdown tables scroll.
- `read_path` loads media, replacing the model-facing `attach` tool; `uagent` combines inspection and approved configuration.
- Memory changes have visible receipts in web history; scheduled runs reconnect after a web restart without resubmitting prompts.
- One agent step renders one web card; mobile rotation keeps layout scale and respects safe areas.
- The conversation is compact, with the status line above the input and totals below it; an expanded tool row is one column and failed calls read red.
- Attachments are prepared per model capability with explicit delivery labels.
- `write_file` creates new files by default; replacing needs `overwrite=true`.
- MCP discovery, subscriptions and input continuations are complete.
- Bundled skills install only under the selected prefix; user-owned skills and configuration are preserved. Custom routes use `UAGENT_PROVIDER_PROTOCOL`.

### Removed

- `UAGENT_TITLE_MODEL` (documented but never read), `/trace` (headless `--json` keeps its `trace` field), `show_image` with terminal image rendering, the self-improvement controller and skill, and the GitHub `action.yml`.
- `UAGENT_WEB_SEARCH_URL` and `UAGENT_WEB_SEARCH_API_KEY`: web search uses the side-model route. `anthropic` remains only as a protocol alias.

### Fixed

- `/share` failed in every live session.
- Audio and video attachments are accepted only on Chat Completions routes; other routes report them as unsupported.
- `permissions.json` is protected like other config files, MCP servers no longer inherit a file-size limit, and non-UTF-8 locales fall back to UTF-8 or ASCII.
- Configuration edits no longer get overwritten by a newly typed value during initial rendering.
- Scratch output shows as an ordinary tool result; truncated whole-file diffs are marked.
- The second Ctrl+C exits reliably and restores terminal modes.
- Unsent drafts survive a reload or an evicted installed app; failed reconnects retry with backoff.
- `/http INDEX` keeps every exchange of the turn; diff shading reaches the end of long lines.
- The installed iOS app draws under a dark status bar and has a favicon.
- Reloading the web UI no longer shifts the layout, and saved conversations show **Resume** from the start.

## v1.0.0 - 2026-09-03

### Upgrade notes

- MCP break: the `initialize` / `2025-11-25` lifecycle is replaced by `server/discover` negotiation for `2026-07-28`, and a required server that cannot initialize now aborts startup.
- Session format 2 is no longer resumed.
- Yolo mode (`--yolo`, `UAGENT_APPROVAL=yolo`, `/yolo`) now also disables the OS sandbox.
- The text-protocol fallback is removed: a route that rejects native tool calls fails with the provider's message.
- The `subagent` tool's `max_steps`, `max_tool_calls`, `max_seconds`, `max_cost` and `memory` arguments became one `limits` object.

### Added

- Hosted web search reports one lifecycle across OpenAI Responses and Anthropic Messages; the terminal shows `searching the web` and `--json-stream` emits `response.hosted_tool` events.
- `scratch` runs `.sh` scripts as well as `.py`, chosen by extension; shell scripts stay inside the OS sandbox.
- Tab after `@` completes a path one segment at a time.
- Ctrl+X Ctrl+E opens the draft in `$VISUAL` or `$EDITOR`.

### Changed

- The memory index shows an opening clause for each global memory, and the always-on slice admits the smallest global memories first.
- `scratch` is advertised only when `uv` or `python3` is on PATH.
- `UAGENT_MAX_TURN_TOKENS` and `UAGENT_SESSION_TOKEN_BUDGET` (and `--token-budget`) cap generated tokens, include side-route usage, persist across resume and appear in headless `stop` metadata.
- Collaborator sessions use the existing debug age/count retention; failed children remain resumable.
- A newline in the same input batch as a bracketed paste is inserted instead of submitting.
- Release archives no longer bundle the `find-skills` skill.
- `self-improve` runs one bounded personal prompt-overlay experiment and emits human-approved configuration proposals.

### Fixed

- Confined commands no longer fail with `cannot spawn shell` on Linux when the agent is not started by its own startup path.
- MCP launch no longer risks allocator-lock deadlocks in the child process.

## Before v1.0.0

Releases v0.4.0 to v0.9.0 are in the git tags: <https://github.com/timongentzsch/uAgent/tags>.
