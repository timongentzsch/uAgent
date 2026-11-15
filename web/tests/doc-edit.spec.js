// A block's edit applied to a file's text, and the saver that writes it:
// doc-edit.ts as the browser runs it, with no app and no host.
import { devServerTest as test, expect, DEV_SERVER_URL } from "./fixtures.js";

test.beforeEach(async ({ page }) => {
  await page.route(`${DEV_SERVER_URL}/doc-edit-test`, (route) =>
    route.fulfill({
      contentType: "text/html",
      body: "<!doctype html><title>doc-edit</title>",
    }),
  );
  await page.goto(`${DEV_SERVER_URL}/doc-edit-test`);
});

const MODULE = "/src/features/files/doc-edit.ts";
// The file's text after an edit, or null where the edit is refused.
const edited = (page, source, edit, wrote) =>
  page.evaluate(
    async ([module, source, edit, wrote]) => {
      const { applyEdit } = await import(module);
      const made = applyEdit(source, edit, (at) => wrote?.[at]);
      return made && made.text;
    },
    [MODULE, source, edit, wrote],
  );
const page1 = (body, head = "") =>
  `<!doctype html>\n<html><head><title>T</title>${head}</head><body>${body}</body></html>\n`;
// What follows `text` is typed after the block's own.
const typed = (index, before, html = `${before}!`) => ({ index, before, html });

test("a save writes the document's own nodes: its comments stay where they stood, and the file keeps its ending", async ({
  page,
}) => {
  // A comment that names <html> before the doctype; one after </html>.
  expect(
    await edited(
      page,
      "<!-- the <html> wrapper is written by hand -->\n<!doctype html>\n" +
        "<html><!-- head next --><head><title>T</title></head><body><p>Hi</p></body></html>\n<!-- end -->\n",
      typed(0, "Hi"),
    ),
  ).toBe(
    "<!-- the <html> wrapper is written by hand -->\n<!doctype html>\n" +
      "<html><!-- head next --><head><title>T</title></head><body><p>Hi!</p></body></html>\n<!-- end -->\n",
  );
  // No <html> tag at all, and no line at the file's end.
  expect(
    await edited(
      page,
      "<!-- note -->\n<!doctype html>\n<title>T</title>\n<p>Hi",
      typed(0, "Hi"),
    ),
  ).toBe(
    "<!-- note -->\n<!doctype html>\n<html><head><title>T</title>\n</head><body><p>Hi!</p></body></html>",
  );
  // A byte order mark stays first, and a file of CRLF lines stays one.
  expect(
    await edited(
      page,
      "\uFEFF<!doctype html>\r\n<html>\r\n<head>\r\n<title>T</title>\r\n</head>\r\n" +
        "<body>\r\n<p>Hi</p>\r\n<pre>\r\n\r\ncode</pre>\r\n</body>\r\n</html>\r\n",
      typed(0, "Hi"),
    ),
  ).toBe(
    "\uFEFF<!doctype html>\r\n<html><head>\r\n<title>T</title>\r\n</head>\r\n" +
      "<body>\r\n<p>Hi!</p>\r\n<pre>\r\n\r\ncode</pre></body></html>\r\n",
  );
  // Saved again with nothing changed, the text is the same.
  const once = await edited(page, page1("<p>Hi</p>\n\n"), typed(0, "Hi"));
  expect(once).toBe(page1("<p>Hi!</p>"));
  expect(await edited(page, once, typed(0, "Hi!", "Hi!"))).toBe(once);
});

test("a pre keeps the blank line it starts with through any number of saves", async ({
  page,
}) => {
  const source = page1(
    "<p>Hi</p><pre>\n\ncode</pre><textarea>\n\ntext</textarea>",
  );
  const once = await edited(page, source, typed(0, "Hi"));
  expect(once).toContain("<pre>\n\ncode</pre><textarea>\n\ntext</textarea>");
  const twice = await edited(page, once, typed(0, "Hi!"));
  expect(twice).toContain("<pre>\n\ncode</pre><textarea>\n\ntext</textarea>");
});

test("an edit is made only where the file still has its place", async ({
  page,
}) => {
  const cells = (...texts) =>
    page1(texts.map((text) => `<p>${text}</p>`).join(""));
  const third = { ...typed(2, "x"), of: 3, around: ["x", undefined] };
  expect(await edited(page, cells("x", "x", "x"), third)).toContain(
    "<p>x</p><p>x</p><p>x!</p>",
  );
  // The agent put a block in before them: the third is another now.
  expect(await edited(page, cells("new", "x", "x", "x"), third)).toBeNull();
  // As many blocks, but another before it.
  expect(await edited(page, cells("x", "y", "x"), third)).toBeNull();
  // The block before it as this last wrote it is the page's own.
  expect(await edited(page, cells("x", "y", "x"), third, { 1: "y" })).toContain(
    "<p>y</p><p>x!</p>",
  );
  // Its own text changed.
  expect(await edited(page, cells("x", "x", "z"), third)).toBeNull();
  expect(await edited(page, cells("x", "x", "z"), third, { 2: "z" })).toContain(
    "<p>x!</p>",
  );
  // What a noscript holds is text in the page: no block of it.
  expect(
    await edited(
      page,
      page1("<noscript><p>Enable scripts</p></noscript><p>Hi</p>"),
      { ...typed(0, "Hi"), of: 1, around: [] },
    ),
  ).toContain("<noscript><p>Enable scripts</p></noscript><p>Hi!</p>");
});

