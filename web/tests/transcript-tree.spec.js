import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  chooseDetail,
  fakeStream,
  openView,
} from "./fixtures.js";
import { agents, arrivals, conversation, pondering } from "./golden-turns.js";

// The transcript tree on the dev page with a mocked host, at each detail
// level. Set UAGENT_SHOTS to a folder to keep a screenshot of each.
test.use({ serviceWorkers: "block" });

const ID = "a".repeat(32);
const SESSION = {
  id: ID,
  generation: "g",
  title: "Golden turns",
  cwd: "/project",
  status: "idle",
  presence: "active",
  updated: 1_700_000_000_000,
};

async function open(page, level, nodes = conversation, children = []) {
  await page.addInitScript(fakeStream, { epoch: "e", cursor: 1 });
  await page.addInitScript((detail) => {
    localStorage.setItem("uagent-detail", detail);
    localStorage.setItem("uagent-thinking", "show");
  }, level);
  await page.route("**/api/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    const json = (body, status = 200) =>
      route.fulfill({ status, json: { v: 2, ...body } });
    if (path === "/api/sessions")
      return json({
        epoch: "e",
        cursor: 1,
        sessions: [SESSION, ...children],
        capabilities: {},
        devices: [],
        commands: [],
      });
    if (path === `/api/sessions/${ID}`)
      return json({
        epoch: "e",
        cursor: 1,
        metadata: SESSION,
        state: { phase: "idle", view: { nodes } },
        pending: null,
      });
    return json({ error: "Not in this mock" }, 404);
  });
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await expect(
    page.locator(`[data-message-id="${nodes[0].id}"]`),
  ).toBeVisible();
}

const rows = (page) => page.locator(".transcript .node-row");

// The vertical space between each pair of neighbours in every column of the
// transcript: the conversation, and what an open row holds.
const gaps = (page) =>
  page.evaluate(() => {
    const found = [];
    for (const column of document.querySelectorAll(
      ".transcript-content, .node, .node-open, .node-kids, .node-body",
    )) {
      // What is out of the flow (a spoken-only line) has no place in it.
      const shown = [...column.children]
        .filter((child) => getComputedStyle(child).position !== "absolute")
        .map((child) => child.getBoundingClientRect())
        .filter((box) => box.height > 0);
      for (let index = 1; index < shown.length; index++)
        found.push(
          Math.round((shown[index].top - shown[index - 1].bottom) * 10) / 10,
        );
    }
    return found;
  });

test("the composer and open receipt share the transcript's text edges", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await open(page, "default", [
    agents[0],
    {
      id: "receipt",
      parent: null,
      kind: "call",
      headline: { verb: "Read", target: "notes.txt" },
      body: { kind: "text", text: "A short note" },
      state: "ok",
    },
  ]);
  const row = page.locator('[data-message-id="receipt"]');
  await row.getByRole("button", { name: /Read notes/ }).click();
  await expect(row.getByRole("group", { name: "Response" })).toBeVisible();
  const mismatch = () =>
    page.evaluate(() => {
      const box = (selector) =>
        document.querySelector(selector).getBoundingClientRect();
      const column = box(".transcript-content");
      const draft = box(".message-box");
      const edge = document.documentElement.dir === "rtl" ? "right" : "left";
      return Math.max(
        Math.abs(column.left - draft.left),
        Math.abs(column.right - draft.right),
        Math.abs(box(".node-headline")[edge] - box(".node-section")[edge]),
      );
    });
  for (const dir of ["ltr", "rtl"]) {
    await page.evaluate((dir) => {
      document.documentElement.dir = dir;
    }, dir);
    for (const width of [1024, 1440, 390]) {
      await page.setViewportSize({ width, height: 900 });
      await expect.poll(mismatch).toBeLessThan(1);
    }
  }
});

