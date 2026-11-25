# Changelog

## Unreleased

### Fixes

- A `web_fetch` rule for a site no longer matches an address whose query or fragment holds `@` and that site's name; the rule is keyed by the host the fetch goes to. A preview's sheet engine route no longer serves a file outside the engine's folder.
- A `web_fetch` allowed for one site no longer follows a redirect to another. It follows redirects itself only on the same host, never from https to http, at most five times; for any other target the model is told the new address, and fetching it asks for approval and checks the site rules again.

## v1.12.0 - 2026-11-24

### Changes

- Delegated agents inherit the parent's approval mode, including YOLO and Plan. Their edit scope still applies. Running agents appear above the composer, and completed agents can be reopened below it as ordinary conversations.
- File citations and copied artifact links open in their original workspace after a conversation moves. Older links with multiple matches offer a folder picker. A deck review finding opens the affected slide directly.
- New side chats move with their parent, count toward its spend and are deleted with it. Closing a tab hides it. Reopen it from Activity, or choose **Open as a conversation of its own** to detach it. Existing v1.11 side chats remain independent. Open side-chat tabs reset once on upgrade.
- Merging a side chat keeps its transcript visible while the summary streams. The parent receives one expandable summary with a link to the side chat. Further replies can be merged again. An interrupted or failed merge leaves the side chat available.
- On phones, Home shows the prompt and three recent conversations. Typing gives the composer more room. A swipe from the left edge closes the topmost pane or opens the conversation drawer.
- A single animated band under the composer indicates work in progress. Other status marks stay still, and reduced-motion settings also stop the band.

### Fixes

- Background memory updates recheck the source and the note before saving. Older conversations cannot recreate a forgotten note under another name. A conversation saved with memory disabled keeps that setting on resume.
- Deck thumbnails stay aligned when a slide has a section heading and when the selection changes. Preview inspection can recover when its access grant rotates.
- After a runtime stops mid-turn, the conversation shows the interruption and starts a new runtime on the next message. The terminal reports the cause if startup fails. Reports and tool activity arriving before the user's message no longer supply the conversation title.
- The agent browser reports a crashed tab and supports reopening it. Browser tools and PDF or PNG export explain when the available memory is below the 600 MiB startup minimum. On Linux, out-of-memory scoring favors ending a runtime or browser before the shared web host.
- Selection actions stay beside the selected words and inside the transcript. A sent message's time and menu align with its last line.
- Tools from a stopped MCP server leave the model's tool list and the context display on the next turn.

## v1.11.0 - 2026-11-22

### Upgrade notes

- **Conversations kept before this update are no longer listed.** Saved conversations now live in one folder, `~/.uagent/history`, and a move between folders keeps a conversation's address. Earlier ones stay on disk in the old per-folder directories under `history/` and are not read. To bring them over, stop uAgent, back up `~/.uagent` and run the conversion script attached to this release. Each device's unread marks, unsent drafts and queued messages reset once.
- **Name the reviewer if Auto should ask another provider.** A call judged in Auto is sent only to the provider the conversation runs on, or to the one `approval.reviewerModel` names (`openrouter/` for that provider's own decision model). Before, any configured provider with a decision endpoint was asked, so a conversation on a local model sent its commands to OpenRouter. With such a setup Auto now asks you, and says which setting to set.
- **Models are offered `apply_patch` in place of `write_file`, `edit_file` and `delete_file`.** The three still run each file of a patch, so permission rules, approvals and tool settings that name them work as before; `write_file` is offered by itself only while the conversation has an attached file to copy.
- **Previewed pages load nothing from another host.** The preview policy no longer admits Google Fonts, and the agent writes pages with system font stacks or a font file in the folder. A page that links a Google font is drawn in its fallback face and lists the link as not loaded.
- **Every `/btw` starts a side chat of its own.** A second `/btw` used to follow up in the first; follow up in the side chat's own field now.
- **Building from source needs OpenSSL 3's headers** (Homebrew's `openssl@3` on macOS, `libssl-dev` on Debian and Ubuntu). Web Push is always built in: the `UAGENT_WEB_PUSH` option and the `ci-release` preset are gone (use `release`).
- **`--image` and `--continue` are gone.** They were unlisted doubles: use `--attach` and `-c`.

