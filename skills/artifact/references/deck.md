# A slide deck

A deck is a folder of slides, `decks/<name>/`: one HTML file per slide.
Files shows the slide beside a legend of all of them, in name order, and
presents from there; the person can reorder, duplicate, add, skip and
delete slides, edit a slide's text and notes in place, and move, resize,
add and delete what is on a slide.

## Files

- Name slides `NN-name.html`: `01-cover.html`, `02-agenda.html`,
  `03-revenue.html`. `NN` is the order; to insert a slide, renumber the
  files after it. The `<title>` is the slide's readable name.
- Each slide has `<meta name="artboard" content="1920x1080 slide">`: 16:9 unless the person
  asks for another format (4:3 `1440x1080`, square `1080x1080`, vertical
  `1080x1920`, or the size they give). Every slide of a deck has the same size.
- Slides share one `style.css` in the folder, which links the design system
  (`@import url("../../design/system/tokens.css");` or the system's path) and
  holds the deck's type scale and slide layouts as classes. A slide holds
  its content, not its styles.
- What the person placed by hand carries it in its `style` attribute: a
  `translate`, a `width`, or `position: absolute` with `left` and `top`
  (a text, a table or a `data-shape` they put in). Keep those attributes
  and elements as they are when you change a slide; never write such a
  style yourself.
- Slides are read from across a room: in `style.css` set the root size so the
  system's `rem` sizes scale with the slide (`html { font-size: 28px; }` on a
  1920px-wide slide, in proportion on others).
- Write a slide's text directly in `h1`–`h3`, `p`, `li`, table cells,
  `figcaption` or `blockquote`: people edit those in place, block by block.
- Speaker notes: one `<aside>` as the last element of `<body>`, plain text
  or a paragraph a line, what the presenter says. It never shows on the
  slide.
- A slide to leave out of the presentation and the PDF, but keep in the
  deck, has `<meta name="slide" content="skip">` right after its artboard.
- A slide that opens a part of the talk names it after its artboard line:
  `<meta name="section" content="Results">`. The legend groups the slides
  under these names.
- No scripts and no animation of your own. Files fades between slides when
  presenting; `<meta name="slide" content="push">` slides this one in from
  the right instead (`content="skip push"` keeps both words).
- Builds: `data-step="1"` on an element hides it until the presenter's
  first click on that slide, `data-step="2"` until the second, and so on;
  elements with the same number appear together. Build a list item by item
  or a chart's second series, at most four steps. The PDF and the legend
  show every step.

## Designing the deck

- Identify the audience, what they should understand or decide, and whether
  they will read the deck alone or hear a presenter. A standalone deck
  defines unfamiliar terms and gives the reader the evidence on the slide.
  Put supporting detail for a live talk in the speaker notes.
- Use each reference for the purpose the person gave it. A product capture
  shows what happened, a source supplies facts, and a template supplies
  layout and typography. Preserve requested content, user-positioned
  elements, citations in notes and live data bindings through revisions.
- Before building out a new deck, make the slide that best tests the
  design: for a product walkthrough, the control and outcome a new reader
  needs to recognize. Write it with the shared stylesheet. Once the write
  succeeds, observe its HTML path and present it as an early preview. For a focused edit,
  work directly on the affected slides.
- Plan a short sequence with one point per slide. Give the most important
  evidence room. A recap can show the decision reached or the result
  produced. Keep the type scale and margins consistent with the reference
  and artboard size. Cut copy before reducing text below a readable size.
- Preserve the proportions of UI, logos and source figures. Show the
  relevant region at a readable scale, keeping enough context to orient
  the reader. Use cover crops for decorative images. Keep charts, tables
  and source-bound values editable where the brief calls for them.

## Review

Use `browser observe` on the deck folder for numbered thumbnails, titles
and per-slide diagnostics. Follow the returned range for a longer deck.
Read the titles in sequence and check the main point at presentation size.
Inspect a particular slide with `part="3"`, or observe its HTML path with
a `selector` such as `figure img` to inspect a detail. A zoom helps
diagnose blur; judge legibility in the full-slide view.

Link each slide-specific finding to its HTML path in the deck, for example
`[Slide 3: Pricing](decks/launch/03-pricing.html)`, so the reader can open
the slide and comment from its actions menu.

After a local edit, review just the changed slide. After changing shared
CSS or data, check all affected slides together. Verify that an editable
field still points to its source and that nearby claims agree with the
value shown. Present the finished deck folder.
