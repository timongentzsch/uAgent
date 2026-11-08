# A design system

A design system is a folder, e.g. `design/system/`, that pages link:

- `README.md`: usage rules that name tokens ("buttons are `--accent` on
  `--page`"), voice, iconography, the source it came from and its commit,
  and a "Not synced" list of what could not be placed.
- `tokens.css`: the only token file.
  - Colours in a `:root, [data-theme="light"] { … }` block and one
    `[data-theme="<id>"] { … }` block per further theme.
  - `--space-*`, `--radius-*`, `--shadow-*` and `--font-<family>` variables.
  - A `.text-<style>` class per text style (`font`, `letter-spacing`).
  - One `/* usage */` comment after each declaration.
- `components/bundle.css` and, per component, `components/<Name>/README.md`
  (what it is for, when to use it, its classes) and
  `components/<Name>/preview.html`, a small page showing it in all its
  states. `<Name>` is the component's name as code uses it (`Button`,
  `ListRow`).
- `fonts/` and `assets/<Group>/` (logos, icons): copied files, never redrawn.
- `book.html`: the brand book (below), which the canvas shows and prints.
- Pages use it with `<link rel="stylesheet" href="../system/tokens.css">` (and
  `bundle.css`) and `data-theme` on `<html>`.

Building one from a codebase:

1. Ask once what to include if it is unclear: tokens, fonts, logos and icons,
   docs, which components.
2. Take tokens exactly as the code has them: CSS custom properties under
   `:root` and theme selectors (`[data-theme="dark"]`, `.dark`, a
   `prefers-color-scheme: dark` block), a token file or a theme object.
   Keep names, values and aliases (`var(--other)`); skip what is computed
   (`calc()`, `color-mix()`) and list it under "Not synced". Never invent a
   value.
3. Copy font files, logos and icons; condense the docs into usage rules.
4. Components: list them, ask which, and write each as CSS classes with a
   preview, from the code's real styles.
5. Write the brand book last, from what the folder now holds.
6. On a later re-sync, update changed values, add new ones, keep notes and
   prose, and list tokens the code no longer defines; ask before removing
   them. Bring the book up to date with them.

### The brand book

`book.html` is the system as a branding agency would hand it over: art
directed in the system's own voice, never a template.

- One document: `<meta name="artboard" content="794">` (A4 wide, as tall as
  it runs), `<meta name="print" content="flow a4">`, and links to
  `tokens.css` and `components/bundle.css`. Every page is a `<section>`
  exactly 210mm × 297mm with `break-after: page` and `overflow: hidden`; its
  running footer (title, page number) sits at the page's foot.
- Contents: a cover; the idea and principles (from README.md); colour, every
  colour a swatch with its token, value, use and contrast on the ground;
  type, every family and text style set large as a specimen; space and shape,
  the scales drawn to size; the components in use, from `bundle.css` classes
  or `components/<Name>/preview.html` in an `<iframe>`; a do and don't page.
- Every colour, size and face through `var(--…)`, so the book follows the
  tokens; a value appears only as a printed label.
- Art direction: full-bleed colour pages, type set large as image, the
  system's own accent and grid. No stock placeholders.
- Every page is full and fits: lay each out as a grid over its whole height
  (the cover too, its title, statement and strip spread to the foot). Budget
  its content before writing it; what would run past 297mm goes to a page of
  its own, and a page that runs short gets a full-bleed block or a larger
  specimen, never empty space.
- Names as written: no `text-transform` on a name or token.
