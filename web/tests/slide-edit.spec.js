// A slide's elements changed by hand, made in its file's text: slideEdit in
// doc-edit.ts as the browser runs it, with no app and no host.
import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";

test.beforeEach(async ({ page }) => {
  await page.route(`${DEV_SERVER_URL}/slide-edit-test`, (route) =>
    route.fulfill({
      contentType: "text/html",
      body: "<!doctype html><title>slide-edit</title>",
    }),
  );
  await page.goto(`${DEV_SERVER_URL}/slide-edit-test`);
});

// The file's text after the change, or null where it is refused.
const edited = (page, source, change) =>
  page.evaluate(
    async ([module, source, change]) =>
      (await import(module)).slideEdit(source, change),
    ["/src/features/files/doc-edit.ts", source, change],
  );
const slide = (body, artboard = "1920x1080 slide") =>
  `<!doctype html>\n<html><head><meta name="artboard" content="${artboard}"><title>T</title></head><body>${body}<aside>Say it.</aside></body></html>\n`;
const CARD =
  '<div class="card"><h2>One</h2><p id="first">First step.</p></div>';
const SLIDE = slide(
  `<main><h1>Plan</h1>${CARD}<ul><li>a</li><li>b</li></ul></main>`,
);
const title = { path: "main > h1", tag: "h1", text: "Plan" };
const card = { path: "main > div", tag: "div", text: "OneFirststep." };
const list = { path: "main > ul", tag: "ul", text: "ab" };
const set = (at, move, more) => ({ set: [{ at, move, ...more }] });

test("a move is the element's translate, taken away at none; a size is its width, a shape's height too", async ({
  page,
}) => {
  const moved = await edited(page, SLIDE, set(title, [40.4, -20]));
  expect(moved).toBe(
    SLIDE.replace("<h1>", '<h1 style="translate: 40px -20px;">'),
  );
  expect(await edited(page, moved, set(title, [0, 0]))).toBe(SLIDE);
  const wide = await edited(page, SLIDE, set(list, [0, 0], { width: 300 }));
  expect(wide).toBe(SLIDE.replace("<ul>", '<ul style="width: 300px;">'));
  expect(
    await edited(page, wide, set(list, [0, 0], { width: null, height: null })),
  ).toBe(SLIDE);
  // Text follows its width: only a shape takes a height.
  expect(await edited(page, SLIDE, set(list, [0, 0], { height: 90 }))).toBe(
    null,
  );
  const shaped = slide(
    '<h1>T</h1><div data-shape="rect" style="width: 10px;"></div>',
  );
  const shape = { path: "div", tag: "div", text: "" };
  expect(
    await edited(page, shaped, set(shape, [1, 2], { width: 20, height: 30 })),
  ).toBe(
    shaped.replace(
      'style="width: 10px;"',
      'style="width: 20px; translate: 1px 2px; height: 30px;"',
    ),
  );
  // An element of its own id is found by it.
  expect(
    await edited(
      page,
      SLIDE,
      set({ path: "#first", tag: "p", text: "Firststep." }, [5, 0]),
    ),
  ).toContain('<p id="first" style="translate: 5px;">');
});

test("an element is changed only where the file has it as the page showed it", async ({
  page,
}) => {
  const refused = (at, source = SLIDE) =>
    edited(page, source, set(at, [10, 10]));
  // Its text, its tag or its place changed meanwhile.
  expect(await refused({ ...title, text: "Plans" })).toBe(null);
  expect(await refused({ ...title, tag: "h2" })).toBe(null);
  expect(await refused({ ...title, path: "main > h1:nth-of-type(2)" })).toBe(
    null,
  );
  // Not the slide's notes, nor what holds its content, nor a file that is
  // no slide.
  expect(await refused({ path: "aside", tag: "aside", text: "Sayit." })).toBe(
    null,
  );
  expect(
    await refused({ path: "main", tag: "main", text: "PlanOneFirststep.ab" }),
  ).toBe(null);
  expect(await refused(title, SLIDE.replace(" slide", ""))).toBe(null);
  // One of several that is not there refuses all.
  expect(
    await edited(page, SLIDE, {
      set: [
        { at: title, move: [1, 1] },
        { at: { ...list, text: "x" }, move: [1, 1] },
      ],
    }),
  ).toBe(null);
});