for (const [device, viewport] of [
  // Tall enough to hold the whole conversation open at full.
  ["desktop", { width: 1280, height: 1400 }],
  ["phone", { width: 390, height: 1800 }],
])
  for (const level of ["minimal", "default", "full"])
    test(`rows are evenly spaced at ${level} on a ${device}`, async ({
      page,
    }) => {
      await page.setViewportSize(viewport);
      await open(page, level);
      const shown = rows(page);
      // Minimal shows each turn's work as one line; default opens it.
      await expect(shown).toHaveCount(
        { minimal: 4, default: 13, full: 13 }[level],
      );
      const measured = await gaps(page);
      expect(measured.length).toBeGreaterThan(0);
      expect(new Set(measured).size, JSON.stringify(measured)).toBe(1);
      const heights = await shown.evaluateAll((items) =>
        items.map((row) => Math.round(row.getBoundingClientRect().height)),
      );
      expect(new Set(heights).size, JSON.stringify(heights)).toBe(1);
      if (process.env.UAGENT_SHOTS) {
        // The mock has no service worker, which the page reports.
        await page.getByRole("button", { name: "Dismiss" }).click();
        await page.locator(".transcript").screenshot({
          path: `${process.env.UAGENT_SHOTS}/${device}-${level}.png`,
        });
      }
    });

test("a long result word gives way to the headline on a phone", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await open(page, "default");
  const rejected = page.locator('[data-message-id="t-j"]');
  const row = rejected.locator(":scope > .node-row");
  const [headline, word] = await Promise.all(
    [".node-headline", ".node-word"].map((selector) =>
      row.locator(selector).evaluate((element) => ({
        width: element.getBoundingClientRect().width,
        cut: element.scrollWidth > element.clientWidth,
      })),
    ),
  );
  expect(headline.cut).toBe(false);
  expect(headline.width).toBeGreaterThan(word.width);
  expect(word.cut).toBe(true);
  await expect(row.locator(".node-word")).toHaveCSS(
    "text-overflow",
    "ellipsis",
  );
  await row.click();
  await expect(rejected.locator(".node-body")).toContainText(
    "path is outside the folder: /etc is not inside /project",
  );
});

test("rows open and close from the keyboard, and stay as left", async ({
  page,
}) => {
  await open(page, "minimal");
  const work = page.locator('[data-message-id="w-m-22"] > .node-row');
  await work.focus();
  await page.keyboard.press("ArrowRight");
  await expect(work).toHaveAttribute("aria-expanded", "true");
  await page.keyboard.press("ArrowRight");
  const first = page.locator('[data-message-id="t-f"] > .node-row');
  await expect(first).toBeFocused();
  await page.keyboard.press("ArrowLeft");
  await expect(work).toBeFocused();
  await page.keyboard.press("Space");
  await expect(work).toHaveAttribute("aria-expanded", "false");
  await page.keyboard.press("Enter");
  await page.reload();
  await expect(work).toHaveAttribute("aria-expanded", "true");
  await expect(
    page.locator('[data-message-id="w-m-12"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
  await chooseDetail(page, "Full");
  await expect(rows(page)).toHaveCount(13);
  await chooseDetail(page, "Minimal");
  await expect(work).toHaveAttribute("aria-expanded", "false");
});

test("a thought is text among the steps, whole at every level and under a folded work", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  await open(page, "minimal", pondering);
  const thought = page.locator(".thought");
  const text = thought.locator(".node-text");
  const whole = () =>
    text.evaluate((node) => node.scrollHeight <= node.clientHeight + 1);
  // Not a row: nothing to open, no box round it, and set apart by its face.
  await expect(thought.locator(".node-row")).toHaveCount(0);
  await expect(thought.getByRole("button")).toHaveCount(0);
  await expect(text).toHaveCSS("font-style", "italic");
  await expect(text).toHaveCSS("border-top-width", "0px");
  // Folded, the work still says what it is thinking.
  await expect(
    page.locator('[data-message-id="w-m-40"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
  const said = await text.textContent();
  expect(await whole()).toBe(true);
  for (const level of ["Default", "Full"]) {
    await chooseDetail(page, level);
    await expect(text).toHaveText(said);
    await expect(thought.getByRole("button")).toHaveCount(0);
    expect(await whole()).toBe(true);
  }
});

test("the header sets the level and shows or hides thinking", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 1400 });
  await open(page, "full");
  await expect(rows(page)).toHaveCount(13);
  await openView(page);
  const thinking = page.getByRole("menuitemcheckbox", { name: "Thinking" });
  await expect(thinking).toBeChecked();
  // A thought is text among the rows, so hiding it leaves them as they were.
  await expect(page.locator('[data-kind="thinking"]')).toHaveCount(1);
  await thinking.click();
  await openView(page);
  await expect(thinking).not.toBeChecked();
  await expect(rows(page)).toHaveCount(13);
  await expect(page.locator('[data-kind="thinking"]')).toHaveCount(0);
  await page.getByRole("menuitemradio", { name: "Minimal" }).click();
  await expect(
    page.locator('[data-message-id="w-m-22"] > .node-row'),
  ).toHaveAttribute("aria-expanded", "false");
});

