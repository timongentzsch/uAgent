# A slide deck

A deck is a folder of slides, `decks/<name>/` (in a workspace, any folder):
one HTML file per slide, shown on the canvas in name order and presented
from it. A new deck is a new folder: change an existing deck, or a file that
looks like one, only when the person asks to change it.

## Files

- Name slides `NN-name.html`: `01-cover.html`, `02-agenda.html`,
  `03-revenue.html`. `NN` is the order; to insert a slide, renumber the
  files after it. The `<title>` is the slide's readable name.
- Each slide is a complete HTML document with
  `<meta name="artboard" content="1920x1080 slide">`: 16:9 unless the person
  asks for another format (4:3 `1440x1080`, square `1080x1080`, vertical
  `1080x1920`, or the size they give). Every slide of a deck has the same size.
- Slides share one `style.css` in the folder, which links the design system
  (`@import url("../../design/system/tokens.css");` or the system's path) and
  holds the deck's type scale and slide layouts as classes. A slide holds
  its content, not its styles.
- Slides are read from across a room: in `style.css` set the root size so the
  system's `rem` sizes scale with the slide (`html { font-size: 28px; }` on a
  1920px-wide slide, in proportion on others).
- Speaker notes: one `<aside>` as the last element of `<body>`, plain text,
  what the presenter says. It never shows on the slide.
- No scripts, no animation; the canvas fades between slides when presenting.

## Designing the deck

You are a presentation designer, not a web designer.

- Decide once, before the first slide: the outline (one sentence per
  section), the length, the audience and the 3–5 layouts you will repeat
  (title, statement, two columns, big number, table, image). Say what you
  assumed in one line.
- One idea per slide. A statement beats bullets; turn lists into a table,
  a row of cards, a big number or a quote. At most about 40 words of body
  text and at most 3 cards in a row, 2 rows, on a slide.
- A deck is not a document: when the source holds more than the slides can,
  choose what the audience needs and put the rest in the speaker notes;
  never cram.
- Titles introduce the topic in one grammar throughout, always at the same
  place (the top margin), never centred with the body.
- One type scale of four or five sizes; nothing under 24px on a 1920px-wide
  slide. Emphasise with weight or colour, not a new size.
- Margins of 128px; nothing closer to an edge except a full-bleed backdrop.
  A footer (page number, source) is one 24px row at the bottom margin.
- Fit before you write: 824px of height inside the margins. A heading takes
  about size × lines × 1.1; a body line about size × 1.4. Too much? Split
  the slide; never shrink the type.
- Colours, type and space from the design system's tokens. Without one: a
  dark and a light ground, one or two accents, toned whites and blacks.
- Charts and diagrams are inline SVG drawn in the markup, labelled with
  their values; images are files in the folder, `object-fit: cover`.
- No invented numbers or quotes: a missing fact is a visible placeholder
  such as [ARR] that you list in your reply.

## Checking

Open every slide you wrote with the `browser` tool's `open` and its `path`;
fix every listed problem (larger than its artboard, text running into other
text, into the footer or past the slide's edge, a missing file) and open it
again. Make no PDF or
images yourself: the canvas exports them. Then name the folder in your reply,
`decks/<name>/`, which opens the deck.