test("a block turned into another kind, or followed by a new one, leaves valid HTML", async ({
  page,
}) => {
  const body = async (inner, edit) =>
    /<body>(.*)<\/body>/s.exec(await edited(page, page1(inner), edit))[1];
  // An item's own paragraphs take its place; a paragraph holds none.
  expect(
    await body("<ul><li><p>a</p><p>b</p></li><li>c</li></ul>", {
      index: 0,
      before: "ab",
      html: "<p>a</p><p>b</p>",
      as: "p",
    }),
  ).toBe("<p>a</p><p>b</p><ul><li>c</li></ul>");
  // A list in the item follows the paragraph.
  expect(
    await body("<ul><li>a<ul><li>b</li></ul></li></ul>", {
      index: 0,
      before: "ab",
      html: "a<ul><li>b</li></ul>",
      as: "p",
    }),
  ).toBe("<p>a</p><ul><li>b</li></ul>");
  // A numbered list counts on after the break.
  expect(
    await body(
      '<ol start="5" id="steps"><li>one</li><li>two</li><li>three</li></ol>',
      { ...typed(1, "two", "two"), as: "h2" },
    ),
  ).toBe(
    '<ol start="5" id="steps"><li>one</li></ol><h2>two</h2><ol start="6"><li>three</li></ol>',
  );
  // An item's own number is counted from; a list that counts down is left
  // as it is.
  expect(
    await body(
      '<ol><li value="10">ten</li><li>eleven</li><li>twelve</li></ol>',
      { ...typed(1, "eleven", "eleven"), as: "p" },
    ),
  ).toBe(
    '<ol><li value="10">ten</li></ol><p>eleven</p><ol start="11"><li>twelve</li></ol>',
  );
  expect(
    await body("<ol reversed><li>one</li><li>two</li><li>three</li></ol>", {
      ...typed(1, "two", "two"),
      as: "p",
    }),
  ).toBe(
    '<ol reversed=""><li>one</li></ol><p>two</p><ol reversed=""><li>three</li></ol>',
  );
  // After the whole figure a cell is part of, not inside it.
  expect(
    await body(
      "<figure><table><tbody><tr><td>cell</td></tr></tbody></table><figcaption>cap</figcaption></figure><p>end</p>",
      { ...typed(0, "cell", "cell"), insert: "p" },
    ),
  ).toBe(
    "<figure><table><tbody><tr><td>cell</td></tr></tbody></table><figcaption>cap</figcaption></figure><p>Paragraph</p><p>end</p>",
  );
});

test("a slide's notes are lines, and no block of the slide; a new slide is laid out as the deck's are", async ({
  page,
}) => {
  const slide = (body) =>
    page1(body, '<meta name="artboard" content="1920x1080 slide">');
  const notes = (before, html) => ({ index: -1, before, html });
  // Paragraphs where the aside held elements, each line one; never markup.
  expect(
    await edited(
      page,
      slide("<h1>T</h1><aside><ul><li>a</li><li>b</li></ul></aside>"),
      notes("a\nb", "a\n<b>c</b>\n\nd"),
    ),
  ).toContain("<aside><p>a</p><p>&lt;b&gt;c&lt;/b&gt;</p><p>d</p></aside>");
  // An aside's lines are all its text: what stands beside its blocks too.
  expect(
    await edited(
      page,
      slide("<h1>T</h1><aside><strong>Say:</strong> <p>one</p></aside>"),
      notes("Say:\none", "Say:\none\ntwo"),
    ),
  ).toContain("<aside><p>Say:</p><p>one</p><p>two</p></aside>");
  // Text where it held text; made where the slide has none.
  expect(
    await edited(page, slide("<h1>T</h1><aside>a</aside>"), notes("a", "a\nb")),
  ).toContain("<aside>a\nb</aside>");
  expect(await edited(page, slide("<h1>T</h1>"), notes("", "new"))).toContain(
    "<h1>T</h1><aside>new</aside></body>",
  );
  // Not on a page that is no slide.
  expect(await edited(page, page1("<p>Hi</p>"), notes("", "new"))).toBeNull();
  // The aside's paragraphs are not counted with the slide's blocks.
  expect(
    await edited(
      page,
      slide("<aside><p>n</p></aside><h1>T</h1><aside><p>a</p></aside>"),
      {
        ...typed(1, "T"),
        of: 2,
      },
    ),
  ).toContain("<h1>T!</h1>");
  // A new slide: inside a copy of the one element that wraps the shown one.
  const blank = (source) =>
    page.evaluate(
      async ([module, source]) => (await import(module)).blankSlide(source),
      [MODULE, source],
    );
  expect(
    await blank(
      slide(
        '<main class="slide two"><h1>T</h1><p>x</p></main><aside>n</aside>',
      ),
    ),
  ).toContain(
    '<body><main class="slide two"><h1>Heading</h1><p>Text</p></main></body>',
  );
  expect(await blank(slide("<h1>T</h1><aside>n</aside>"))).toContain(
    "<body><h1>Heading</h1><p>Text</p></body>",
  );
});