test("what the agent says while it works shows under a folded work", async ({
  page,
}) => {
  // open() waits for r-3-2-1, here the note.
  const running = [
    {
      id: "m-1",
      parent: null,
      kind: "turn",
      body: { kind: "text", text: "q" },
    },
    {
      id: "w-m-1",
      parent: "m-1",
      kind: "work",
      state: "running",
      headline: { verb: "", target: "1 read" },
    },
    {
      id: "t-1",
      parent: "w-m-1",
      kind: "call",
      state: "ok",
      headline: { verb: "Read", target: "a.txt" },
    },
    {
      id: "r-3-2-1",
      parent: "w-m-1",
      kind: "message",
      role: "assistant",
      body: { kind: "markdown", text: "Looking at the host first." },
    },
  ];
  await open(page, "minimal", running);
  const work = page.locator('[data-message-id="w-m-1"]');
  await expect(work.locator("> .node-row")).toHaveAttribute(
    "aria-expanded",
    "false",
  );
  await expect(work).toContainText("Looking at the host first.");
  await expect(page.locator('[data-message-id="t-1"]')).toHaveCount(0);
});

test("an older assignment opens as a regular tool call and links to the child", async ({
  page,
}) => {
  await open(page, "default", agents);
  const call = page.locator('[data-message-id="t-s"]');
  const row = call.locator("> .node-row");
  await expect(row).toHaveAttribute("aria-expanded", "false");
  await row.click();
  await expect(row).toHaveAttribute("aria-expanded", "true");
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await call.getByRole("button", { name: /^Open agent(?: ·|$)/ }).click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=child");
});

