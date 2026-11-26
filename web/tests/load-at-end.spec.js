import {
  devServerTest as test,
  expect,
  DEV_SERVER_URL,
  fakeStream,
  leaveEnd,
  mockHost,
  online,
} from "./fixtures.js";

// A conversation opens at its end, and what arrives after its rows are drawn
// (a picture, a card's cover) changes no row's height.
test.use({ serviceWorkers: "block" });

const ID = "b".repeat(32);
const OTHER = "c".repeat(32);
const session = (id, title) => ({
  id,
  generation: "g",
  title,
  cwd: "/project",
  file_origin: "/project",
  status: "idle",
  presence: "active",
  updated: Date.now(),
});
const PICTURE = "0123456789abcdef0123456789abcdef";
// A 4×3 PNG, the shape of the 800×600 a part says.
const PNG = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAAAAACRn/EaAAAADklEQVR4nGNoAAIGOAEALQ8GAa0C414AAAAASUVORK5CYII=",
  "base64",
);

const text = (id, parent, sequence, role, words) => ({
  id,
  parent,
  kind: role === "user" ? "turn" : "message",
  role,
  sequence,
  body: { kind: "text", text: words },
});
const nodes = (prefix, size) => [
  ...Array.from({ length: 12 }, (_, index) => [
    text(`${prefix}-q${index}`, null, index * 2, "user", `Question ${index}`),
    text(
      `${prefix}-a${index}`,
      `${prefix}-q${index}`,
      index * 2 + 1,
      "assistant",
      `Answer ${index}. ` + "Some words to fill the row. ".repeat(12),
    ),
  ]).flat(),
  text(`${prefix}-q`, null, 30, "user", "Show me"),
  {
    id: `${prefix}-files`,
    parent: `${prefix}-q`,
    kind: "message",
    role: "assistant",
    sequence: 31,
    parts: [
      {
        kind: "file",
        id: PICTURE,
        name: "shot.png",
        mime: "image/png",
        bytes: PNG.length,
        ...size,
      },
      {
        kind: "artifact",
        type: "deck",
        path: "decks/a/",
        cover: "decks/a/01.html",
        count: 2,
      },
      {
        kind: "artifact",
        type: "deck",
        path: "decks/b/",
        cover: "decks/b/01.html",
        count: 2,
      },
    ],
  },
  text(`${prefix}-end`, `${prefix}-q`, 32, "assistant", "Here they are."),
];

// Serves the conversations; the picture, the cards' origins and the preview
// address wait until `release` is called.
async function serve(page, size, { origin = "/project" } = {}) {
  let release;
  const held = new Promise((resolve) => (release = resolve));
  const asked = [];
  await page.addInitScript(fakeStream, { epoch: "e", cursor: 1 });
  const sessions = [session(ID, "Presented"), session(OTHER, "Other")];
  await page.route(/\/preview\/[a-z]+\//, async (route) => {
    await held;
    await route.fulfill({
      contentType: "text/html",
      body: "<!doctype html><h1>Slide</h1>",
    });
  });
  await mockHost(page, {
    epoch: "e",
    cursor: 1,
    sessions,
    state: { phase: "idle", view: { nodes: nodes("p", size) } },
    more: async (route, path, json) => {
      asked.push(`${route.request().method()} ${path}`);
      if (path === `/api/sessions/${OTHER}`)
        return json({
          epoch: "e",
          cursor: 1,
          metadata: sessions[1],
          state: { phase: "idle", view: { nodes: nodes("o", size) } },
          pending: null,
        }).then(() => true);
      const late = async (answer) => {
        await held;
        await answer();
        return true;
      };
      if (path.endsWith(`/assets/${PICTURE}`))
        return late(() =>
          route.fulfill({ contentType: "image/png", body: PNG }),
        );
      if (path.endsWith("/file-origin"))
        return late(() =>
          json({ cwd: "/project", host: "", origin, current: true }),
        );
      if (path.endsWith("/preview"))
        return late(() => json({ base: "/preview/token/" }));
    },
  });
  return { release: () => release(), asked };
}