test("a text, a table and each shape are put in placed by hand, last in the slide's content before its notes", async ({
  page,
}) => {
  const add = (kind, [width, height], source = SLIDE) =>
    edited(page, source, { add: { kind, place: [100.2, 200], width, height } });
  expect(await add("text", [480])).toBe(
    SLIDE.replace(
      "</ul></main>",
      '</ul><p style="position: absolute; left: 100px; top: 200px; width: 480px;">Text</p></main>',
    ),
  );
  expect(await add("table", [640])).toContain(
    '</ul><table style="position: absolute; left: 100px; top: 200px; width: 640px;"><thead><tr><th>Column</th><th>Column</th></tr></thead><tbody><tr><td>Cell</td><td>Cell</td></tr>',
  );
  for (const [kind, corners] of [
    ["rect", ""],
    ["round", " border-radius: 24px;"],
    ["ellipse", " border-radius: 50%;"],
  ])
    expect(await add(kind, [240, 160])).toContain(
      `</ul><div data-shape="${kind}" style="background: var(--accent, currentcolor);${corners} position: absolute; left: 100px; top: 200px; width: 240px; height: 160px;"></div></main>`,
    );
  // Without a wrapper, before the notes.
  expect(await add("text", [480], slide("<h1>T</h1>"))).toContain(
    '<h1>T</h1><p style="position: absolute; left: 100px; top: 200px; width: 480px;">Text</p><aside>',
  );
  // A shape has a height; a kind is one of those.
  expect(await add("rect", [240])).toBe(null);
  expect(await add("img", [240, 160])).toBe(null);
  expect(await add('rect"><script>x()</script>', [240, 160])).toBe(null);
  expect(await add("text", [0])).toBe(null);
});

test("copies are placed by hand after their originals, without their ids; deleted elements are gone", async ({
  page,
}) => {
  const copied = await edited(page, SLIDE, {
    copy: [
      { at: card, place: [148, 300], width: 600 },
      { at: list, place: [20, 20], width: 300 },
    ],
  });
  expect(copied).toBe(
    SLIDE.replace(
      CARD,
      `${CARD}<div class="card" style="position: absolute; left: 148px; top: 300px; width: 600px;"><h2>One</h2><p>First step.</p></div>`,
    ).replace(
      "</ul></main>",
      '</ul><ul style="position: absolute; left: 20px; top: 20px; width: 300px;"><li>a</li><li>b</li></ul></main>',
    ),
  );
  expect(
    await edited(page, SLIDE, {
      copy: [{ at: card, place: [148, 300], width: 600, height: 40 }],
    }),
  ).toBe(null);
  expect(await edited(page, SLIDE, { remove: [card, list] })).toBe(
    SLIDE.replace(CARD, "").replace("<ul><li>a</li><li>b</li></ul>", ""),
  );
});

test("what was put in by hand is laid over or under the rest put in beside it; nothing else is", async ({
  page,
}) => {
  const placed = (text) =>
    `<p style="position: absolute; left: 0px; top: 0px; width: 100px;">${text}</p>`;
  const source = slide(
    `<main><h1>Plan</h1>${placed("A")}${placed("B")}</main>`,
  );
  const a = { path: "main > p:nth-of-type(1)", tag: "p", text: "A" };
  const b = { path: "main > p:nth-of-type(2)", tag: "p", text: "B" };
  const swapped = source.replace(
    `${placed("A")}${placed("B")}`,
    `${placed("B")}${placed("A")}`,
  );
  expect(await edited(page, source, { front: [a] })).toBe(swapped);
  expect(await edited(page, source, { back: [b] })).toBe(swapped);
  // Already there, or not put in by hand.
  expect(await edited(page, source, { front: [b] })).toBe(null);
  expect(await edited(page, source, { front: [title] })).toBe(null);
});

test("the file keeps its byte order mark and its line ends", async ({
  page,
}) => {
  const source = `\uFEFF<!doctype html>\r\n<html><head><meta name="artboard" content="1920x1080 slide"></head>\r\n<body>\r\n<h1>T</h1></body></html>\r\n`;
  expect(
    await edited(
      page,
      source,
      set({ path: "h1", tag: "h1", text: "T" }, [1, 0]),
    ),
  ).toBe(source.replace("<h1>", '<h1 style="translate: 1px;">'));
});

test("what the page says is taken only as paths, checks, bounded numbers and kinds", async ({
  page,
}) => {
  const refused = (change) => edited(page, SLIDE, change);
  for (const move of [
    [Number.POSITIVE_INFINITY, 0],
    [Number.NaN, 0],
    [1e9, 0],
    ["10px", 0],
    [1],
    "1 1",
  ])
    expect(await refused(set(title, move))).toBe(null);
  for (const width of [-5, 0, "300px", Number.NaN, 1e9])
    expect(await refused(set(list, [0, 0], { width }))).toBe(null);
  for (const path of [7, { toString: "main > h1" }, "main >", "", null])
    expect(await refused(set({ ...title, path }, [1, 1]))).toBe(null);
  expect(
    await refused({
      set: Array.from({ length: 1001 }, () => ({ at: title, move: [1, 1] })),
    }),
  ).toBe(null);
  expect(await refused({ set: [] })).toBe(null);
  expect(await refused({ remove: "main > h1" })).toBe(null);
  expect(await refused({ copy: [{ at: card, place: [0, 0] }] })).toBe(null);
  expect(await refused({ unknown: [] })).toBe(null);
});
