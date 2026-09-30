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
  | a file change | `• ` | `changed: ` |
  | a notice | `· ` | none, `warning: ` or `error: ` |
  | a decision | the prompt | `approval needed: ` or `question: ` |

- Glyphs fall back to ASCII, and Markdown is printed as written rather than
  rendered.
- Input is read a line at a time by the terminal itself, so the screen
  reader's own line editing and review keys work. Answer a decision by typing
  its letter (`y`, `s`, `a`, `n`) or your own guidance, then Enter.

Colour still follows `NO_COLOR`. Colour is never the only signal: diffs keep
their `+` and `-` gutters, and a failed result says `failed:` in plain mode.
The colours are the 16 ANSI ones, so the terminal's own palette, including a
high-contrast theme, applies.

### Reduced motion

`UAGENT_REDUCED_MOTION=1` replaces the spinner with a still label
("Working…") that changes only when the work it names changes. Plain mode
implies it.

### Other

- `NO_COLOR` removes colour and keeps bold, dim and italic;
  `TERM=dumb` removes all styling.
- A locale that cannot decode UTF-8 gets ASCII glyphs everywhere.

## Web app

- Native `<dialog>` sheets and a popover menu, so focus, Escape and the
  top layer come from the browser.
- `prefers-reduced-motion` disables the sheet and drawer animations.
- The ask form follows the ARIA radio and checkbox group patterns.

## Tests

| Promise | Test |
| --- | --- |
| Plain mode: no cursor control, labelled rows, ASCII only | `test_plain_mode_writes_labelled_lines_without_cursor_control` in `tests/integration_ui.py` |
| Plain row marks | `presentation_test.cc` |
| `NO_COLOR` keeps attributes | `test_no_color_keeps_bold_italic_and_dim` |
| ASCII fallback | `test_non_utf8_locale_draws_only_ascii` |