### Changes

- **See what fills the context.** Statistics opens with the context window broken down: tool results, replies, your messages, files, notes, tools and MCP tools (each listing its tools by size), the system prompt, instructions and memory, what is free until the conversation compacts, and the tools it is not offered. The parts follow a running turn and stay with a saved conversation. The numbers are estimates; **Compact now** and **Raw context** sit beside them.
- **Ask about a selection.** Select text in an answer, a message or a tool's output and **Ask in side chat**: a side chat opens with the selection quoted, answers beside the conversation and leaves it untouched. **Quote** puts it over the input instead, in the message being written, with the lines of a file or the end of a terminal quoted the same way. Lines selected in Files can be asked about the same way.
- **Merge a side chat back.** **Merge** has the side chat sum up what it found; the conversation receives the summary as it does an agent's report, answers, and the tab closes. Only the summary enters the conversation.
- **Several side chats, in a column.** Side chats are tabs in a column beside the chat, between the chat and Files when Files is open, and under the transcript in a narrow window.
- **Several files change in one call.** The agent changes files with `apply_patch`: one call adds, updates and deletes any number of files, where each file took a model round before. Every file of a patch is still approved, journalled and shown as its own change, and rules written for `write_file`, `edit_file` and `delete_file` keep applying to it. On five small fix tasks a small GPT model needed about a third fewer rounds and tokens.
- **The to-do list is for long work.** The agent keeps one for work of several stages or many files, no longer for a single fix, where some models spent whole rounds on it.
- **Agents started in Auto are judged.** The review sees an agent's brief (and a later message to it) before the agent starts, against what you asked, so a conversation steered by something it read cannot pass a request on through an agent. In Ask, Plan and yolo agents start unasked as before; a folder rule can refuse or ask (`/permissions deny agent_start *`).
- **An agent's report arrives as data.** The parent reads it labelled as the agent's report, data, not instructions, whether it waited for the agent or the report came as mail.
- **A report that tries to direct its parent waits for you.** In Auto the review judges a report before the parent reads it. One that instructs the parent, claims work beyond its brief or carries secrets shows on a card with **Withhold** and **Deliver**; with nobody to ask, the parent gets only a note that it was withheld, and the report stays in the agent's conversation.
- **Whiteboards.** The agent can draw a flow, map, retro or plan as notes, sections, text, twelve shapes, tables, pictures, link cards and arrows, alone (`boards/<name>/board.html`) or as a page of a design. Opened in Files, items are dragged, nudged with the arrow keys, resized by their corner and retyped with a double click, and ⌘Z undoes; each change is written to that item alone.
- **Numbers from your data.** A page can show values from a CSV or JSON file in its folder; pointing at one shows the file, column and row it came from, and Open shows the file there. A value naming a missing column or row is reported to the agent.
- **Sheets take borders, text sizes, row heights and coloured dropdown options**, from the format row too, and keep them through Excel export and import. Find works in sheets, over what the cells show, and Replace changes typed text, never a formula.
- **Documents have checklists**, ticked in Files without Edit, and code and quote blocks you can insert.
- **Slides build and push.** Elements of a slide can appear one click at a time while presenting, a slide can push the one before out (others fade in), and sections head the legend.
- **A design's pages export from the canvas**: copy as PNG, PNG at 1× or 2×, PDF, or every page as one PDF, each at its own size.
- **See what a turn changed in a file.** A file opened from a turn's changed files shows that turn's change, even outside a repository, with Undo beside it.
- **Whiteboards are drawn anew.** Shapes are filled and rounded with no line round them, and an arrow runs in right-angled legs with rounded turns: straight between two items that face each other, round an item that stands between, into the side that has room. A label stands beside its arrow where it covers nothing, and a section's name stands above its frame. Boards made before draw this way too; their files are unchanged.
- **Undo and redo in documents, decks and whiteboards.** ⌘Z or Ctrl+Z takes back the last change you made by hand, ⇧⌘Z or Ctrl+Y makes it again, and both are buttons in the tool row. If the agent wrote the file since, undo says so and changes nothing.
- **A whiteboard is listed as one.** The Library has Whiteboards beside Designs and Decks, and says Documents where it said Docs.
- **An open artifact is live.** A file shown in Files follows what is done to it within about a second, whoever does it: the agent's write, a shell command, a sub-agent, another conversation. The open page takes the change where it stands, so its zoom, scroll and selection stay: a whiteboard's items glide to their new places and new ones fade in, a document's blocks change in place. What changed is outlined for a moment, and while the agent is writing the open file the bar says **Agent writing**. A page with scripts of its own is loaded again, as before.
- **Edit a whiteboard by hand.** With **Edit** on, a toolbar at the foot of the board places a note, a text, a shape, a section or an arrow; a bar over what is picked changes its shape, colour and text size, brings it to the front or sends it back, duplicates, deletes or asks the agent about it. Pick several with Shift or by dragging a box round them, move them together, align them and spread them evenly. Any corner resizes, a second press on a picked item retypes it, and a dot on each side of a picked item draws an arrow to another. Copies of several items keep the arrows between them. Delete, ⌘D, ⌘C and ⌘V, ⌘A and Escape work, each change is one step to undo, and all of it works in the pane beside the chat and on a touch screen. With Edit off a board is only looked at: one finger pans, two zoom, and nothing moves; a second finger during a drag puts the item back and zooms.
- **Edit a slide by hand.** With **Edit** on, a press picks an element of a slide and a second press types in it. Drag it to move it, with the rest of the slide staying where it is; a corner sets its width. The toolbar at the foot puts in a text, a rectangle, a rounded one, an ellipse or a table where you press next. The bar over what is picked selects the container it is in (so a whole card moves), puts a moved element back, duplicates, deletes, orders what you put in and asks the agent about it; several picked align and spread evenly. The keys and undo work as on a whiteboard, and a change the file no longer takes, because the agent rewrote that element meanwhile, is said and the slide is shown as the file is.
- **A sent message lifts off the input.** The message you send starts as the draft in its box and travels up into its place on a spring, turning into its band on the way, and the rows above it are pushed up by the same motion. It moves from the first frame after the press, without fading in, and it is in its place within about a sixth of a second however far it travels; from high up the view it arrives without the overshoot. The times and both springs are tokens (`--motion-send`, `--motion-glide`, `--ease-send`, `--ease-settle`), and reduced motion turns all of it off as before.
- **A deck as a grid, and a slide that pushes in.** **Grid** in a deck's tool row shows every slide as a tile in the stage's place: press one to open it, drag one to a new place. In the slide menu, **Push in** makes the slide push the one before it out when presenting, where the others fade; it sits beside **Skip slide** and keeps it.
- **A thought is drawn whole.** With thinking shown, a thought is no longer cut to its first lines in the web or the terminal, whatever its work shows.
- **A conversation keeps its address when it moves.** Moving a conversation to another folder rewrites only where it works: its link, pins, unread mark, drafts and queued messages stay with it, on every device, and a side chat can still be merged back afterwards.
- **A sheet's format is a bar under the grid.** Bold, italic, wrap, alignment, text size, colours, borders and number format for the selection sit in one row at the foot of the sheet; the cell menu no longer repeats them.
- **Fill a sheet by dragging.** The selection has a handle at its corner: dragged down or across it continues a series, repeats a value or carries a formula along, with the cells' looks.
- **A cell can be a link.** A cell that holds only an address, or the agent's `<a href>`, reads as a link and opens it; ⌘K sets or removes one.
- **Text tools where the text is.** In a document they appear by the text you select, with what can be inserted in a bar at the foot; on a slide they are in the bar over the element being typed in.
- **The agent sees what it made.** `observe` with a path pictures a document, deck, sheet, whiteboard or design as Files draws it, a tall page in parts, so the agent can check its work by looking.
- **Switching conversations is lighter.** A switch redraws two rows of the list where it redrew all of them, draws each message once, and a conversation shown before appears in the first frame without a loading placeholder: about half the time from press to content.