// The saver over a file kept in the page (doc-edit-harness.js).
const HARNESS = "/tests/doc-edit-harness.js";

test("each write reads the file first, so what the agent wrote meanwhile is kept", async ({
  page,
}) => {
  const said = await page.evaluate(async (harness) => {
    const { saverOn, doc, edit, outcome } = await import(harness);
    const { saver, file, calls, body } = saverOn();
    const made = outcome(saver.save("f", edit(0, "a", "a1")));
    // The agent rewrote the other block while this one was typed in.
    file.text = doc("a", "b by the agent");
    file.revision = 7;
    return [await saver.flush(), await made, calls, body()];
  }, HARNESS);
  expect(said).toEqual([
    true,
    { at: 0 },
    ["read 7", "write on 7"],
    "<p>a1</p><p>b by the agent</p>",
  ]);
});

test("a block that was refused stays owed, with why, until it is written or forgotten; a refused kind change is not", async ({
  page,
}) => {
  const said = await page.evaluate(async (harness) => {
    const { saverOn, edit, outcome } = await import(harness);
    const { saver, refuse, body } = saverOn();
    refuse(1);
    const first = outcome(saver.save("f", edit(0, "a", "a1")));
    // Another block is saved, and says what is still owed.
    const second = outcome(saver.save("f", edit(1, "b", "b1")));
    const owing = [await saver.flush(), await saver.flush()];
    saver.forget();
    const forgotten = await saver.flush();
    const third = outcome(saver.save("f", edit(1, "b", "b2")));
    await saver.flush();
    // Owed again once refused again, and paid by a write of that block.
    refuse(1);
    const again = outcome(saver.save("f", edit(0, "a", "a2")));
    owing.push(await saver.flush());
    const paid = outcome(saver.save("f", edit(0, "a", "a3")));
    const whole = await saver.flush();
    // Another kind that is refused takes nothing typed with it.
    refuse(1);
    const turned = outcome(
      saver.save("f", { ...edit(0, "a", "a3"), as: "h2" }),
    );
    return [
      await first,
      await second,
      owing,
      forgotten,
      await third,
      await again,
      await paid,
      whole,
      await saver.flush(),
      await turned,
      body(),
    ];
  }, HARNESS);
  expect(said).toEqual([
    "Not saved: refused",
    { at: 1, owed: "refused" },
    [false, false, false],
    true,
    { at: 1 },
    "Not saved: refused",
    { at: 0 },
    true,
    true,
    "Not saved: refused",
    "<p>a3</p><p>b2</p>",
  ]);
});

test("a closing page writes into the file as last seen here, when it was seen and is small", async ({
  page,
}) => {
  const said = await page.evaluate(async (harness) => {
    const { saverOn, edit, outcome } = await import(harness);
    // Never read: not attempted.
    const unseen = saverOn();
    const lost = outcome(unseen.saver.save("f", edit(0, "a", "a1")));
    await unseen.saver.flush(true);
    // Written once: the next closes on what that write left, with no read.
    const seen = saverOn();
    void seen.saver.save("f", edit(0, "a", "a1"));
    await seen.saver.flush();
    void seen.saver.save("f", edit(1, "b", "b1"));
    await seen.saver.flush(true);
    // Too large for a closing page to send: not attempted either.
    const large = saverOn();
    void large.saver.save("f", edit(0, "a", "a1"));
    await large.saver.flush();
    const cut = outcome(
      large.saver.save("f", edit(1, "b", "x".repeat(50_000))),
    );
    await large.saver.flush(true);
    return [
      typeof (await lost),
      unseen.calls,
      seen.calls,
      seen.body(),
      typeof (await cut),
      large.calls,
    ];
  }, HARNESS);
  expect(said).toEqual([
    "string",
    [],
    ["read 0", "write on 0", "write on 1, closing"],
    "<p>a1</p><p>b1</p>",
    "string",
    ["read 0", "write on 0"],
  ]);
});
