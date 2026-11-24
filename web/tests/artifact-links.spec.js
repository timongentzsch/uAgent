import {
  test,
  expect,
  openFiles,
  slide,
  viewerOf,
  writeFiles,
} from "./fixtures.js";

test.use({ serviceWorkers: "block" });

test("a copied slide link opens the same slide on a paired device and after reload", async ({
  page,
  context,
  host,
  session,
}) => {
  const first = "decks/notes/01-intro.html";
  const second = "decks/notes/02-R&D #2 + 50%.html";
  await writeFiles(host.project, {
    [first]: slide("Opening"),
    [second]: slide("Results"),
  });
  await page.addInitScript(() => {
    Object.defineProperty(navigator, "clipboard", {
      value: {
        writeText: async (text) => {
          window.copiedLink = text;
        },
      },
    });
  });
  const pane = await openFiles(page, session, second);
  const headingLink = pane.getByRole("heading").getByRole("link");
  const href = await headingLink.getAttribute("href");
  const url = new URL(href);
  expect(url.origin).toBe(host.origin);
  expect(url.pathname).toBe("/");
  expect(url.search).toBe("");
  expect(Object.fromEntries(new URLSearchParams(url.hash.slice(1)))).toEqual({
    session: session.id,
    file: second,
  });
  const { count } = viewerOf(pane);
  await expect(count).toHaveText("2 / 2");
  const more = pane.getByRole("button", { name: "More tools" });
  if (await more.isVisible()) {
    await more.click();
    await pane.getByRole("menuitem", { name: "Export" }).click();
  } else await pane.getByRole("button", { name: "Export" }).click();
  await pane.getByRole("menuitem", { name: "Copy link" }).click();
  expect(await page.evaluate(() => window.copiedLink)).toBe(href);

  const paired = await context.browser().newContext({
    storageState: { cookies: await context.cookies(), origins: [] },
  });
  try {
    const secondDevice = await paired.newPage();
    await secondDevice.goto(href);
    const files = secondDevice.getByRole("region", { name: "Files" });
    await expect(viewerOf(files).count).toHaveText("2 / 2");
    await secondDevice.reload();
    await expect(viewerOf(files).count).toHaveText("2 / 2");
    await expect(files.getByRole("heading")).toHaveText(second);
    await files
      .getByRole("button", { name: "Previous slide", exact: true })
      .click();
    await expect(viewerOf(files).count).toHaveText("1 / 2");
    await expect
      .poll(() =>
        new URLSearchParams(new URL(secondDevice.url()).hash.slice(1)).get(
          "file",
        ),
      )
      .toBe(first);
    await secondDevice.reload();
    await expect(files.getByRole("heading")).toHaveText(first);
  } finally {
    await paired.close();
  }
});

test("file links follow browser history and missing targets stay missing", async ({
  page,
  session,
  host,
}) => {
  await writeFiles(host.project, {
    "src/a.txt": "first",
    "src/b.txt": "second",
    "archive/missing.txt": "another file",
  });
  const url = (file) =>
    `/#${new URLSearchParams({ session: session.id, ...(file && { file }) })}`;
  const title = page.locator(".files-title");
  await page.goto(url());
  await page.goto(url("src/a.txt"));
  await expect(title).toHaveText("src/a.txt");
  await page.goto(url("src/b.txt"));
  await expect(title).toHaveText("src/b.txt");
  await page.goBack();
  await expect(title).toHaveText("src/a.txt");
  await page.goBack();
  await expect(title).toHaveCount(0);
  await page.goForward();
  await expect(title).toHaveText("src/a.txt");
  await page.goto(url("missing.txt"));
  await expect(
    page.getByText("missing.txt is not in this folder."),
  ).toBeVisible();
  await page.goto(url("../outside.txt"));
  await expect(title).toHaveCount(0);
});

test("closing a linked file on a phone returns to a reloadable conversation", async ({
  page,
  host,
  session,
}) => {
  await writeFiles(host.project, {
    "deck/01-start.html": slide("Start"),
    "deck/02-end.html": slide("Finish"),
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(`/#session=${session.id}&file=deck%2F01-start.html`);
  const files = page.getByRole("dialog", { name: "Files" });
  await expect(viewerOf(files).count).toHaveText("1 / 2");
  await files.getByRole("button", { name: "Next slide" }).click();
  await expect(viewerOf(files).count).toHaveText("2 / 2");
  await files.getByRole("button", { name: "Close files" }).click();
  await expect(files).toHaveCount(0);
  await expect(page).toHaveURL(new RegExp(`#session=${session.id}$`));
  await page.reload();
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
  await expect(files).toHaveCount(0);
});

test("a linked file follows the session's execution host when the address omits it", async ({
  page,
  host,
  session,
}) => {
  await writeFiles(host.project, { "host.txt": "the session workspace" });
  const owner = "c".repeat(32);
  await page.route("**/api/hosts", (route) =>
    route.fulfill({
      json: {
        hosts: [
          {
            id: owner,
            address: "test-workspace",
            phase: "connected",
            error: "",
          },
        ],
      },
    }),
  );
  await page.route(/\/api\/sessions(?:\?.*)?$/, async (route) => {
    const reply = await route.fetch();
    const catalogue = await reply.json();
    catalogue.sessions.find((row) => row.id === session.id).host = owner;
    await route.fulfill({ json: catalogue });
  });
  await page.goto(`/#session=${session.id}&file=host.txt`);
  const heading = page.locator(".files-title");
  await expect(heading).toHaveText("host.txt");
  const address = () =>
    Object.fromEntries(new URLSearchParams(new URL(page.url()).hash.slice(1)));
  const expected = { session: session.id, host: owner, file: "host.txt" };
  await expect.poll(address).toEqual(expected);
  const href = await heading.getByRole("link").getAttribute("href");
  expect(
    Object.fromEntries(new URLSearchParams(new URL(href).hash.slice(1))),
  ).toEqual(expected);
  await page.reload();
  await expect(page.locator(".code-view")).toContainText(
    "the session workspace",
  );
});