// Every painted frame's transcript height and distance from its end, from
// the first frame its rows show in (not the loading outline).
const record = () => {
  window.frames_ = [];
  const tick = () => {
    const box = document.querySelector(".transcript:not([aria-busy])");
    if (box?.querySelector("[data-message-id]"))
      window.frames_.push({
        height: box.scrollHeight,
        below: box.scrollHeight - box.clientHeight - box.scrollTop,
      });
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
};

const loaded = (page) =>
  Promise.all([
    expect
      .poll(() =>
        page
          .locator(".node-file img")
          .evaluate((image) => image.complete && image.naturalWidth > 0),
      )
      .toBe(true),
    expect(page.locator(".artifact-card iframe")).toHaveCount(2),
  ]);

for (const [label, size] of [
  ["with its size", { width: 800, height: 600 }],
  ["without a size", {}],
]) {
  test(`a picture and cards ${label} load without moving the end`, async ({
    page,
  }) => {
    await page.addInitScript(record);
    const { release } = await serve(page, size);
    await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
    await expect(page.locator(".artifact-card")).toHaveCount(2);
    release();
    await loaded(page);
    // Two more frames: whatever the loads changed has been painted.
    await page.evaluate(
      () =>
        new Promise((done) =>
          requestAnimationFrame(() => requestAnimationFrame(done)),
        ),
    );
    const frames = await page.evaluate(() => window.frames_);
    expect(new Set(frames.map((frame) => frame.height)).size).toBe(1);
    expect(frames.at(-1).below).toBeLessThanOrEqual(2);
  });
}

test("a card whose file is from elsewhere keeps its box", async ({ page }) => {
  const { release } = await serve(page, {}, { origin: "/elsewhere" });
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  const cover = page.locator(".artifact-card-cover").first();
  const before = await cover.boundingBox();
  release();
  await expect(cover).toContainText("Preview unavailable");
  expect((await cover.boundingBox()).height).toBe(before.height);
  await expect(page.locator(".artifact-card iframe")).toHaveCount(0);
});

test("the cards of a conversation ask for its preview address once", async ({
  page,
}) => {
  const { release, asked } = await serve(page, {});
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  release();
  await expect(page.locator(".artifact-card iframe")).toHaveCount(2);
  expect(asked.filter((line) => line.endsWith("/preview"))).toEqual([
    `POST /api/sessions/${ID}/preview`,
  ]);
});

const jump = (page) => page.getByRole("button", { name: "Jump to latest" });
const scrollUp = async (page) => {
  await leaveEnd(page.locator(".transcript"));
  await expect(jump(page)).toBeVisible();
};

test("a reload opens a conversation left mid-way at its end", async ({
  page,
}) => {
  const { release } = await serve(page, { width: 800, height: 600 });
  release();
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await scrollUp(page);
  await page.reload();
  await expect(page.locator('[data-message-id="p-end"]')).toBeVisible();
  await expect
    .poll(() =>
      page
        .locator(".transcript")
        .evaluate(
          (node) => node.scrollHeight - node.clientHeight - node.scrollTop,
        ),
    )
    .toBeLessThanOrEqual(2);
  await expect(jump(page)).toBeHidden();
});

// The reader's place: the first row showing and how far its top is from
// the transcript's.
const place = (page) =>
  page.locator(".transcript").evaluate((node) => {
    const top = node.getBoundingClientRect().top;
    const row = [...node.querySelectorAll("[data-message-id]")].find(
      (row) => row.getBoundingClientRect().bottom > top,
    );
    return {
      id: row.dataset.messageId,
      offset: Math.round(row.getBoundingClientRect().top - top),
    };
  });

test("switching back in the app returns to where the reader was", async ({
  page,
}) => {
  const { release } = await serve(page, { width: 800, height: 600 });
  release();
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await scrollUp(page);
  const left = await place(page);
  await page
    .getByRole("button", { name: /^Other/ })
    .first()
    .click();
  await expect(page.locator('[data-message-id="o-end"]')).toBeVisible();
  await page
    .getByRole("button", { name: /^Presented/ })
    .first()
    .click();
  await expect(page.locator('[data-message-id="p-end"]')).toBeAttached();
  await expect(jump(page)).toBeVisible();
  await expect.poll(() => place(page)).toEqual(left);
});

test("a reload reads the open conversation once", async ({ page }) => {
  const { release } = await serve(page, {});
  release();
  await page.goto(new URL(`/#session=${ID}`, DEV_SERVER_URL).href);
  await online(page);
  const reads = [];
  page.on("request", (request) => {
    if (new URL(request.url()).pathname === `/api/sessions/${ID}`)
      reads.push(request.url());
  });
  await page.reload();
  // Online comes after every read the connection makes.
  await online(page);
  expect(reads).toHaveLength(1);
});
