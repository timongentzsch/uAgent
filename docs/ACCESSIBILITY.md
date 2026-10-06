# Accessibility

What µAgent does for people who use a screen reader, magnification, high
contrast, reduced motion or only a keyboard, in the terminal and in the web
app, and how each promise is tested.

## Terminal

### Plain mode

`uagent --plain`, or `UAGENT_PLAIN=1` in the environment or configuration,
writes the conversation the way a screen reader reads it best:

- Rows are append-only lines. Nothing moves the cursor, erases a line,
  repaints a status row or animates a spinner.
- Each row opens with a spoken label instead of a glyph:

  | Row | Default | Plain |
  | --- | --- | --- |
  | your message | `> ` | `you: ` |
  | the answer | `uagent` header | `uagent:` |
  | a tool call | `→ ` | `tool: ` |
  | its result | `← ` | `result: ` or `failed: ` |
  | a status line | `• ` | `status: ` |
  | a file change | `• ` | `changed: ` |
  | a skill in use | `◆ skill ` | `skill: ` |
  | a notice | `· ` | none, `warning: ` or `error: ` |
  | a decision | the prompt | `approval needed: ` or `question: ` |

- Glyphs fall back to ASCII, and Markdown is printed as written rather than
  rendered.
- Input is read a line at a time by the terminal itself, so the screen
  reader's own line editing and review keys work. Answer an approval by
  typing its letter, or your own guidance, then Enter: `y` allow once, `s`
  allow for this session, `a` always in this repository, `n` deny.

Colour still follows `NO_COLOR`. Colour is never the only signal: diffs keep
their `+` and `-` gutters, and a failed result says `failed:` in plain mode.
The colours are the 16 ANSI ones, so the terminal's own palette, including a
high-contrast theme, applies.

### Reduced motion

`UAGENT_REDUCED_MOTION=1` replaces the spinner with a still label
("Working…") that changes only when the work it names changes. Plain mode
implies it. To keep either setting, save it once from a session:
`/config user UAGENT_PLAIN=1`. Both apply when a terminal next starts.

### Other

- `NO_COLOR` removes colour and keeps bold, dim and italic;
  `TERM=dumb` removes all styling.
- A locale that cannot decode UTF-8 gets ASCII glyphs everywhere.

## Web app

The app targets WCAG 2.2 AA, checked with axe-core on every surface a person
meets.

- **Zoom and contrast.** The viewport never forbids zoom, and Settings → This
  browser → Zoom scales the interface from 50 to 200%. Field, switch and
  option edges have a 3:1 or better border (`--field-border`), and muted text
  is at least 4.5:1 on every surface it sits on. `prefers-contrast: more`
  switches to a higher-contrast palette. In forced colours (Windows High
  Contrast), switches, the current item, pressed buttons and status lights
  keep their meaning through system colours and shape: a failed light is
  square.
- **Announcements.** A screen reader hears the edges of a turn
  ("Responding", "Needs your input", "Response complete"), never each
  streamed token. The transcript is a log that is busy while a turn runs. A
  new decision is an alert and takes focus, unless you are typing. Failures
  are alerts.
- **Names.** Each message is named by its author and time ("You, 10:32"),
  each row menu "Actions for …", and diffs summarise their `+N −M`. Failed
  tool rows say "Failed" in text and by icon, not only by colour.
- **Keyboard.** A skip link leads to the conversation. Ctrl+K (⌘K on a Mac)
  opens the command palette, `?` outside a text field lists every shortcut,
  and Alt+↑/↓ step through conversations. Menus open with the arrow keys on
  their button, jump by first letter, and close with Tab. The composer is a
  combobox that announces how many suggestions it has. Image markup places
  pins with the arrow keys and Enter. The remote browser screen has zoom and
  scroll buttons.
- **Structure.** Sheets are native `<dialog>`s and menus are popovers, so
  focus, Escape and the top layer come from the browser. Markdown headings
  sit below the page's own.
- **Motion.** `prefers-reduced-motion`, or Settings → This browser →
  Animations: Off, stops every animation and transition. A working spinner
  still turns.

## Tests

| Promise | Test |
| --- | --- |
| Plain mode: no cursor control, labelled rows, ASCII only | `test_plain_mode_writes_labelled_lines_without_cursor_control` in `tests/integration_ui.py` |
| Plain row marks | `tests/unit/presentation_test.cc` |
| `NO_COLOR` keeps attributes | `test_no_color_keeps_bold_italic_and_dim` in `tests/integration_ui.py` |
| ASCII fallback | `test_non_utf8_locale_draws_only_ascii` in `tests/integration_ui.py` |
| Web: no axe WCAG 2.2 AA violations in the component showcase, chat, decisions, ask, settings, the drawer; keyboard menus, skip link, forced-colours switch, viewer scroll buttons; axe absent from `dist/` | `web/tests/a11y.spec.js` |
