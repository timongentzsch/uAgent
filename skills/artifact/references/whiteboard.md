# A whiteboard

A whiteboard is a page of notes, shapes and arrows laid out by position: a
flow, a map, a retro, a plan. Alone it is a folder with one page,
`boards/<name>/board.html`; inside a design it is one more page of that
folder (`design/landing/00-board.html`) and lies on the canvas beside the
screens. Files shows it as a board the person pans and zooms; with Edit on
they add, move, resize, retype, recolour and delete items and draw arrows,
which writes the file.

## The file

```html
<!doctype html>
<meta charset="utf-8">
<meta name="artboard" content="1600x1000">
<meta name="board">
<title>Signup flow</title>
<h1 id="title" data-at="40 32">Signup flow</h1>
<section id="web" data-at="40 120" data-size="760 420"><h2>Web</h2></section>
<div id="ad" data-shape="pill" data-at="80 200" data-size="160 64">Ad click</div>
<div id="form" data-shape="rect" data-at="320 200" data-size="180 80">Sign-up form</div>
<div id="ok" data-shape="diamond" data-at="580 190" data-size="160 100">Valid?</div>
<div id="db" data-shape="database" data-at="580 380" data-size="160 100">Leads</div>
<p id="n1" class="note" data-fill="yellow" data-at="880 120">Which fields are required?</p>
<small id="key" data-at="40 560">Pill: where a visit starts</small>
<hr data-from="ad" data-to="form">
<hr data-from="form" data-to="ok">
<hr data-from="ok" data-to="db" data-label="yes">
<hr data-from="ok" data-to="form" data-label="no" data-dash>
```

- `<meta name="board">` after the artboard line makes the page a
  whiteboard. The artboard is the board's size; leave room around the
  items.
- Every item is a direct child of `<body>` with an `id` (lower-case, unique
  on the page) and `data-at="x y"`: its top left corner in pixels from the
  board's top left. Text, headings and sections are items too: each needs
  both, or the person cannot move it and no arrow can reach it. Most items also take `data-size="width height"`; left
  out, the item takes its content's size. Later items lie over earlier
  ones.
- No scripts and no styles of your own: the viewer draws every item.

## Items

| Item | Written | Notes |
| --- | --- | --- |
| Note | `<p class="note" data-fill="yellow">` | a sticky note, 160x160 unless sized; fills `yellow`, `orange`, `pink`, `green`, `blue`, `grey` |
| Text | `<h1>` title, `<h2>` large, `<p>` body, `<small>` small | plain text on the board |
| Section | `<section data-size="w h"><h2>Name</h2></section>` | a named area drawn behind the items over it; it holds only its name |
| Shape | `<div data-shape="rect">Label</div>` | `rect`, `rounded`, `pill`, `ellipse`, `diamond`, `hexagon`, `parallelogram`, `database`, `queue`, `document`, `cloud`, `person`; a `data-fill` as for notes |
| Table | `<table>` with `<th>` and `<td>` | a plain table, sized by its content |
| Picture | `<img src="screen.png" alt="…" data-size="w h">` | an image file in the folder |
| Link | `<a href="01-hero.html">Hero</a>` | a card that opens that file of the folder |
| Widget | `<div class="widget" data-size="w h">…</div>` | a small piece of HTML drawn as it is: a list, a counter, a bar |
| Arrow | `<hr data-from="a" data-to="b">` | from one item's id to another's |

An arrow needs no `id` or `data-at`: it runs between the two items' nearest
sides. `data-label="yes"` writes a label on it, `data-dash` dashes it, and
`data-head` sets its heads: `end` (the default), `both`, `none`. Write the
arrows after the items they join.

## Laying it out

- Read across, then down: a flow runs left to right in one row of the same
  height, a branch drops below it.
- Keep 40px or more between items and 80px between sections; align items
  on a 20px grid.
- One idea per note, five words or so; a longer thought is a text item.
- Shapes carry a flow's steps; notes carry open questions and ideas beside
  it. Use one shape per kind of step, and say which in a small text item if
  it is not plain.
- A board of more than about 30 items gets sections.
