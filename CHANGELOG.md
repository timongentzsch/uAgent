# Changelog

## v1.9.0 - 2026-11-11

### Changes

- **One box starts a conversation, and a phone gets a home screen of its own.** The home screen's field, its buttons and **Other place…** are one box: what to ask, then where it starts (**Scratch**, which opens the folder browser), which model it starts with, and the button that starts it. It lists the conversations with a place suggested, each with **Move**. On a narrow screen the box is docked at the bottom, as a conversation's is, above it the latest conversations from every place with **All** to the whole list; the heading and the keyboard tips are left out. In that list a title takes the whole row with its time under it, a row's menu opens on a long press instead of a button, Search and New sit in the sheet's header, and Library and Scheduled sit under the list, at every width.
- **Choosing a folder is a file browser, and pages wait until they are whole.** The folder in **New conversation** is browsed, not typed: the path is a row of crumbs that each lead back, the folders in it are one list (a click chooses one, its arrow or a double click goes in), recent folders on that host lead the home view, and dot folders show once **Hidden folders** is on. The pencil types a path. While a folder loads, here and in the Files tree, its rows show in outline. A slide, a document or a sheet shows once its fonts, pictures and fetched data are in (five seconds at most), with its outline standing in until then, so its text is not set twice.
- **A remote host says which µAgent it runs.** A host's details name its version, which is always the main's own build, and the host reads **Updating…** while a new main copies its executable there on the next connection. A restarted main connects again the hosts that were connected when it stopped, so an update reaches them unasked, and a build no main has used for 30 days is removed from the host.
- **The transcript says what was done, and starts quieter.** A turn's work line counts its steps by kind ("Worked 3 reads, 1 search, 2 edits, 1 command, 2 notes") instead of "7 steps", and a run of reading, searching and fetching is one row, "Explored 3 files, 1 search". A new install starts at `minimal`: your message, that line, the answer. What the agent says between calls shows as it says it at every level and folds into the line at `minimal` once the turn has ended. Thinking is its own switch, off at first (`thinking`, `UAGENT_THINKING`): on, a thought is text among the steps, fainter than a reply and in italics: its first two lines where a work is open (the rest on **More**, or at full), and the latest one on a line under a folded work while it runs. On the web the level and the switch sit in the conversation's header; in the terminal Ctrl+O steps through the levels.
- **Start first, file later.** The home screen is a field: type and the conversation starts in Scratch, with no place to pick; **New** in the sidebar opens it. µAgent then suggests where the conversation belongs. With your first message, the title's model also sees your recent conversations and may offer to continue one of them or to start over in its folder, while the first turn has changed no file. When a turn ends, files it changed in a folder you already work in name that folder; otherwise the model is asked once, and may name a place, two, or a new workspace. A suggestion shows above the input, under **To file** on the home screen and in the sidebar, with **Move**, **Other place…** and **Keep in Scratch**. Nothing moves by itself, and `title.model = off` turns the model's part off.
- **Memory is lighter, and the agent saves what you tell it.** The agent now saves a memory when you correct it or state a standing preference, not only when asked, and merges into a note that already covers the topic. The background extractor is one model request instead of a child process with its own tools, and records a line only when it saved or failed. A conversation waiting in Scratch is read after it is filed, so its lesson lands with the right folder. When a scope is three-quarters full, an extraction may fold one overlapping note into the one it writes. The index lists the newest notes first, with a line for Claude Code's and Codex's notes too.
- **Free-model setup.** Setup can select a current OpenRouter model with tool support and zero advertised prices, using a separate provider entry and API key. The preset excludes paid models, hosted search and PDF parsing, sends zero price caps, and becomes the default only when selected. Provider accounts, rate limits and data policies still apply.
- **Complete remote bundles.** Release and source installs verify matching remote payloads for Linux and macOS on ARM64 and x86_64 before replacing the main binary. Source fallback builds the selected release tag. Unpublished developer builds need a matching bundle or explicit `UAGENT_LOCAL_ONLY=1`. macOS x86_64 is now a native release target.
- **Remote folders use the main's configuration.** Add an SSH host and choose it when starting a conversation or opening a folder. The main keeps providers, model keys, history, memory and approvals; the remote gateway runs files, Git, commands and terminals. Independent mains share one installed executable. The connection guide documents supported platforms, reconnect behavior and first-version limits.
- **Hosts are visible where they matter.** Conversations are grouped by host, with pins and collapsible Recent. The home screen offers New conversation; creation reuses the Files tree with a Host selector. The bottom Remote hosts panel reports connection status and measured SSH round trip, with reconnect/disconnect controls. Selection controls are square with phone touch targets; mobile message metadata sits below the text.
- **Pick several, act once.** In the sidebar, Ctrl-click (⌘-click) picks conversations and workspaces without opening them and Shift-click adds a run, across places; on a phone, **Select** in a row's menu starts picking with taps. The footer becomes a bar to move, close or delete them all, after one confirmation that names what goes and keeps picked whatever could not go, with the reason. The Library picks the same way across kinds and places and deletes memories, skills, designs, decks, docs, design systems and shared files together; the pane beside says what each kind loses. Ctrl+A (⌘A), ⌫ and Esc work while picking, wherever the focus is.
- **About, with updates.** Settings has an About page: the version, how µAgent was installed, and **Check for updates**, which has the host ask GitHub for the newest release and shows its notes. An install made by the installer updates from there: the host runs that release's installer, which replaces the files only, and restarts itself, while conversations keep running and restart once idle. A container, a checkout and an install the host cannot write get the command to run instead. Only a person starts an update, and not where the terminal is turned off. `UAGENT_WEB_UPDATE_CHECK` (off by default) lets the host ask once a day. The installer now records how it installed, and swaps a release's files in whole, so a copy that fails leaves the old install.
- **Only you write a skill of yours.** `write_file` into your own skill folders (`~/.uagent/skills`, `~/.agents/skills` and the Claude Code and Codex ones µAgent reads) always asks a person, in every approval mode, as changing your instructions does. Before, yolo let the agent write a skill that every later conversation read.
- **Sheets.** Ask for a budget, a tracker or a table and the agent writes a sheet: one HTML page whose tables are the tabs of a workbook, each cell holding what you would type. Files shows it as a spreadsheet and calculates it in the page with the [IronCalc](https://www.ironcalc.com) engine, which is installed beside µAgent and never part of the app. You edit it there as in any spreadsheet: type, paste, fill down, undo, sort, filter, insert and delete rows and columns; each change is saved into the file, where only that tab's table is rewritten, unless the agent changed the file meanwhile. A selection shows what its numbers come to, **Comment** adds its range to your message, and cells in error are listed for the agent to fix. The agent's own check of a sheet lists the same errors. The agent can add rules that fill, embolden or strike what meets a condition, and charts (bars, stacked bars, lines, a pie) that follow their cells; **Chart** adds one from a selection. An `.xlsx` or `.csv` file opens in the same grid, read-only, and **Save as sheet** makes it a sheet; every sheet exports as an Excel workbook or as CSV.
- **Documents and decks open in a viewer.** The `artifact` skill writes slide decks (a folder of `1920x1080 slide` pages, one per slide, with speaker notes in an `<aside>`) and documents on paper (one page with `print` `flow a4`), on your design system, 16:9 and A4 unless you ask for another size. Either opens as one page with a legend beside it, or under it in a narrow pane: a deck's slides as numbered thumbnails, a document's pages marked on a small picture of it. A counter, the arrow keys and Page Up and Down step through it, and **Find** looks through every slide or the whole document. Drag a slide in the legend to move it, or duplicate or delete it from **Slide actions**; the files are renamed to keep the order. A deck presents over the whole app, phones included, with the notes and a **Presenter view** window that keeps step with it. A document shows where its pages break, takes your typing in place (each block saved to the file unless the agent changed it meanwhile) and downloads as Markdown. Both export a PDF printed by the agent's browser on the page's own paper, a deck one sheet a slide. The self-check also reports text running over text on a slide and a document block taller than a page.
- **The browser is lighter.** Chrome keeps no earlier pages alive for Back and does not look for casting devices: after ten page visits it held 4 renderers and 333 MB instead of 10 and 446 MB. A research browser's Chrome stops when its conversation releases it instead of idling for 15 minutes. Watching streams about two-thirds fewer bytes while a page scrolls; driving keeps the sharper picture. The `browser` tool's description is 14% shorter, about 135 tokens less with every request, without losing a task in an A/B run.
- **The terminal panel is one row of tabs.** Each terminal is a tab naming its folder, with **+**, the menu and hide beside them, in place of a title row over a second row of tabs. Files no longer has its own terminal button; the header's opens the panel.
- **The browser can draw pages at a laptop, desktop, tablet or phone size.** **Page size** in the browser's status menu applies Chrome's device mode to the agent's tabs, fitted into the window, so a mobile layout can be checked by you and the agent alike. While you only watch, your own pointer stays visible; Chrome's takes over when you drive.
- **Files is an explorer tree.** Folders open in place with indent guides, as in VS Code's Explorer; the arrow keys walk the tree, the open folders are kept per folder, and a folder that is a deck or a design offers **Open** on its row. An opened page or picture zooms as the canvas does, from its controls or a pinch over it, in Safari too.
- **The Library shows everything, from every place.** Memories, skills, designs, design systems and files the agent shared are one list, grouped by folder and workspace as the sidebar groups conversations, with a rail to filter by kind and place and a search that reads what an item says. A memory opens beside the list and can move to another place or be copied to every place; a design shows its pages and opens its canvas; a shared file opens, downloads or leads back to its conversation. On a phone the kinds are chips and the place is a field.
- **Designs you see live beside the chat.** A folder of design pages opens as a canvas in the Files pane, with the pictures, films, documents and decks it holds beside them: every page runs live in a sandbox that reaches neither the app nor its API, rows group by name, a page the agent writes reloads at once, and one that fails offers its errors to the agent. The new `artifact` skill teaches the agent the formats, the craft rules and how to build a design system from your code, with its brand book: A4 pages the agent art-directs in the system's own voice, which read like a PDF on any screen and print to one. What the agent hands over with the `present` tool, which replaces `artifact`, shows under its reply as a card: an artifact with its cover live, what it is and how much it holds, any other file as a download. Pages are named by step, screen and state (`02-sign-in--error.html`): the step is the canvas row, so a screen's variants and states sit side by side. `write_file` can copy a file you attached into the folder (`from`), so a design can use your logo or photos.
- **Arrivals glide, as in a messenger.** While you follow a reply, the view eases to new content instead of jumping, a new step, message or paragraph fades in where it lands, and your sent message rises from the input. Jump to latest glides down. Any scroll, key or touch takes over at once; reduced motion keeps everything instant. The three durations are tokens (`--motion-glide`, `--motion-arrive`, `--motion-send`), so your own stylesheet can slow them, speed them up or turn them off with `0ms`.

- **Sending feels settled.** Your message keeps one row from the moment you send it instead of flickering once; "Working" and its clock live only in the status line under the input, while the turn's row counts its steps; on a phone that line opens and closes smoothly instead of snapping the transcript.
- **What appears also leaves.** Banners, toasts, Jump to latest and the docked Files pane fade out the way they came, cannot be clicked while they go, and hand focus back (to the input, or to what opened the pane). Settings, the Files and Library lists, activity and question pages push in one way and back the other. A leaving dialog can no longer take a second tap.
- **One touch size, one hover, one selection.** Small controls take a 44px touch; a filled control's press is distinct from its hover; a selected item is the stripe and fill everywhere, a pressed toggle the accent. Approval prompts wrap instead of cutting off why they need you, and the phone Files sheet has one header.
- **The web's stylesheets are layered.** Tokens, element defaults and shared pieces sit in cascade layers, so a stylesheet of your own outranks them all, and the narrow-column layout follows the app's text size.

- **Restyle the web interface from `~/.uagent/config/style.css`.** The sheet overrides the design tokens (colours, spacing, type, motion) and applies on every paired device without a new build. Ask the agent to change the look: it shows the exact sheet for you to approve, and open pages apply it when the turn ends. The `uagent` tool shows the sheet (`show`, topic `style`) and replaces it (`write`).
- **One `uagent` tool configures µAgent, and comes with its skill.** `uagent` takes over `uagent_configure` and `uagent_instructions`: `show` reads this build (settings, commands, the prompt, MCP servers, permission rules, skills), `set` changes settings, `write` replaces an instructions file, the stylesheet, a system-message section, the folder's `.mcp.json` or a skill, and `command` runs a slash command such as `/model` or `/yolo` when the turn ends. Every change shows exactly what it does and waits for you, whatever the approval mode. The tool is offered once the `uagent-config` skill is open, by the model or with `$uagent-config`, and stays for the rest of the conversation; a conversation that never configures anything sends up to about 670 fewer tokens with every request.
- **Scratch work is `write_file` and `run`.** The `scratch` tool is gone. The system message's Scratch section tells the agent to write a one-off script under `.uagent/scratch` and start it with `run`, Python through `uv run` with its packages named in the script; an approval for such a `run` shows the script. Each section of the system message can be edited in Settings, with Reset, and is saved in `~/.uagent/config/prompt.json`.

- **One browser call can run several steps.** `browser` takes `action: batch` with up to 20 ordered `steps` (click, type, press, scroll, drag) on the page of one `view_id`. It stops at the first step that fails or meets a dialog and returns the page once. A model that took one round per form field now fills a form in one call.
- **Browser results are leaner and show values.** An action's result compares only the page itself with the last one: the "done" lines and the view id sit outside the comparison, line numbers are gone, and an unchanged page reads "No change on the page." A filled field shows the start of what it holds, except a password, a one-time code or a card field.
- **The playwright-cli browser skill is gone.** The agent's browser is the web host's. Where the host has no browser service (macOS outside the container, or Linux without Chrome and TigerVNC), the agent reads pages with `web_fetch`.
- **A turn says what started it.** A turn started by an agent's report, a message or finished background work is recorded as such, not as the person's. In the web, an agent's report reads "Agent "title" finished" with a button that opens it, and finished background work reads as one line instead of raw harness text.
- **Deleting a conversation deletes its agents.** Its agents and theirs go with it, unless an agent's worktree holds work that would be lost; then nothing is deleted and the agent is named.
- **The list shows agents only while they work.** A conversation's agents that are working or waiting on you sit beneath it on a thin line, with their titles at nearly full width. A finished agent leaves the list; its report in the conversation opens it, and search finds it. The "N agents" fold is gone.
- **Empty conversations stay out of the list** until something is said in them, unless one is open or working.
- **`run` says a background result starts the next turn.** Models told people they had no way to report back and held turns open on waits; `run` now says to end the turn and how to check back later.
- **OpenRouter is a provider like any other.** Every model is reached through a provider and named `provider/model`. The `openrouter.apiKey`, `endpoint.baseUrl` and `endpoint.apiKey` settings and the `OPENROUTER_API_KEY`, `UAGENT_BASE_URL` and `UAGENT_API_KEY` variables are gone, as is the endpoint a start fell back to without one. Move a saved OpenRouter key into a provider: `"openrouter": {"base_url": "https://openrouter.ai/api/v1", "api_key": "$OPENROUTER_API_KEY"}`, which `uagent setup` now writes. What a known host such as openrouter.ai supports still comes from its address, whatever the provider is called.
- **Providers in Settings start from a preset.** Add a provider starts from OpenRouter, OpenAI, Anthropic or Ollama; with none yet, the list offers them. Each row says where its key comes from, and the model picker lists models under their provider and names the providers that sent no list.
- **The agent can schedule tasks.** A `schedule` tool lists, saves, pauses, resumes, deletes and runs scheduled tasks. A task it saves runs in the conversation's folder on its model, with no wider approval mode than the conversation's. Before, an agent asked to schedule something had no way to, and argued against it.
- **A scheduled task can check before it asks a model.** A task's optional `check` is a shell command run first in its folder: exit 0 starts the run with what it printed, exit 1 records "nothing new" without a model call. Frequent polling, such as a stock alert every two minutes, now costs a model call only when something changed.
- **Each call is one row.** Calls no longer fold into "Explored", "Researched" or "Edited" groups; "Worked N steps" is the only fold. They covered about one call in eight, and a `run` row now says what it does on its own.
- **The prompt names the `browser` tool.** Where it is offered, the prompt and `web_fetch` point to it for pages built by scripts, behind a login or cut off, instead of to a skill, and a fetched page says it may be partial.
- **The Context value opens the raw context.** In a conversation's statistics, the context line itself leads to the raw context, in place of a separate button under it.
- **A model without a chosen effort uses its provider's default.** When a provider's model list declares a default reasoning effort (`default_parameters.reasoning_effort`), a model with no effort set now sends it, as one picked from the model list already did. Before, the request named none, and some endpoints then reasoned longer than the default they list: on GPT-6.1-Sol a coding task took 24% longer.
- **The to-do list is pending or done.** The `in_progress` status is gone; the first item not done is the current step. Marking each item in progress and then done took a model round of its own each time, about a quarter of GPT-6.1-Sol's rounds.
- **Every build has the web interface and the browser.** The `UAGENT_WEB` and `UAGENT_BROWSER` build options are gone, so building from source needs Node.js (or a built `web/dist`); `install.sh` says so instead of building a terminal-only binary. Web Push stays optional with `UAGENT_WEB_PUSH`.
- **A `run` row reads as what it does.** `run` requires a `description` ("Run the unit tests"), shown on the row, with the command when it opens. Approvals still show the command. The `intent` argument is gone, and "edited N files" no longer counts commands.

### Fixes

- **A conversation of one long turn shows again.** When a single turn held more steps than one page, a saved conversation opened with only "Load older retained messages": the page carried the turn's steps but not the message that started it, so none could be drawn. A page now keeps that message, as the live view already did, and older steps load after it.
- **Draw on a page opens the markup at once.** In a design, the pen button goes straight to annotating the page's picture, with a spinner while the picture renders, instead of the image viewer first. The viewer no longer shows a zoom cursor, since a click there does not zoom.

- **Root commands ask for a password only when sudo needs one.** A user sudo lets in without a password (NOPASSWD), or a session already running as root, is no longer asked: µAgent checks with `sudo -n` first, on the target machine too, and runs the command with no prompt.
- **Deleting a conversation removes its agents' worktrees on macOS too.** An agent's worktree was recognised only when its saved folder was spelled like the home folder, so `/private/var/…` against `/var/…` left the worktree in place, unchecked for unsaved work. Paths are now compared where they lead.
- **`run` takes an empty `host` and a short `yield_ms`.** A model that fills in every optional argument, as GPT-6.1-Sol does, sent `"host": ""` and `"yield_ms": 1`, and `run` refused both, so in a web conversation it could not start a command at all. An empty host now counts as none, and a short yield is raised to the shortest one, as it already was where it is used.
- **A wait on background work shows as a row.** An `activity` wait or poll was hidden, so a turn spent waiting looked like thinking.
- **"1 turn", and a separator in the turn footer.** The composer said "1 turns", and a turn's changed files ran into its token count.
- **The browser no longer asks for a screenshot of a page that has text.** A short page, such as an order confirmation, got the hint meant for pages with no text.
- **No flag warning across the agent's browser.** Chrome 154 shows "You are using an unsupported command-line flag: --disable-blink-features=AutomationControlled" in a bar over every page, in the live view and the agent's screenshots. The managed policy now turns that warning off (`CommandLineFlagSecurityWarningsEnabled`); reinstall it with `install.sh` or the command in [Browser](docs/BROWSER.md).
- **Agents started with `items` and without `wait` run in the background.** `items` always waited for every agent, so a parent that asked for background agents sat idle until they finished and then did the rest of the work itself. Without `wait` the call now returns at once, and each agent reports when it is done.
- **Memory extraction no longer leaves conversations behind.** Each background extraction was saved as a conversation of its own, titled "This is bounded background memory extraction…", in the space of the conversation it read.
- **The find box keeps focus while you type.** When a message held more matches than it showed, such as a word in a link's address, Find clicked the message's actions button to open it; the menu then took the focus.
- **A long title no longer matches every search.** Search matched a name by its letters in order anywhere, so a long conversation title turned up for most words and crowded out the conversations that hold them. Letters spread wider than twice the query no longer match; an abbreviation such as "plt prb" still finds "Palette probe".
- **`--control` says what it expects.** A schedule request with a missing or unknown `action` returned the task list, and a request with no known `kind` said only "unknown library kind"; both now name the valid values. A caller the sandbox keeps out of the web host's folder no longer reads `runner_active: false`.
- **One item sent bare counts as a list.** A model that sends a single string where a tool takes a list of strings, such as one `web_search` query, is no longer refused and sent back for another round.
- **A switch that is on keeps its right edge.** Its knob slid past the end of the track and covered the border.
- **`web_search` is always a tool call.** A search hosted inside the conversation's own response could leave no record of itself for the next request, so a model took its real results for figures it had made up and withdrew them. A search now runs as a side request on a route that can search; its cited answer is a tool result that stays in the conversation and shows as a row in the history. `search_plugin` and `search_sources` are gone from `supports`: `hosted_search` now says an endpoint can search on any wire API. Set `webSearch.model` to answer searches with a cheaper model than the conversation's.
- **A signal no longer fails a browser action.** A signal arriving while a browser request, a Chrome control read, an RFB read or a timed write waited, such as a child process exiting, counted as a lost connection. The action then failed with "browser service timed out or disconnected". These waits now carry on.

## v1.8.0 - 2026-11-01

### Changes

- **Screens slide, a sheet swipes away, and the drawer follows the edge.** Where the conversations list is a drawer, opening a conversation slides its transcript in from the right and going back brings the previous one in from the left, unless the system already drew its own back transition; animations off, or a browser without view transitions, change at once. On a phone, a sheet follows a finger pulling it down from its header (or its body when scrolled to the top) and closes when let go past a third of its height or after a flick, the way Close does; short of that it springs back. The terminal and browser keep their touches. A swipe from the left edge opens the conversations drawer, in place of Safari's back swipe, except over a control, text that scrolls sideways, the terminal or an open sheet.
- **Switches are real switches, and every control answers a press.** A setting's switch is a native one, which gives an iPhone running iOS 18 its tap of haptic feedback. Any button, row or menu item fills the moment it is pressed and fades back on release, instead of only some of them, and the press no longer shrinks a control.
- **A file goes to the share sheet on a phone.** Download in Files hands the file to the system share sheet where the device can share files, and downloads it as before elsewhere or if the sheet is refused; cancelling the sheet shows nothing. Export still saves the transcript on the host.
- **The screen stays on while a turn runs.** On a touch screen the open conversation keeps the display awake until its turn ends, and asks again when you return to the app. A laptop is left to sleep.
- **A notification names the conversation.** A background notification shows the conversation's title and why it needs you (approval, a question, an error, finished) instead of only "µAgent needs your attention". `UAGENT_WEB_NOTIFICATION_DETAIL` (Notification detail, on by default) turns it off, and then the title never leaves the host. The status bar also follows the system's light or dark appearance before the app loads.
- **Layout follows the room the app has, down to a Split View window.** The conversations list sits beside the conversation wherever it and the conversation's minimum width fit and the window is not short, at any zoom, and is a drawer elsewhere; it no longer switches at a fixed 900px. The composer, the header's totals and menu, and the messages follow the conversation's own width, so a narrow column beside the list gets the phone layout. A window resized across these widths keeps the draft, the reading place and an open sheet.
- **A phone shows less: totals under the title, a short menu, rows without icons.** The context left and turns moved from under the composer to under the conversation's title, so the composer at rest is the input and one row of controls. The conversation's ⋯ menu lists Files, Terminal, Find, Pin, Rename, Move to… and More; Open browser, Fork, Statistics, Tools, Export, Detail, Compact, Restart, Close and Delete moved under More, and Settings left it for the conversation list's footer. On a touch screen only the open row shows its ⋯, and a long press on any conversation, folder or terminal row opens its menu. Queue next shows no Alt+Enter hint there.
- **A page that reloads often gets its event stream every time.** The host noticed a closed page only at the stream's next 15-second heartbeat, so four quick reloads used up the limit and the next one was refused for up to 15 seconds. A closed page is now noticed within a second, and a new stream at the limit replaces the oldest instead of being refused, which also frees the slot of a phone that vanished without closing.
- **A message sent while reconnecting waits and goes out once.** Plain text typed in the moment after reopening the app shows as "Waiting for connection", clears the draft, and is sent in order once the host is reached, as guidance if a turn is running by then. Return to composer takes it back before it goes. Attachments and slash commands still need the connection.
- **The terminal fills the phone, names its tabs and keeps its lines whole.** The page's terminal shows "Terminal 1 · ~" in one bar, with the tabs in a row of their own once there are two, and the number stays with a terminal when another closes. Add to chat moved into its ⋯ menu and is absent when no conversation is open. The terminal reaches the keys at the bottom of the sheet, which gained a keyboard key; its lines no longer clip under the keys, and a browser tab's terminal regrows when the keyboard closes.

## v1.7.0 - 2026-10-31

### Changes

- **A folder's terminal has a name, a time and a menu, as a conversation does.** Its row shows "Terminal 1", "Terminal 2" and so on, which stay with it when another closes, and the time it started. Its ⋯ menu pins it to the top of the list, renames it or closes it.
- **A conversation and its agents can move to another folder.** Move to… in a conversation's menu picks a folder or workspace, or takes a path, and moves the saved conversation with the agents it started; the page and any other open device follow it to its new address. Files stay in the old folder and memories stay with the old folder, and an agent that works in a worktree of its own keeps it. The conversation is told in a note, and a move is refused while a turn runs, mail waits or another program holds the conversation. Moving changes a conversation's id.
- **Conversations and folders can be pinned.** Pin in the menu of a conversation, workspace or folder moves it to a Pinned section at the top of the sidebar, the same on every paired device; plain folders gained a ⋯ menu for it.
- **Files shows what changed in a repository.** A Changes button beside Files lists the folder's uncommitted changes against the last commit, with their count, and opens each as a diff. It is read only, and runs no program the repository's own configuration names.
- **A conversation can start without choosing a folder.** New → Conversation opens one in a workspace named Scratch, which the host creates on first use.
- **The composer is shorter on phones and its totals sit in one field.** An idle composer no longer reserves an empty row under the input; the status row appears only while a turn works. Context left, turns, cost and queued guidance read together in one button that opens the statistics, which now start with the context and the way to the raw context. The word "Working" and the current to-do shimmer like the running row.
- **A wheel's scroll survives a page changing height on a busy machine.** Keeping your place while content above you changed could cancel the rest of a scroll in WebKit, because it waited a fixed time rather than for scrolling to stop.
- **A change the host notices by itself reaches the web at once.** A scheduled task you saved or paused, a new workspace or a session deleted elsewhere could wait for the event stream's next heartbeat, up to 15 seconds, so the Scheduled page kept showing the old state and Run now stayed disabled.

## v1.6.1 - 2026-10-31

### Changes

- **The container image is published.** v1.6.0 shipped without it or its compose files, because the release workflow skipped those jobs, so choosing the container installed µAgent here instead. This release is otherwise v1.6.0.

## v1.6.0 - 2026-10-31

### Upgrade notes

- **Remembered approvals are rules.** "Allow for this conversation" and "Always allow here" keep the rule the prompt names (`run npm test *`, `write_file ./src/`, a host for `web_fetch`, every call of an MCP tool; the exact call where nothing safe generalizes) instead of one exact call. The rules file is version 2 and its rules are `{root, tool, match}`; exact actions remembered before are dropped. `permission_rules` `delete` takes `tool` and `match` instead of `key`, and the `approval.resolved` event names the `rule` that allowed a call.

- **One status control under the web's input; the status line above it is gone.** The control reads "Working · 4m 02s · 1 command · 2 agents", "Needs you" with Show, "Stopped · reason" with Continue, or "Reconnecting…", and is absent while nothing runs ("Ready", "No activity" and "Idle" are gone); Queue next sits beside it. A command moved to the background keeps its call's row `running` until it ends, and that row then takes its result, so background completions no longer add a "Finished" receipt; a wait that takes the end does the same. Calls a tool declares `routine` (an `activity` wait, poll or list, an `agents` list) carry `routine` and show at full detail only, and every call node carries its start `time`. The terminal's working row names the detail level when it is not the default.
- **Terminals belong to a folder, or to the page.** A folder's terminal (`terminal` `open` with `folder`) is listed under its folder or workspace on every device, opens in the conversation's place and runs on until closed, exited or idle; the panel under the chat holds the page's own, which end with their view and are never listed or reattached. The session catalogue's `terminals` (null with the terminal off) and the `terminals.changed` event list the folders' shells.
- **The panel and its lead agent are gone.** `uagent panel`, `/panel`, `PANEL.md` (yours and the folder's `.uagent/PANEL.md`), every `panel.*` setting (`panel.dailySpendUsd`, `UAGENT_PANEL_DAILY_SPEND_USD`) and the Panel settings, `panel.json`, the session state's `paused` and the session header's and listing's `kind` are removed; `uagent panel -p` is `uagent -p`, which waits for the agents it started. `uagent_instructions`, `/instructions edit` and the control's `set` take only a `scope`. Any conversation starts agents, listed beneath it in the sidebar. A folder's or workspace's name in the web opens its overview (what works and needs you, spend, changes, and each conversation's step, to-do progress and cost), and `/overview` prints it in a terminal. The host answers `GET /api/folder?path=` (`spend`, `changed`, and `git` in a repository), and the session listing and header carry `todo` progress (`done`, `total`, `current`) and the listing each conversation's `cost`.
- **`approval.mode` takes `plan`.** Plan mode is a fourth approval mode beside `ask`, `auto` and `yolo`, so clients and scripts that list the modes get one more; a pending decision may carry `files` (shared files it is about).
- **A work node replies to one message of yours.** The work after guidance read into a running turn is a node of its own, `w-<message id>` under that message, finished once its facts hold `outcome` and `duration_ms`; `tool.call` events name their `work` instead of `turn_root`. Conversations saved before show their steps without a work line.
- **An agent is a conversation another conversation started, with one set of tools.** `subagent`, `thread`, `state`, `session` and `history` are replaced by `agent_start` (`brief`; `edits` none, worktree or folder; `model`; `wait`, by default; `items`, one agent each), `agent_send` (`id`, `text`), `agents` (`list`, `read` with `part` rows, report, diff or search, `stop`, `close`, `delete`) and `decide`, which every conversation has. Each agent is saved beside its parent as `agent-<id>.json`, runs and opens like any conversation, reports by mail when its turn ends, and asks its parent first (route `parent`), the person after 5 minutes or a yield. Agents take their parent's approval mode, Auto at most, and `edits` none only reads. Lean and full toolsets, answer schemas, refuters, `/agents ID …` and the inspector's agent follow-up are gone; `/agents` lists the conversation's agent tree. Settings `subagent.*`, `panel.maxThreads` and `panel.environment` are replaced by `agents.max` (8 at work at once) and `agents.depth` (2); `activities.maxConcurrent` counts commands only. Session metadata names an agent's `parent` instead of `folder`, and sessions have no `thread` kind. Saved subagent and thread conversations are not read as agents.
- **Yolo raises the sandbox's `default` level to `relaxed`.** While a session is in yolo, its commands read anywhere and write the home folder, except `~/.uagent`; the credential stores, the browser profile and µAgent's settings stay hidden. Ask and auto keep `default`, an explicit `readonly`, `relaxed` or `off` is unchanged. `/status` names the level in effect (`relaxed (yolo)`). A background agent can now be put in yolo too, from `/permissions`, `--yolo` or the web's control; it still starts in auto. Once you write to a background agent yourself, starting its turn or into one that runs, that turn's approvals and questions come to you instead of its parent.
- **Tool results have one contract, and their bracket notes are for the model only.** A result is its output, facts (exit, lines added and removed, a count, why it was refused) and a hint the model reads after the output: `[exit code N]`, `[path lines a-b of N]`, `[ripgrep · N result lines]`, `[captured log: …]`, the activity and `[Activity result: …]` headers, the web search and fetch notes, `[image saved: …]` and the `[running]`/`[started]`/`[detached]` lines now come at the end of what the model reads, and no row shows them. On the wire, tool rows and activity and memory receipts carry `state` (`ok`, `failed`, `cancelled` or `running`) instead of `status` (`success`, `completed`, `complete`), `result` (the facts and their `word`, such as `exit 2 · 1.4s`), `body_kind` instead of `view.output`, and `error_code`; `change` holds only the diff, without its `Edited path (+N -M)` line; `exchange_path` and the snapshot's `?artifact` query are gone. Conversations saved before show their tool rows without facts, and a row whose facts are missing shows no text.
- **The transcript reaches clients as one tree of nodes.** Every row of a view, a history page and a `block` patch is a node `{id, parent, kind, headline, result, state, routine, time, body}`. `kind` is `turn` (a message of yours), `message` (with its `role`), `work`, `group`, `call`, `thinking`, `notice` or `footer`, instead of `user`, `assistant`, `attachment`, `tool_result`, `activity`, `turn_summary` and `compaction`, and text is in `body.text`, a change's diff in a `diff` body. A reply's thinking is a node of its own; a turn's work (headline `Worked` `6 steps · edited 1 file · 1 error`, word `2m 4s`) and a run of like calls (`Explored` `3 calls`) are nodes the host makes, the files a call hands you a node of the turn (`<call>:files`), so `activity.group` is gone. A new node goes before the first node under it, else in the order it arrived, and streamed text appends to `body.text`. Gone from rows: `text_bytes`, `content_revision`, `content_complete`, the three `reasoning_*` counters, `reply_to`, `turn_root`, `detail_id`, `call_id`, `occurrence_id`, `response_id`, `name`, `arguments`, `view` and `error_code`; `?detail=` takes a node's id only. History pages end on whole turns, and search hits name their node's `id` and kind (the `history` tool's `message_id` is now `id`). Conversations saved before show their turns' work without counts and their calls ungrouped.
- **The Detail level is chosen per device.** In the web, Conversation menu → Detail (and `/detail` typed there) sets this browser's level only; the saved `UAGENT_DETAIL` is where a device starts until it chooses one. `/verbosity` and `UAGENT_VERBOSITY` are renamed to these. A level is how deep each turn opens: minimal shows a turn's work as one line, default opens it one level, full opens everything. Rows you open or close stay that way on the device until you choose a level. Failed and cancelled calls sit inside the turn's work, so at minimal they show once the work line is opened; files handed to you stay in sight.
- **Rows no longer carry `category` or `change_path`.** A call's or receipt's intent stays with the host, and a diff stored whole is read through `?detail=` by its node's id; its path never leaves the host.
- **Changing settings and instructions are tools of their own,** so their schema requires that payload and a provider that enforces schemas sends it: `uagent` `configure` is `uagent_configure` (`scope`, `changes`) and `set_instructions` is `uagent_instructions` (`scope`, `text`). `uagent` only inspects and takes no `action`. Update scripts, toolsets and allowlists that name the old shape.
- **A conversation messages only its parent, its agents and their siblings.** Yolo sessions in one folder no longer reach each other. A message counts one forward more than the mail that started the turn sending it. A parent's message reads `[parent guidance]`, any other session's `[session <title> (<id>)]`. Agents report their spend to their parent session (`~/.uagent/activity/usage-<session id>.jsonl`), so a parent's cost now includes its agents' finished turns.
- **Commands no longer read your other folders.** Below the `relaxed` level, commands cannot read the home folder's visible entries, `~/.uagent/spaces` or neighbouring workspaces, only their own workspace and tool folders. Add folders with `sandbox.read`.
- **Session ids changed.** The session and workspace hash was corrected, so conversations saved before are not found.
- **No compatibility with older saved data.** Conversations missing display metadata, context window, last prompt, delivered mail or object attachments are not loaded.
- **Names and formats from v1.4 and earlier are no longer read.** Set `sandbox.level` instead of `sandbox.enabled`, and note `UAGENT_SANDBOX` takes only `strict`, `default`, `relaxed` or `off`. A format 2 settings file needs one run of v1.5 first.
- **Settings and tasks from v1.3.1 or earlier are no longer read.** Run v1.4 or v1.5 once to rewrite format 1 settings and tasks saved with `"permissions": "prompt"`.
- **The self-directive is gone.** `adapt_system`, `prompt.adaptSystem` (`UAGENT_ADAPT_SYSTEM`), `/instructions clear` and the web's Clear are removed.
- **Removed commands:** `/debug-config` (use `/config KEY`, which now also shows the conversation's own value), `/models` (use `/model QUERY` or `/model all`), the aliases `/new`, `/ctx`, `/commands` and `/resume` (`/reset`, `/context`, `/help`, `/sessions`), and `uagent config export` and `uagent config import` (copy `~/.uagent/config/settings.json`).
- **Removed settings:** `approval.allowAbove`, `approval.denyAbove` and `approval.vetoAbove` (set `decision_bars` on the provider), `mcp.roots` (set `roots` per server in `.mcp.json`), `project.trustConfig` (use `--trust-project-config`), `memory.redactKeywords`, `terminal.markdown` and `terminal.reducedMotion` (use `--plain`); `UAGENT_INTERNAL_TOOL_RUN_ALLOWLIST` is gone.
- **Background work is called activity everywhere.** The turn's status line is now `turn_status` on the wire (frame kind, session state and metadata field; event `turn.status`), activity rows have `kind` `command`, `memory` or `detached` (was `agent`), result headers read `[Activity result: <kind> <id> ...]`, mail says `[activity finished`, the status row counts `act:N`, and the activity logs live in `~/.uagent/activity/` (was `bg/`). Old logs under `bg/` are left in place.
- **The `strict` sandbox level is gone.** Use `sandbox.network` off (`UAGENT_SANDBOX_NET=0`) with `default`. A saved or exported `strict`, or any value that is no level, refuses every command and says so, rather than becoming `default` with the network open.
- **A `-p` run is saved like any conversation**, so `-c -p` continues the latest one and scripts leave one history entry per run.
- **Headless `--json` reports this run's usage only**, not what a continued conversation spent before; a conversation another runtime holds gets an error envelope.
- **`todo` is a to-do list every agent keeps.** It takes `items` (`text` and `status`: `pending`, `in_progress` or `done`, at most one in progress) and replaces the whole list; the list is kept with the conversation, carried on every request while it has items, and shown above the web's input. The panel's pinned goals, decisions and open questions, its plan board and `/board` are gone.

### Changes

- **The web bundle carries DOMPurify 3.4.17**, which fixes the sanitizer advisories against 3.4.16 (pulled in by Mermaid).

- **Root commands, here and over ssh, without the agent ever seeing the password.** `run(root=true)` runs a command through sudo, on this machine or with `host="user@machine"` on another over ssh, with or without a terminal. Each asks for your approval; the first of a turn asks for that machine's sudo password, hidden in the terminal and a password field in the web. µAgent writes it only to sudo's own prompt, which it recognises by a random token sudo prints with echo already off, so the password never reaches the model, the screen, a log or the session; it is forgotten when the turn ends, and a wrong one stops the command and is asked again. A command's own `sudo` fails at once with a pointer to `root=true`. Headless runs and agents get no `root`.
- **A command's prefix rule rests on the sandbox, not on a list of programs.** A kept prefix (`run make *`) is offered for and covers only commands the enforced sandbox holds, which bounds what any prefix lets through; with the sandbox off or not enforced every command is allowed only exactly. The fixed list of programs never given a prefix (shells, interpreters, `rm`, `find`…) is gone: it let any program it missed through. A failed sandboxed command now always carries the sandbox's note instead of only when its output used one of eight phrases, and two hints keyed to command or output text (`pkill -f`, "No module named") are gone; `scratch` says where a script's packages go.
- **The container starts under current Docker.** Its seccomp profile is Docker's current default plus the user-namespace calls Chrome needs; the old copy refused `openat2`, `close_range`, `clone3`'s fallback and the Landlock calls, so runc 1.5 crashed starting the container and the agent's command sandbox could not confine itself in it. The io_uring calls Docker no longer allows are gone with it.
- **One line installs µAgent, and setup asks for your keys.** `curl -fsSL https://raw.githubusercontent.com/timongentzsch/uAgent/master/install.sh | sh` installs the latest release, checked against its checksum, or the published container image (`ghcr.io/timongentzsch/uagent`), and running it again updates; a system the release cannot run on (glibc older than 2.38, musl, macOS before 15, another CPU) builds from source instead of installing a binary that does not start. It offers to add `~/.local/bin` to your `PATH` and to keep `uagent --web` running as a systemd or launchd service of yours, then prints a pairing link. `uagent setup` (run by the installer, and offered by a first start with no key) asks for OpenRouter, OpenAI and Anthropic keys, checks each, and saves them with a default model it suggests; OpenAI and Anthropic become providers named `openai` and `anthropic` whose keys are variables in the settings file. A start whose runtime cannot begin now says why ("no provider configured — run uagent setup …") instead of "session runtime ended while starting (status 1)".
- **Plan mode, as in Claude Code.** `/plan` in the terminal, or Plan in the web's approval control, puts a conversation in plan mode: it only reads (commands run in the `readonly` sandbox, and agents it starts only read), and anything else is refused with "plan mode is read-only; write the plan to …". The one file it writes is the conversation's plan, kept beside its session (`<session>.json.plan.md`), never in the folder. Each turn in plan mode ends with the plan shared as a file in the conversation and three choices: **Approve and run in auto**, **Approve and ask before changes** (either leaves plan mode in that mode and starts the work), or **Keep planning** with your feedback. A turn that writes no plan says so and stays in plan mode, as the mode does through a restart.
- **Guidance you send while a turn runs shows where it was read.** It sorted after the whole work, next to the answer; now the work before it ends there, your message follows at every detail level, and the work after it counts its own steps and time (`Worked 2 steps · 18s`). The footer still totals the turn.
- **On a phone, the terminal (and every surface) regrows when the keyboard closes.** iOS can keep reporting the keyboard's smaller visible area until the page scrolls, and a terminal never scrolls the page, so its sheet stayed half height over a black band. Only a field being typed into now holds the keyboard layout.
- **Every call reads as a verb and a target, with its result in a few words** (`Ran npm test · exit 1 · 4.2s`, `Edited a.ts · +3 −1`, `Searched /x/ in src · 12 lines`, `denied`, `rejected: …`). `agent_send` and `agents` say what they did (Messaged, Read, Stopped, Closed, Deleted) and `todo` its progress (`Updated to-dos · 2/5 done`); an MCP tool reads "Called <Tool>" with its first short argument, never its raw JSON. A skill loaded is a row like any other.
- **The terminal draws the host's tree.** A turn opens to the level's depth: `minimal` shows the work as one line (`Worked 4 steps · edited 1 file · 1 error · 53ms`), `default` opens it to its steps, one line each (`✗ Ran python3 missing.py · exit 2`), with interim replies indented among them, and `full` opens every step to its output's last lines. Routine notices show at `full` only. A live step is drawn once it ends, in the tree's order, and the work's line once the turn does, after the answer; replies still stream, under one speaker row per turn. A resumed reply no longer leaves a blank line under it. `/detail` redraws the conversation in place instead of attaching again, and the terminal no longer counts steps or files itself.
- **The host builds the transcript once for every client.** Live rows and saved history were built by two builders, and a turn's "Worked" summary was counted in the web and again in the terminal; one builder now makes the same nodes for both from what was recorded, and counts a turn's work once.
- **A refused call reads as what it would have done** (`Ran cd /tmp/fli && …`), with the error beneath, instead of as its raw JSON arguments; one that names no tool, or sends no object, reads as `Called <name>` with its first short argument.
- A reply that is only blank lines (some models send one before their calls) is no longer drawn as an empty row with its own time and ⋯ menu.
- **The web draws the host's tree as it is.** Its own folding of turns and groups is gone: each row is one line of one height, with one gap between rows at every level, and opens to its output, diff or files and what it offers (Full output, Input and output, Open agent). Your message carries its time and ⋯ on its first line; an answer has no header, and its time and ⋯ end its turn's footer, whose menu holds the statistics. Work, group and call rows have neither. Rows are focusable: Enter or Space opens and closes one, → and ← open and close it or move into and out of it. A row with more than 30 steps shows the latest and one "… N earlier steps" line, and while a turn runs its closed work line says what it is doing and for how long (`Working · Running npm test · 12s`). Routine notices show at full only, a compaction says what it kept (`120 → 40 messages · 1.2s`); older history read back is bounded on whole turns, so a turn never loses its end.
- **The file tools treat the sandbox's writable roots as the work area**: writing a script to `/tmp` (or `$TMPDIR`, the package caches, a `sandbox.write` root) no longer asks, as the same write by a sandboxed command never did. Hidden stores, closed folders and µAgent's own configuration keep their checks.
- **The composer is one row while the draft is one line** on a wide enough screen: attach, model, approval and send sit beside the input, and the second row comes back once the text wraps. Phones keep two rows.
- **On a phone the terminal is its own full-screen view**, and the keyboard shrinks it instead of covering it; Add to chat steps back to the draft. The web UI is a little denser everywhere: one spacing scale (padding 8px, gaps 6px, as phones had), 36px roomy rows and headers, prose line height 1.5. Touch targets stay 44px.
- **Each model call records which provider served it.** A router such as OpenRouter picks the upstream per request; its name and the generation id now go in the debug trace's `model_response`, the `/http` capture and the message's statistics ("served by …"). The conversation's statistics and `/cost` count model calls and rejected tool calls per serving provider, so a provider that mangles tool calls stands out.
- **A terminal in the web interface.** A panel under the chat (the header's Terminal button, Ctrl+`) holds tabs of your own shell on the host, unsandboxed, for paired devices only: in the conversation's folder, your home folder without one, or a folder from Files. A folder's terminal ends when closed, when it exits or after `web.terminalIdleMinutes` (60) unseen and silent. The agent never sees them; **Add to chat** puts the selection or the screen into the message. `web.terminal` (`UAGENT_WEB_TERMINAL`) turns it off.
- **A tool call missing a field its action needs is stopped before it runs, and says what arrived.** `agents`, `decide`, `activity`, `memory` and `browser` declare the fields each action needs; their descriptions list them, and a call without one is told every missing field and what it carried (`` `id` is required for stop; this call carried only `action` ``), as a call missing a field its schema requires is. Before, most tools checked their own fields only once the call ran, so repeating the call never ended the turn. `activity` `write` takes a left-out `chars` as an empty write.
- **The agent adds an endpoint without dropping the others.** A `uagent_configure` call for `UAGENT_PROVIDERS` names only the providers it adds or changes, and `null` removes one; the rest, and keys the agent is never shown, are kept. Before, the value replaced every provider, so adding one deleted the endpoint the conversation was using.
- **More limits are settings,** for you and for the agent: `activities.maxConcurrent` (commands at once), `tools.concurrency`, `tools.readFileKb`, `tools.editFileMb`, `tools.listEntries`, `tools.grepResults`, `activities.logMb`, `webSearch.maxResults`, `webSearch.maxUses`, `webSearch.callsPerTurn`, `webSearch.timeout`, `memory.max`, `memory.maxBytes`, `memory.alwaysBytes`, `memory.idleHours`, `skills.max`, `instructions.maxKb`, `mcp.maxServers`, `mcp.maxTools`, `attachments.maxCount`, `attachments.maxImagePx`, `history.files`, `runtime.idleMinutes` and `limits.repeatedCallStop`. Log retention is one pair, `logs.keepDays` (14) and `logs.keepFiles` (200), replacing the separate 14/50, 7/200, 7/100 and 7-day values for sessions, activity, MCP logs and terminals; session traces and MCP logs now keep more files than before.
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
- The web host answers `GET /api/search?q=...` across conversations, tool output included.
- A new `readonly` sandbox level: commands read the workspace, write only temporary files and have no network. `sandbox.level=readonly` can be chosen for any conversation.
- Deleting a conversation removes its lock; the agents it started stay.
- A refused approval reads "this action was not approved".
- A command failing a name lookup while the sandbox denies the network now ends with the sandbox's note that the network is closed.
- On macOS the sandbox lets commands write the per-user temp folder, which stopped `git` printing cache errors.
- The system prompts say that `$...$` and `$$...$$` math renders.
- A background command's result reaches the model once; if the conversation is idle, it starts a turn. Guidance sent while `run` waits (`yield_ms: 0`) moves the command to the background.
- A runtime lets go one idle period after its last input; a child's SIGCHLD no longer restarts the period.
- Web: reloads paint the app frame first, restore Library and Scheduled from samples, and keep the app on screen while the host is unreachable; no spinner shows for loads under 150 ms.
- Web: dialogs fade in with their content; statistics show input (cached and cache-written included), output and cache hit rate.
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
- Sandbox: a linked git worktree whose repository is in a closed folder (a sibling worktree, an agent's worktree) can use git there: its git directories open for reading, and a commit may write the worktree's git directory and the shared `objects`, `refs` and `logs`.
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