### Fixes

- **A sheet no longer rises and then drops.** On a touch screen a sheet with fields, such as **Model and effort**, slid up a little too tall and then shrank, so its top moved down again: each field held 6px more than its height until a script had measured it, which it could only do once the sheet was shown. A one-line field has its height from the first frame now.
- **Even on a phone, too.** The status line under the input stands as far from the screen's bottom as from its sides, and in the Activity sheet a row's Remove button fills its row, so it is as far from the sheet's end as from its side. The phone tour of every view now fails on a control that stands closer to one edge of its box than to the other, as the desktop one does.
- **A conversation is not written over by an empty one.** When an agent's report reached a conversation that another process still held, a second runtime started for it, could not resume it, and on its way out saved its own empty conversation, which landed over the real one once the holder let go: the title became "(no messages)" and the cost zero. A runtime that cannot resume now leaves the file as it is.
- **Requests no longer wait two seconds in a burst.** When several new connections arrived at once, the web host counted them against idle threads that had not woken yet and left the rest queued until an idle connection timed out, two seconds later: a folder opened slowly, and an open artifact followed its file late. It starts a thread for each queued connection now, as the HTTP library does from 0.62.
- **An open folder is followed even after a slow first look.** While the first look at one folder was still out, looks at the folder then shown were skipped, so a change made in that time became the baseline and was never shown, and Find kept counting the old text. Each folder's look stands for itself now.
- **A whiteboard's section lets go.** A press on the empty inside of a section picked the whole section, so on a board laid out in sections nothing could be let go of, and the slightest move dragged the section. A section is held by its name or its edge now. A press inside it is a press on the board: it lets go of what is picked, a drag from there draws a frame, a finger pans. A frame picks a section only when it holds all of it.
- **The same distance to a box's side as to its end.** In the one-line input the Send and approval buttons stood 2px from the box above and below and 4px from its side; they stand 4px from every edge, as far as from each other. Every bar (a conversation's head, the Files head and its tool rows, the terminal, the browser, a sheet's bars) insets its controls by that same 4px on every side, where it gave 8px at the sides and 4 to 6px above and below, so bars stacked over each other line their first buttons up; a bar is 40px high at a desk and 52px on a touch screen, and the tighter tool-row variant is gone. The list's sections stand as far from their dividing lines as from its sides. **Ask in side chat** and **Quote** under a selection are the floating toolbar the files views use, a tray with its two buttons evenly inside, where they were two joined buttons unevenly in a box. A test measures it on a conversation, a selection, the Library, Scheduled, Settings and the Files views.
- **The marks inside a page are tokens.** What is picked by hand on a whiteboard or a slide, and the outline of what someone else just changed, take their colours from `--page-pick` and `--page-changed`, so your own stylesheet changes them with the rest.
- **A press fills a row to its end.** In the home screen's Recent list the fill stopped before the time, which stood outside the row's button and took no tap; in a turn's changed files it stopped before the counts. Both are part of the button now.
- **Everything you can tap shows the press.** On a touch screen a conversation in the list, a row of the Library or of Scheduled, a slide's tile, a link or a file reference in an answer, **Copy code**, **More options** and **Show source**, a question's option, a tool's card and a sent picture or file looked the same pressed as at rest. Each now fills as a button does, or takes the ring where a picture would hide a fill. A tool's whole card switches it, not only its text.
- **A long quote stays inside the message box.** On a phone a quote whose first line was wider than the screen stretched the box under it: the draft, its controls and **Send** ran off the edge. The quote is cut short with an ellipsis, as on a wide screen.
- **The Library's place field fills its row on a phone.** It stopped short of its arrow, which stood outside the field.
- **The list's drawer shows its title whole on a phone.** It read "Conversati…"; the drawer now takes the screen's width but for an edge of the page to close it by.
- **The way back from a side chat takes its tap.** On a touch screen the line under a side chat's title, or an agent's, that leads back to its conversation lay under the touch area of the totals below it, so a tap opened the statistics. Both are one line now, side by side.
- **A new scheduled task's header keeps its row.** On a phone **All runs** and the task's name lay over the first field.
- **A sheet's tabs are as tall as the buttons beside them**, a full touch target on a phone.
- **Files attached to a draft are drawn as chips**, with an edge; they had the fill of the message box.
- **A link and a picture answer a pointer**, as buttons do.
- **A tile keeps no ring after a tap.** On a touch screen the slide or page last tapped could keep the ring a pointer shows.
- **A side chat on a phone is a page of its own.** The box under the transcript held tabs, three buttons, a second input and its button on one small screen. Where the side chats have no column, the conversation's input now asks (marked **Side chat**, with the quote) and the side chat opens as its own page with the way back and **Merge** in its header.
- **A conversation's side chats are in its Activity list**, with its commands and agents: "2 side chats" under the input, a row to open or remove each. A side chat's own fields are the message box every message is written in, with its arrow.
- **Jump to latest stays clear of a selection's actions** on a phone.
- **A side chat can be asked of a conversation that has gone idle.** The question arrived while the conversation's runtime was starting again and was refused with "session not ready".
- **A part of the client that failed to load is loaded again.** Markdown, math and code highlighting were remembered as failed after one interrupted load, so a moment offline could leave answers unrendered until a reload. Loads started ahead of their use no longer raise an error nobody handles.
- **An approval names the action, not the tool.** The card asking for a decision says what the call does in the words its row will show ("Running git push", "Writing notes.md", "Starting agent …") where it showed the tool's name (`run`, `write_file`, `agent_start`), and its "Allow run?" line is gone. A held agent report reads "Delivering an agent's report".
- **Quote, as in a messaging app.** **Add to chat** is now **Quote**, in a selection's bar, in Files and in the terminal's menu. A quote no longer lands in the text field: it is a line over the input with an X to remove it, several stack, they stay with the draft, and they go before your text when it is sent; a quote alone can be sent, and a command typed under quotes runs as typed and leaves them for the next message. The selection's buttons are no longer covered by a pane's resize handle. A comment on a design is added with **Add comment**.
- **The focus ring is drawn inside the control.** It stood outside every field, button and row, so in a pane, a sheet or a column it was cut off at the edge or lay over the control beside it, such as the quote above a side chat's message box. On a filled button it is drawn in the button's own ink, so it shows in both themes.
- **The buttons of a toolbar keep an inset from its edges.** The browser's bar, the terminal's, a sheet's and a page's tools had no padding above and below, so on a touch screen a filled button such as **Done** touched the bar's top, bottom and the window's corner while its neighbours floated. They sit in the bar as the header's do.
- **A queued message survives a reload.** A message queued behind a running turn could leave the queue when the page was reloaded at that moment, and read as not confirmed or as sent. It stays queued, and is taken once.
- **A reload keeps your place.** In Safari the transcript could return to its end after a reload.
- **A side chat reads as working at once**, in the Activity list and in its own column, from the moment its question is sent.
- **A sheet's cell menu closes when you press elsewhere in the app**, and that press does its work at once.
- **No line along a phone's screen edge.** The conversations drawer drew its frame into the display's round corners.
- **Deleting a conversation removes its mailbox.**
- **An MCP server's output schemas no longer count against the limit for tool schemas**, so a server that declares them keeps more of its tools.