for (const width of [390, 1280]) {
  test(`active agents stay above the input and completed agents reopen from below at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 800 });
    const assignments = [
      "fstab: validate syntax and sources, compare configured and active mounts",
      "firewall: inspect active policies and flag unusual rules. Record any configuration issues and explain which checks require additional access.",
    ];
    const children = assignments.map((title, i) => ({
      ...SESSION,
      parent: ID,
      agent_edits: "none",
      id: "child" + i,
      title,
      turn_active: i === 0,
    }));
    const history = agents.map((node) =>
      node.id === "t-s"
        ? {
            ...node,
            parts: children.map((child) => ({
              kind: "link",
              to: "agent",
              id: child.id,
              label: child.title,
            })),
          }
        : node,
    );
    await open(
      page,
      "default",
      [
        ...history,
        ...Array.from({ length: 20 }, (_, i) => ({
          id: "m-" + (100 + i),
          sequence: 100 + i,
          kind: "message",
          role: "user",
          body: { kind: "markdown", text: "Earlier message " + i },
        })),
      ],
      children,
    );
    const active = page.getByRole("navigation", { name: "Active agents" });
    const past = page.getByRole("button", { name: "Past agents (1)" });
    const input = page.locator(".message-box");
    await expect(
      active.getByRole("button", { name: "fstab · Working" }),
    ).toHaveText("fstabWorking");
    await expect(past).toBeVisible();
    const strip = await active.boundingBox();
    const box = await input.boundingBox();
    expect(strip.y + strip.height).toBeLessThanOrEqual(box.y + 1);
    expect((await past.boundingBox()).y).toBeGreaterThanOrEqual(
      box.y + box.height - 1,
    );
    const scrolled = await page.locator(".transcript").evaluate((el) => {
      const from = el.scrollTop;
      el.scrollTop = from ? 0 : el.scrollHeight;
      return from !== el.scrollTop;
    });
    expect(scrolled).toBe(true);
    expect((await active.boundingBox()).y).toBe(strip.y);
    const call = page.locator('[data-message-id="t-s"]');
    await call.locator("> .node-row").click();
    await expect(call.locator(".sender-name")).toHaveText([
      "fstab",
      "firewall",
    ]);
    await page.evaluate(
      (child) =>
        globalThis.testStream.dispatchEvent(
          new MessageEvent("update", {
            data: JSON.stringify({
              v: 2,
              epoch: "e",
              sequence: 2,
              generation: child.generation,
              session_id: child.id,
              kind: "metadata",
              metadata: { ...child, turn_active: false },
            }),
          }),
        ),
      children[0],
    );
    await expect(active).toHaveCount(0);
    const pastAgents = page.getByRole("button", { name: "Past agents (2)" });
    const triggerHeight = (await pastAgents.boundingBox()).height;
    await pastAgents.click();
    const roster = page.getByRole("dialog", { name: "Past agents" });
    const row = roster.getByRole("listitem").filter({ hasText: "firewall" });
    // Only the trigger below the input uses the compact button size.
    expect(
      (await row.getByRole("button").boundingBox()).height,
    ).toBeGreaterThan(triggerHeight);
    const task = row.locator("summary");
    await expect(task).toHaveAccessibleName(
      /^Assignment for firewall\. inspect active/,
    );
    await expect(task).toHaveText(assignments[1].slice("firewall: ".length));
    const folded = await task.boundingBox();
    const rowBefore = await row.boundingBox();
    await task.focus();
    await page.keyboard.press("Enter");
    await expect(row.locator("details")).toHaveJSProperty("open", true);
    await expect(row.getByText("Read-only assignment")).toBeVisible();
    const expanded = await task.boundingBox();
    const rowAfter = await row.boundingBox();
    expect(expanded.x - rowAfter.x).toBe(folded.x - rowBefore.x);
    expect(expanded.y - rowAfter.y).toBe(folded.y - rowBefore.y);
    expect(expanded.height).toBeGreaterThan(folded.height);
    await page.keyboard.press("Space");
    await expect(row.locator("details")).toHaveJSProperty("open", false);
    await row.getByRole("button", { name: "firewall Finished" }).click();
    await expect.poll(() => new URL(page.url()).hash).toBe("#session=child1");
  });
}

test("active agents share one moving band until only waiting agents remain", async ({
  page,
}) => {
  await page.emulateMedia({ reducedMotion: "no-preference" });
  const children = ["Build", "Review"].map((title, i) => ({
    ...SESSION,
    parent: ID,
    id: "active" + i,
    title,
    turn_active: true,
  }));
  await open(page, "default", agents, children);
  const strip = page.getByRole("navigation", { name: "Active agents" });
  const band = strip.locator(".work-band");
  await expect(band).toHaveCount(1);
  const pixels = () => band.evaluate((canvas) => canvas.toDataURL());
  const initial = await pixels();
  await expect.poll(pixels).not.toBe(initial);
  await page.evaluate((sessions) => {
    sessions.forEach((child, i) =>
      globalThis.testStream.dispatchEvent(
        new MessageEvent("update", {
          data: JSON.stringify({
            v: 2,
            epoch: "e",
            sequence: 2 + i,
            generation: child.generation,
            session_id: child.id,
            kind: "metadata",
            metadata: { ...child, status: "waiting", needs_you: i === 0 },
          }),
        }),
      ),
    );
  }, children);
  await expect(
    strip.getByRole("button", { name: "Build · Needs you" }),
  ).toBeVisible();
  await expect(
    strip.getByRole("button", { name: "Review · Waiting" }),
  ).toBeVisible();
  await expect(band).toHaveCount(0);
});

test("mailed reports are ordinary conversation messages in sequence, with selectable paragraphs and a sender", async ({
  page,
}) => {
  const mail = {
    ...arrivals[0],
    agent: { ...arrivals[0].agent, title: "Research your assigned segment" },
    body: {
      kind: "markdown",
      text: arrivals[0].body.text + "\n\nThe later flight is **cheaper**.",
    },
  };
  const namedAssignments = agents.map((node) =>
    node.id === "t-s"
      ? { ...node, parts: [{ ...node.parts[0], label: "Find flights" }] }
      : node,
  );
  await open(page, "minimal", [
    ...namedAssignments,
    mail,
    ...arrivals.slice(1),
  ]);
  const report = page.getByRole("article", { name: /^Find flights(?:,|$)/ });
  await expect(report).toHaveClass(/\bresponse\b/);
  await expect(report).toHaveAttribute("data-sequence", "82");
  await expect(
    report.locator("p").filter({ hasText: "Two flights found." }),
  ).toBeVisible();
  await expect(
    report.locator("strong").filter({ hasText: "cheaper" }),
  ).toBeVisible();
  await expect(report).not.toContainText("[agent");
  const reply = page.locator('[data-message-id="r-9-1-1"]');
  await expect(reply.getByRole("heading", { name: "µAgent" })).toBeVisible();
  await expect(reply).toContainText("Noted.");
  const order = await page
    .locator('[data-message-id="m-82"], [data-message-id="r-9-1-1"]')
    .evaluateAll((nodes) => nodes.map((node) => node.dataset.messageId));
  expect(order).toEqual(["m-82", "r-9-1-1"]);
  await expect(page.locator('[data-message-id="m-92"]')).toContainText(
    "A background command finished",
  );
  await expect(page.getByRole("dialog")).toHaveCount(0);

  await report
    .getByText("Two flights found.", { exact: true })
    .evaluate((paragraph) => {
      const range = document.createRange();
      range.selectNodeContents(paragraph);
      const selection = getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
    });
  await page
    .getByRole("group", { name: "Selection" })
    .getByRole("button", { name: "Quote", exact: true })
    .click();
  await expect(page.locator(".composer .quote-bar")).toContainText(
    "Two flights found.",
  );
  await report
    .getByRole("button", { name: /^Open agent · Find flights$/ })
    .click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=child");
});

test("find opens a long inline message at a match outside its excerpt", async ({
  page,
}) => {
  const tail = "The overnight connection arrives in Zurich.";
  const mail = {
    ...arrivals[0],
    body: {
      kind: "markdown",
      text:
        arrivals[0].body.text +
        "\n\n" +
        "Other trains are available. ".repeat(29) +
        "\n\n" +
        tail,
    },
  };
  await open(page, "minimal", [mail, ...arrivals.slice(1)]);
  const report = page.getByRole("article", { name: /^Find flights(?:,|$)/ });
  await expect(report.getByText(tail, { exact: true })).toHaveCount(0);
  await expect(
    report.getByRole("button", { name: "Show full message" }),
  ).toBeVisible();
  await page.route("**/api/search?**", (route) =>
    route.fulfill({ json: { v: 2, hits: [{ sequence: 82, count: 1 }] } }),
  );
  await page.keyboard.press("ControlOrMeta+f");
  const bar = page.getByRole("search", { name: "Find in this conversation" });
  await bar.getByRole("searchbox").fill("Zurich");
  await expect(bar).toContainText("1 of 1");
  await expect(report.getByText(tail, { exact: true })).toBeVisible();
  await expect
    .poll(() =>
      page.evaluate(() =>
        Array.from(CSS.highlights.get("find-current") || []).map((range) =>
          range.toString(),
        ),
      ),
    )
    .toEqual(["Zurich"]);
  await page.keyboard.press("Escape");
  await expect(bar).toHaveCount(0);
  await report.getByRole("button", { name: "Show less" }).click();
  await expect(report.getByText(tail, { exact: true })).toHaveCount(0);
  await report.getByRole("button", { name: "Show full message" }).click();
  await expect(report.getByText(tail, { exact: true })).toBeVisible();
});

test("a folded batch exposes a live decision and the expanded call keeps the waited result", async ({
  page,
}) => {
  const batch = agents.map((node) =>
    node.id !== "t-s"
      ? node
      : {
          ...node,
          parts: [
            {
              kind: "link",
              to: "agent",
              id: "first",
              label: "Check prices",
              state: "completed",
              report:
                '[agent first "Check prices" finished (completed)]\nThree options.\n\nThe earliest train leaves at noon.',
            },
            {
              kind: "link",
              to: "agent",
              id: "second",
              label: "Check dates",
              state: "working",
            },
          ],
        },
  );
  const waiting = {
    ...SESSION,
    id: "second",
    parent: ID,
    title: "Check dates",
    needs_you: true,
    turn_active: true,
  };
  await open(page, "minimal", batch, [waiting]);
  const work = page.locator('[data-message-id="w-m-42"]');
  const toggle = work.locator("> .node-row");
  await expect(toggle).toHaveAttribute("aria-expanded", "false");
  const review = work.getByRole("button", { name: /Review decision/i });
  await expect(review).toBeVisible();
  await expect(work).toContainText("Check dates");
  await toggle.click();
  const call = work.locator('[data-message-id="t-s"]');
  await call.locator("> .node-row").click();
  await expect(call).toContainText("Check prices");
  await expect(
    call.getByText("The earliest train leaves at noon.", { exact: true }),
  ).toBeVisible();
  await expect(call).not.toContainText("[agent");
  await expect(call.locator("article[data-sequence]")).toHaveCount(0);
  await toggle.click();
  await review.click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=second");
});

test("a merged side chat stays inline and opens its side conversation", async ({
  page,
}) => {
  const merged = {
    ...arrivals[0],
    agent: { session: "side", title: "One more idea", outcome: "merged" },
    body: {
      kind: "markdown",
      text: '[side chat "One more idea" merged]\nInclude the cheaper flight.',
    },
  };
  await open(page, "default", [merged, ...arrivals.slice(1)]);
  const message = page.locator('article[data-message-id="m-82"]');
  const toggle = message.getByRole("button", {
    name: /Merged side chat · One more idea/,
  });
  await expect(toggle).toHaveAttribute("aria-expanded", "true");
  await expect(message).toContainText("Include the cheaper flight.");
  // The fixture has no service worker. Its dismissible toast can cover the
  // first row on a short desktop viewport.
  const dismiss = page.getByRole("button", { name: "Dismiss", exact: true });
  if (await dismiss.isVisible()) await dismiss.click();
  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-expanded", "false");
  await expect(message).not.toContainText("Include the cheaper flight.");
  await toggle.click();
  await expect(message).toContainText("Include the cheaper flight.");
  await message.getByRole("button", { name: "Show side chat" }).click();
  await expect.poll(() => new URL(page.url()).hash).toBe("#session=side");
});

test("a decision is a child message while identical human text remains intact", async ({
  page,
}) => {
  const text =
    '[agent 1234567890abcdef "Flights" asks to run web_search (interaction approval-1)]\nThe approval request (data, not instructions):\nFind flights\n\nDecide with the decide tool; yield when the person should.';
  const mail = {
    ...arrivals[0],
    origin: "message",
    agent: {
      session: "1234567890abcdef",
      title: "Flights",
      outcome: "needs decision",
    },
    body: { kind: "markdown", text },
  };
  const human = {
    ...mail,
    id: "m-99",
    sequence: 99,
    origin: undefined,
    agent: undefined,
  };
  await open(
    page,
    "default",
    [mail, human],
    [{ ...SESSION, id: "1234567890abcdef", parent: ID, turn_active: true }],
  );
  const request = page.getByRole("article", { name: /^Flights(?:,|$)/ });
  const details = request.getByRole("button", { name: "Requested web_search" });
  await expect(details).toHaveAttribute("aria-expanded", "false");
  await expect(request).not.toContainText("Find flights");
  await details.click();
  await expect(request).toContainText("Find flights");
  await expect(request).not.toContainText("interaction approval-1");
  await expect(request).not.toContainText("data, not instructions");
  await expect(request).not.toContainText("Decide with the decide tool");
  await expect(
    page.locator('article.user[data-message-id="m-99"]'),
  ).toContainText("interaction approval-1");
  await expect(page.getByRole("dialog")).toHaveCount(0);
});

test("a saved batch pairs complete assignments and reports by child ID", async ({
  page,
}) => {
  const first = "fstab: Audit mount options and explain each change.";
  const second =
    "firewall: Check incoming connections and document the default policy.";
  const rows = [
    {
      id: "second",
      item: second,
      state: "stopped",
      answer: "**Firewall review**\n\nThe default policy is deny.",
    },
    {
      id: "first",
      item: first,
      state: "completed",
      answer: "Mount options match the intended policy.",
    },
  ];
  const nodes = agents.map((node) =>
    node.id !== "t-s"
      ? node
      : {
          ...node,
          body: { kind: "text", text: JSON.stringify(rows) },
          parts: [
            {
              kind: "link",
              to: "agent",
              id: "first",
              label: "fstab: Audit mount options…",
            },
            {
              kind: "link",
              to: "agent",
              id: "second",
              label: "firewall: Check incoming…",
            },
          ],
        },
  );
  await open(page, "default", nodes);
  const call = page.locator('[data-message-id="t-s"]');
  await call
    .getByRole("button", { name: /Started 2 agents · firewall, fstab/ })
    .click();
  for (const row of rows) {
    const sender = call.getByRole("button", {
      name: `Open agent · ${row.item}`,
    });
    const reply = sender.locator(
      "xpath=ancestor::div[contains(@class,'agent-sender')]/..",
    );
    await expect(reply).toContainText(row.item.split(": ")[1]);
    await expect(reply).toContainText(row.answer.split("\n\n").at(-1));
  }
  await expect(
    call.locator("strong").filter({ hasText: "Firewall review" }),
  ).toBeVisible();
  await expect(call).toContainText("Stopped");
  await expect(call).toContainText("Completed");
  await expect(call).not.toContainText('"answer":');
  await expect(
    call.getByRole("button", { name: "Input and output" }),
  ).toBeVisible();
});

test("a truncated delegation requests its recorded exchange on demand and keeps failed assignments", async ({
  page,
}) => {
  const links = [
    {
      kind: "link",
      to: "agent",
      id: "first",
      label: "fstab: Inspect boot mounts.",
      state: "completed",
      report: "Short preview.",
    },
  ];
  const batch = agents.map((node) =>
    node.id !== "t-s"
      ? node
      : {
          ...node,
          parts: links,
          body: { kind: "text", text: '[{"id":"first",…', truncated: true },
        },
  );
  await open(page, "default", batch);
  let reads = 0;
  await page.route("**/api/sessions/*?detail=t-s&raw=1**", (route) => {
    reads++;
    return route.fulfill({
      json: {
        more: false,
        text: JSON.stringify({
          complete: true,
          response: JSON.stringify([
            {
              id: "first",
              state: "completed",
              item: "fstab: Inspect boot mounts.",
              answer: "Complete retained report.",
            },
            {
              id: "",
              state: "not started",
              item: "firewall",
              answer: "No slot was available.",
            },
          ]),
        }),
      },
    });
  });
  const call = page.locator('[data-message-id="t-s"]');
  await call.locator("> .node-row").click();
  await expect(call).toContainText("Short preview.");
  expect(reads).toBe(0);
  await call.getByRole("button", { name: "Full output" }).click();
  await expect(call).toContainText("Complete retained report.");
  await expect(call).toContainText("No slot was available.");
  await expect(call).toContainText("Not started");
  await expect(
    call.getByRole("button", { name: /Open agent · firewall/ }),
  ).toHaveCount(0);
  expect(reads).toBe(1);
});

test("compact tool receipts distinguish structured results, legacy bodies and failures", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const receipt = {
    id: "t-search",
    kind: "call",
    tool: "web_search",
    parent: null,
    sequence: 1,
    state: "ok",
    headline: { verb: "Searched", target: "station access" },
    result: { word: "1 source" },
    input: [{ kind: "fields", rows: [["Query", "station access"]] }],
    body: { kind: "link", text: "original provider output" },
    parts: [
      { kind: "markdown", text: "The west gate is **step-free**." },
      {
        kind: "source",
        title: "Station access",
        url: "https://transit.example/access",
        excerpt: "Use the west gate.",
      },
    ],
  };
  const rows = [
    receipt,
    {
      ...receipt,
      id: "t-raw",
      sequence: 2,
      tool: "docs_search",
      body: { kind: "fields", text: "Extension result in its original form" },
      parts: [{ kind: "unfamiliar" }],
    },
    {
      ...receipt,
      id: "t-fail",
      sequence: 3,
      state: "failed",
      body: { kind: "fields", text: "The source was unavailable." },
      parts: [{ kind: "fields", rows: [["Provider", "Transit"]] }],
    },
    {
      ...receipt,
      id: "t-code",
      sequence: 4,
      tool: "docs_example",
      result: {},
      body: { kind: "fields", text: "raw JSON result" },
      parts: [
        {
          kind: "code",
          label: "Returned excerpt",
          language: "ts",
          text: "const gate = 'west';",
        },
      ],
    },
  ];
  rows.push({
    ...receipt,
    id: "t-lines",
    sequence: 5,
    tool: "read_path",
    body: { kind: "fields", text: "  first\n\nthird\n" },
    parts: [{ kind: "code", first_line: 24, label: "Lines 24–26" }],
  });
  await open(page, "default", rows);
  const search = page.locator('[data-message-id="t-search"]');
  const row = search.getByRole("button", { name: /Searched station access/ });
  await expect(row).toHaveAttribute("aria-expanded", "false");
  let rawReads = 0;
  await page.route("**/api/sessions/*?detail=t-search&raw=1**", (route) => {
    rawReads++;
    return route.fulfill({
      json: {
        more: false,
        text: JSON.stringify({
          request: { queries: ["station access"] },
          response: receipt.body.text,
        }),
      },
    });
  });
  await row.focus();
  await page.keyboard.press("Enter");
  await expect(search.getByRole("group", { name: "Request" })).toContainText(
    "station access",
  );
  const response = search.getByRole("group", { name: "Response" });
  await expect(response.locator("strong")).toHaveText("step-free");
  await expect(
    response.getByRole("link", { name: "Station access" }),
  ).toHaveAttribute("href", /transit\.example\/access$/);
  await expect(response).toContainText("Use the west gate.");
  await expect(search).not.toContainText(receipt.body.text);
  expect(rawReads).toBe(0);
  await search.getByRole("button", { name: "Input and output" }).click();
  const raw = page.getByRole("dialog");
  await expect(raw.getByRole("tabpanel")).toContainText("queries");
  await raw.getByRole("tab", { name: "Response" }).click();
  await expect(raw.getByRole("tabpanel")).toHaveText(receipt.body.text);
  expect(rawReads).toBe(1);
  await page.keyboard.press("Escape");
  for (const [id, expected] of [
    ["t-raw", "Extension result in its original form"],
    ["t-fail", "The source was unavailable."],
    ["t-code", "const gate = 'west';"],
  ]) {
    const call = page.locator('[data-message-id="' + id + '"]');
    await call.locator("> .node-row").click();
    await expect(call.getByRole("group", { name: "Response" })).toContainText(
      expected,
    );
  }
  const code = page.locator('[data-message-id="t-code"]');
  await expect(code).not.toContainText("raw JSON result");
  const lines = page.locator('[data-tool="read_path"]');
  await lines.locator("> .node-row").press("Enter");
  expect(await lines.locator(".node-code pre").textContent()).toBe(
    "24\t  first\n25\t\n26\tthird",
  );
});

test("a partial batch keeps each report independent and an empty list stays empty", async ({
  page,
}) => {
  const a = "fstab: Validate the boot mounts.";
  const b = "firewall: Inspect the default policy.";
  const report =
    "The recorded mount matches the active entry. ".repeat(30) +
    "All six entries verified.";
  const assignments = [
    { id: "first", item: a, state: "completed", answer: report },
    { id: "second", item: b, state: "working" },
  ];
  await open(page, "default", [
    {
      id: "t-agents",
      kind: "call",
      tool: "agent_start",
      parent: null,
      state: "ok",
      headline: { verb: "Started", target: "review agents" },
      body: { kind: "text", text: JSON.stringify(assignments) },
      parts: assignments.map((v) => ({
        kind: "link",
        to: "agent",
        id: v.id,
        label: v.item,
        state: v.state,
      })),
    },
    {
      id: "t-empty",
      kind: "call",
      tool: "grep",
      parent: null,
      state: "ok",
      headline: { verb: "Searched", target: "station_id" },
      result: { items: 0, unit: "match", word: "0 matches" },
      body: { kind: "text", text: "No matches." },
    },
  ]);
  const batch = page.locator('[data-tool="agent_start"]');
  await batch.locator("> .node-row").press("Enter");
  const fstab = batch.locator(".node-agent").filter({ hasText: "fstab" });
  const firewall = batch.locator(".node-agent").filter({ hasText: "firewall" });
  await expect(fstab).not.toContainText("All six entries verified.");
  await fstab.getByRole("button", { name: "Show full message" }).click();
  await expect(fstab).toContainText("All six entries verified.");
  await expect(firewall).not.toContainText("All six entries verified.");
  await expect(
    firewall.getByRole("button", { name: "Show full message" }),
  ).toHaveCount(0);
  await fstab.getByRole("button", { name: "Show less" }).click();
  await expect(fstab).not.toContainText("All six entries verified.");
  const empty = page.locator('[data-tool="grep"]');
  await expect(empty.locator("> .node-row")).toContainText("0 matches");
  await empty.locator("> .node-row").click();
  await expect(empty.getByRole("group", { name: "Response" })).toHaveText(
    /No matches\./,
  );
});
