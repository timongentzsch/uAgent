# A design

A design is a folder of pages, `design/<name>/`: one flow or product
surface, e.g. `design/landing/` or `design/onboarding/`. Files shows the
folder as a canvas, every page a live artboard; a document, a deck's folder,
an image or a video inside it lies on the canvas too.

No look given and nothing to match: offer 2–4 genuinely different directions
as separate files in one row, each exploring an axis you can name, and say
what each is for in your reply. Keep option names stable across turns.

## Pages

- Name pages `NN-screen[--state].html`:
  - `NN` is the step, two digits; it is the page's row on the canvas, so the
    variants and states of a step share it and sit side by side.
  - `screen` says what the page is; a `--state` (`--error`, `--empty`,
    `--annual`) marks a state or variant of it.
  - `design/onboarding/`: `01-welcome.html`, `02-sign-in.html`,
    `02-sign-in--error.html`, `03-workspace--empty.html`. Two directions of
    one screen: `01-hero-numbers.html`, `01-hero-session.html`.
  - A page without a step sits in the first row.
- `<meta name="artboard" content="1440x900">` fixes a page's size: a phone
  screen 390x844, a desktop screen 1440x900, a square post 1080x1080.
  `content="1440"` (a width only) makes a page fluid, its height its
  content, laid out with `max-width`, `%`, `fr` and `rem` so it also works
  at a phone's width.
- Pages of one design may share a stylesheet in the folder (`style.css`); a
  change to it reloads every page.
- Markup first. Draw every screen, card and row in HTML; scripts only for
  behaviour (state, a toggle, a carousel), never to build the page.
- Links between files (`<a href="02-sign-in.html">`) make a clickable
  prototype; a flow that shares state is one file with sections it shows and
  hides.
- Accessible as drawn (WCAG AA): real controls with labels, 44px touch
  targets on phone screens. Never draw a fake status bar or keyboard on a
  phone screen.

## Tweaks: levers the person can try

Declare a few levers that cut across the design — an accent, a density, a
variant, a dark theme — in the page's head. Not copy, not a colour used once:

```html
<script type="application/json" id="tweaks">
{ "--accent":  { "editor": "color", "default": "#2f6fde", "options": ["#e8590c", "#2f6fde", "#111111"] },
  "--density": { "editor": "range", "default": 1, "min": 0.8, "max": 1.3, "step": 0.05, "unit": "×" },
  "variant":   { "editor": "enum", "default": "split", "options": ["split", "stacked"] },
  "phone":     { "editor": "boolean", "default": true } }
</script>
```

- Editors: `color` (3–4 `options` show as swatches), `range` (`int` and
  `float` too, with `min`, `max`, `step`, `unit`), `enum`, `boolean`.
- A name starting with `--` is a CSS variable on `:root`: define it in your
  CSS with the same default and use `var(--accent)`. Ranges set a bare
  number and `unit` is only shown, so give the unit in CSS:
  `gap: calc(var(--gap) * 1px)`. Any other name is a `data-` attribute on
  `<html>` (`data-variant="stacked"`, `data-phone="false"`) that your CSS
  selects on.
- `default` is the current value: when the person keeps a value, write it
  there and in the CSS.