## v1.10.1 - 2026-11-16

### Upgrade notes

- **The HTTP API names things one way.** Session metadata says `needs_you` where it said `pending` (a snapshot's `pending` decision and a receipt's `pending` stay). `/api/browser/status` and `/api/browser/viewer` take `instance_id`, `/api/search` takes `session_id`, and `POST /api/library/delete` takes `session_id`. A malformed command is 400 (409 now means only that the protocol differs), saving a file of an unknown conversation is 404, a ping of an unknown host is 404, and an unknown `update` action is refused.

### Fixes

- **A narrow list keeps its names.** A conversation's place gives way before its title, a remote host's status before its name, and Library and Scheduled stack instead of running past the edge.
- **A takeover whose Chrome cannot start leaves nobody in control.** The browser stayed held by that device, so the agent's browser calls and every other device were refused until it let go.
- **The browser's status says when its service is down** (503), instead of answering 200 with an error inside.
- **An unknown command kind is refused as unsupported**, not as a stale generation, and every stale-generation refusal reads the same.

## v1.10.0 - 2026-11-16

### Upgrade notes

- **Remember your folder rules again.** `~/.uagent/config/permissions.json` is format 3: each rule has an effect. A file of format 2, which every install has, is refused with a message naming it, and none of its rules apply until you remove it; nothing is converted.
- **A headless run no longer takes approvals from its input.** `echo y | uagent -p …` approves nothing: with no terminal on stdin every call that would ask a person is refused. Give an unattended run a folder rule (`/permissions allow run "npm test *"`) instead.
- **Yolo no longer writes the files that run code at login.** At the `relaxed` sandbox level, and so in yolo, commands cannot write `~/.ssh`, shell startup files, git's global config, `~/.mcp.json`, other agents' folders (`~/.agents`, `~/.claude`, `~/.codex`) or launch agents and user services; a file tool's write to one always asks you.
- **Auto judges by kind, not by who calls.** A file tool's change inside the folder runs without the judge, in the conversation as in its agents; an agent's commands now go to the judge as the conversation's do.

