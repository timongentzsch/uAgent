import {
  test,
  showcaseTest,
  expect,
  online,
  SHOWCASE_URL,
  selectProbeModel,
} from "./fixtures.js";

// Geometry comes from style.css alone: a control or row centres its content
// in its box, an icon shares its text's centre line, and a one-line control
// is one of the sizes on the scale. These measure what a person sees, so a
// sheet that pads one side or sets its own height fails here.
function measure() {
  const rem = parseFloat(getComputedStyle(document.documentElement).fontSize);
  // The scale's sizes, in px at this zoom: --control-s, --control-m,
  // --control-l and --touch.
  const sizes = [24, 32, 40, 44].map((size) => (size / 14) * rem);
  const name = (el) =>
    [el.parentElement, el]
      .map(
        (e) =>
          e.tagName.toLowerCase() +
          [...e.classList].map((c) => `.${c}`).join(""),
      )
      .join(" > ");
  const visible = (el) => {
    const r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0 && r.bottom > 0 && r.top < innerHeight;
  };
  // The text and icons drawn inside an element, skipping what is hidden.
  const ink = (el) => {
    const box = { top: Infinity, bottom: -Infinity, lines: 0 };
    const walker = document.createTreeWalker(
      el,
      NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT,
      {
        acceptNode(node) {
          if (node.nodeType !== 1) return NodeFilter.FILTER_ACCEPT;
          const style = getComputedStyle(node);
          return style.display === "none" ||
            style.visibility === "hidden" ||
            style.opacity === "0" ||
            node.classList.contains("sr-only")
            ? NodeFilter.FILTER_REJECT
            : NodeFilter.FILTER_ACCEPT;
        },
      },
    );
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      let rects = [];
      if (node.nodeType === 1 && node.tagName === "svg")
        rects = [node.getBoundingClientRect()];
      else if (node.nodeType === 3 && node.textContent.trim()) {
        const range = document.createRange();
        range.selectNodeContents(node);
        rects = [...range.getClientRects()];
        box.lines += rects.length;
      }
      for (const r of rects) {
        if (!r.height) continue;
        box.top = Math.min(box.top, r.top);
        box.bottom = Math.max(box.bottom, r.bottom);
      }
    }
    return box.top === Infinity ? null : box;
  };
  const off = [];
  const controls = document.querySelectorAll(
    "button, summary, .row, .message.user, .sidebar footer",
  );
  for (const el of controls) {
    if (!visible(el)) continue;
    const style = getComputedStyle(el);
    // A text link drawn as a button (min-height 0) has no box to centre in.
    if (style.minHeight === "0px" && el.matches("button")) continue;
    const r = el.getBoundingClientRect();
    const drawn = ink(el);
    if (!drawn) continue;
    const top = drawn.top - r.top - parseFloat(style.borderTopWidth);
    const bottom =
      r.bottom - drawn.bottom - parseFloat(style.borderBottomWidth);
    // A font's glyph box sits up to 1.6px off its line's centre; padding
    // on one side shows as 3px or more.
    if (Math.abs(top - bottom) > 2)
      off.push(
        `${name(el)}: ${top.toFixed(1)}px above, ${bottom.toFixed(1)}px below`,
      );
    // One line of text, no detail line: the box is a size on the scale.
    const oneLine =
      drawn.lines <= 1 && !el.querySelector(".row-detail, .message-input");
    if (
      oneLine &&
      !el.matches(".message.user, .sidebar footer") &&
      !sizes.some((size) => Math.abs(r.height - size) < 1)
    )
      off.push(`${name(el)}: ${r.height.toFixed(1)}px tall`);
  }
  // An icon beside a word in a control shares its centre line.
  for (const svg of document.querySelectorAll(
    ":is(button, summary, .row) > svg, :is(button, summary, .row) > span > svg",
  )) {
    if (!visible(svg)) continue;
    const parent = svg.parentElement;
    const word = [...parent.childNodes].find(
      (node) =>
        (node.nodeType === 3 && node.textContent.trim()) ||
        (node.nodeType === 1 &&
          node.tagName !== "svg" &&
          !node.classList.contains("sr-only") &&
          node.getClientRects().length === 1 &&
          node.textContent.trim()),
    );
    if (!word) continue;
    const range = document.createRange();
    range.selectNodeContents(word);
    const text = range.getClientRects();
    if (text.length !== 1) continue;
    const s = svg.getBoundingClientRect();
    const delta = (s.top + s.bottom) / 2 - (text[0].top + text[0].bottom) / 2;
    if (Math.abs(delta) > 2)
      off.push(`${name(parent)}: icon ${delta.toFixed(1)}px off its text`);
  }
  return [...new Set(off)];
}

for (const width of [1280, 390]) {
  showcaseTest(
    `the showcase's controls and rows keep the scale at ${width}px`,
    async ({ page }) => {
      await page.setViewportSize({ width, height: 900 });
      await page.goto(SHOWCASE_URL);
      await expect(
        page.getByRole("heading", { name: "µAgent UI showcase", exact: true }),
      ).toBeVisible();
      // Every screen of the page, top to bottom.
      const scroller = page.locator("#app");
      const height = await scroller.evaluate((el) => el.scrollHeight);
      const found = [];
      for (let y = 0; y < height; y += 800) {
        await scroller.evaluate((el, top) => el.scrollTo(0, top), y);
        found.push(...(await page.evaluate(measure)));
      }
      expect([...new Set(found)]).toEqual([]);
    },
  );

  test(`a conversation's rows keep the scale at ${width}px`, async ({
    page,
    session,
    command,
  }) => {
    await page.setViewportSize({ width, height: 900 });
    await selectProbeModel(command, session);
    await page.goto(`/#session=${session.id}`);
    await online(page);
    await page
      .getByLabel("Message or guidance")
      .fill(
        "A message long enough to wrap over more than one line of the bubble at a phone's width",
      );
    await page.getByRole("button", { name: "Send", exact: true }).click();
    const footer = page.locator(".turn-summary");
    await expect(footer).toBeVisible();
    // The footer is pointed at, as the report that started this was.
    if (width > 600) await footer.hover();
    expect(await page.evaluate(measure)).toEqual([]);
  });
}

// A list's rows are parted by hairlines in every pane of Settings, whatever
// the row primitive draws for itself.
test("every row after a group's first is parted by a hairline in Settings", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const pane = page.locator(".settings-pane");
  for (const [scope, loaded] of [
    ["All conversations", pane.locator("[data-setting]").first()],
    ["Host", pane.getByText("Paired devices")],
  ]) {
    await page
      .locator(".settings-nav")
      .getByRole("button", { name: scope, exact: true })
      .click();
    await expect(loaded).toBeVisible();
    const borders = await pane.evaluate((el) =>
      [...el.querySelectorAll(".group-rows > :not(:first-child)")].map(
        (row) => getComputedStyle(row).borderTopWidth,
      ),
    );
    expect(borders.length).toBeGreaterThan(0);
    expect(new Set(borders)).toEqual(new Set(["1px"]));
  }
});