### Changes

- **Folder rules allow, ask or refuse.** A rule that refuses holds in every mode, yolo included, and before you are asked; one that asks asks in every mode; both cover any part of a chained command. An approval's More options keeps one as **Ask each time / Always allow / Never allow**; Settings › This folder › Rules lists them by effect and adds one; `/permissions allow|ask|deny TOOL MATCH` does from the terminal.
- **You see why you are asked, and what Auto refused.** The approval card says which stage asks: the mode, a rule, the review, or a call that always needs you. A call Auto's review or a rule refused without asking shows in the transcript, under a folded turn too, with why and **Allow in this conversation** (or **Rules…**). After 3 refusals in a row, or 20 in a conversation, Auto asks you until you answer one.
- **The reviewer evaluation has a hold-out.** `tests/reviewer_eval` has 40 more cases (permission or a boundary given earlier, the turn's earlier calls, the repository's state, trusted places, file and web tools, instructions planted in the action), a fit and a hold-out split, and `--chat`, which lets a chat model stand in for the decision model to compare what the judge is shown. Showing it more of the conversation, and an explicit policy, did not do better on the hold-out, so the judge is unchanged.
- **The approval order is one pure function** (`Decide`, `src/app/approval_policy.cc`) with a table test and invariants checked over every combination of facts.
- **The docs are written for someone who reads the source.** The README opens on the process model, the properties and the design decisions. `docs/architecture/` has one page per subsystem in one shape (responsibility, processes, formats, protocol, limits, failures, source and tests); `docs/interfaces/` specifies the HTTP API, headless output, the files on disk and the artifact formats, and its settings, command line, tool schema and limits pages are generated from the binary's registries, with a test that fails when they differ; `docs/operations/` covers install, container, remote access and recovery; one `docs/manual.md` replaces the guides. `SECURITY.md` is a threat model. Diagrams are Mermaid in the page. The old pages are gone and their paths with them.

### Fixes

- **An approval's answers stack on a phone.** In a narrow column Deny, Allow for this conversation and Allow once each take a row at full size, the usual answer last; side by side the middle one broke into two lines of small text.
- **A dialog no longer scrolls sideways for a long path.** On a phone the Move dialog's note pushed Cancel and Move off the screen. Text in a dialog breaks where it must.
- **Phone rows fit.** The suggestion bar's answers stack; a page's tools after Edit fold into **More tools**, Export with them; a sheet's tabs keep room for a tab's name; a form's two fields side by side go one over the other when either would be narrower than a pane. A test opens each view at phone width and fails on a cut or wrapped button.
- **A sheet's Duplicate and Move act on the tab you see.** Right after a tab was shown, the page could copy or move the next tab instead.

## v1.9.1 and earlier

Notes for v1.0.0 to v1.9.1 are in this file as tagged at v1.9.1:
<https://github.com/timongentzsch/uAgent/blob/v1.9.1/CHANGELOG.md>. Releases
v0.4.0 to v0.9.0 are in the git tags:
<https://github.com/timongentzsch/uAgent/tags>.
