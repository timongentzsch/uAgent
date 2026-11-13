import { mkdir } from "node:fs/promises";
import { join } from "node:path";
import {
  test,
  expect,
  savedConversation,
  connected,
  online,
  selectProbeModel,
} from "./fixtures.js";
// Mocked host APIs must stay intercepted across reload and worker updates.
// Each host's places are a fold of their own.
test.use({ serviceWorkers: "block", places: true });
const remote = "b".repeat(32);
async function offlineHost(
  page,
  phase = "disconnected",
  notifications = false,
  error = "",
) {
  // These API mocks exercise a client without the separate PWA lifecycle.
  await page.addInitScript((notifications) => {
    if (notifications) {
      Object.defineProperty(navigator, "serviceWorker", {
        value: Object.assign(new EventTarget(), {
          register: () => new Promise(() => {}),
          getRegistration: async () => undefined,
          ready: new Promise(() => {}),
        }),
      });
    } else delete Navigator.prototype.serviceWorker;
  }, notifications);
  let reads = 0;
  const actions = [];
  await page.route("**/api/hosts", async (route) => {
    if (route.request().method() === "POST") {
      actions.push(route.request().postDataJSON());
      await route.fulfill({ json: { id: remote } });
    } else {
      reads++;
      await route.fulfill({
        json: {
          hosts: [
            {
              id: remote,
              address: "GPU",
              phase,
              error,
            },
          ],
        },
      });
    }
  });
  await page.route(/\/api\/sessions(?:\?.*)?$/, async (route) => {
    const response = await route.fetch();
    const catalogue = await response.json();
    catalogue.sessions.push({
      id: "a".repeat(32),
      host: remote,
      title: "GPU conversation",
      cwd: "/srv/project",
      updated: Date.now(),
      turns: 1,
      status: "saved",
      generation: "",
    });
    await route.fulfill({ json: catalogue });
  });
  return { actions, reads: () => reads };
}
test.describe("in Recent", () => {
  test.use({ places: false });
  test("restored hosts stay disconnected; Recent names and opens the owning host", async ({
    page,
    host,
    request,
  }) => {
    const local = await savedConversation(host, request, "Local conversation");
    const observed = await offlineHost(page);
    await page.goto(`/#host=${remote}`);
    await expect(
      page
        .locator(".host-list")
        .getByRole("button", { name: /GPU.*Disconnected/ }),
    ).toBeVisible();
    await expect.poll(observed.reads).toBeGreaterThanOrEqual(2);
    expect(observed.actions).toEqual([]);
    const today = page.getByRole("group", { name: "Today", exact: true });
    const row = (title) => today.locator(".session-row", { hasText: title });
    // The host shown names only the place; another host's row names it too.
    await expect(row("GPU conversation").locator(".session-tag")).toHaveText(
      "project",
    );
    await expect(row("Local conversation").locator(".session-tag")).toHaveText(
      `Local · ${host.project.split("/").at(-1)}`,
    );
    expect(observed.actions).toEqual([]);
    await row("Local conversation").locator(".list-row").click();
    await expect(page).toHaveURL(new RegExp(`#session=${local.id}$`));
    await expect(
      page.getByRole("heading", { name: "Local conversation", level: 1 }),
    ).toBeVisible();
    await row("GPU conversation").locator(".list-row").click();
    await expect.poll(() => observed.actions.length).toBe(1);
    expect(observed.actions[0]).toEqual({ action: "connect", id: remote });
    await expect(page).toHaveURL(
      new RegExp(`host=${remote}.*session=${"a".repeat(32)}`),
    );
    await page.reload();
    await expect(
      page
        .locator(".host-list")
        .getByRole("button", { name: /GPU.*Disconnected/ }),
    ).toBeVisible();
    expect(observed.actions).toHaveLength(1);
  });
});
test.describe("phone host controls", () => {
  test.use({
    isMobile: true,
    hasTouch: true,
    viewport: { width: 390, height: 844 },
  });
  test("host details expose connection controls", async ({ page }) => {
    const observed = await offlineHost(page);
    await page.goto("/");
    await page
      .getByRole("button", { name: "Open conversations", exact: true })
      .tap();
    const row = page
      .locator(".host-list")
      .getByRole("button", { name: /GPU.*Disconnected/ });
    await expect(row).toBeVisible();
    expect((await row.boundingBox()).height).toBeGreaterThanOrEqual(44);
    await row.tap();
    const detail = page.getByRole("dialog", { name: "GPU", exact: true });
    await expect(
      detail.getByRole("button", { name: "Reconnect", exact: true }),
    ).toBeEnabled();
    await expect(
      detail.getByRole("button", { name: "Disconnect", exact: true }),
    ).toBeDisabled();
    await detail.getByRole("button", { name: "Reconnect", exact: true }).tap();
    await expect.poll(() => observed.actions.length).toBe(1);
    expect(observed.actions[0]).toEqual({ action: "reconnect", id: remote });
  });
  // An iPhone home-screen app writes the address under the drawer, a step
  // back later: what was picked in the drawer still opens, on its own host.
  test("a home-screen app's drawer opens another host's conversation and folder", async ({
    page,
    host,
    request,
  }) => {
    const local = await savedConversation(host, request, "Standalone local");
    await offlineHost(page, "connected");
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "standalone", { value: true });
    });
    await page.goto(`/#session=${local.id}`);
    await expect(
      page.getByRole("heading", { name: "Standalone local", level: 1 }),
    ).toBeVisible();
    const drawer = page.getByRole("dialog", {
      name: "Conversations",
      exact: true,
    });
    const pick = async (group, name) => {
      await page
        .getByRole("button", { name: "Open conversations", exact: true })
        .tap();
      await drawer
        .getByRole("group", { name: group, exact: true })
        .getByRole("button", { name, exact: true })
        .tap();
      await expect(drawer).toHaveCount(0);
    };
    await pick("GPU", "GPU conversation");
    await expect(page.locator(".execution-host")).toHaveText("GPU");
    await expect(page).toHaveURL(
      new RegExp(`#host=${remote}&session=${"a".repeat(32)}$`),
    );
    // The drawer's own entry is gone once it has closed.
    await expect
      .poll(() => page.evaluate(() => history.state?.layer ?? 0))
      .toBe(0);
    await pick("Local", "Standalone local");
    await expect(page.locator(".execution-host")).toHaveText("Local");
    await expect(page).toHaveURL(new RegExp(`#session=${local.id}$`));
    await pick("GPU", "srv/project");
    await expect(
      page.getByRole("region", { name: "Overview of srv/project" }),
    ).toBeVisible();
    await expect(page).toHaveURL(new RegExp(`#host=${remote}$`));
  });
});

for (const width of [390, 1440]) {
  test.describe(`${width}px creation`, () => {
    test.use({
      viewport: { width, height: 844 },
      isMobile: width < 600,
      hasTouch: width < 600,
    });
    test(`home chooses a host and folder at ${width}px`, async ({ page }) => {
      await offlineHost(page, "connected");
      const creations = [];
      await page.route("**/api/folders?*", async (route) => {
        const query = new URL(route.request().url()).searchParams;
        const remote = query.has("host");
        const root = remote ? "/srv" : "/home/local";
        const path = query.get("path") === "~" ? root : query.get("path");
        await route.fulfill({
          json: {
            path,
            parent: "/",
            more: false,
            entries:
              path === root
                ? [{ name: "projects", dir: true }]
                : [{ name: "sample", dir: true }],
          },
        });
      });
      await page.route("**/api/command", async (route) => {
        const command = route.request().postDataJSON();
        if (command.kind !== "create") return route.continue();
        creations.push(command);
        await route.fulfill({
          json: {
            request_id: command.request_id,
            accepted: false,
            error: "Fixture stops after folder selection",
          },
        });
      });
      await page.goto("/");
      await page
        .getByRole("button", { name: "Other place…", exact: true })
        .click();
      const dialog = page.getByRole("dialog", {
        name: "New conversation",
        exact: true,
      });
      await dialog
        .getByRole("combobox", { name: "Host", exact: true })
        .selectOption(remote);
      // The folder that opens is the one chosen; a click chooses another,
      // and its arrow goes into it.
      const chosen = dialog.locator(".folder-picker-path");
      await expect(chosen).toHaveText("/srv");
      await dialog
        .getByRole("button", { name: "projects", exact: true })
        .click();
      await expect(chosen).toHaveText("/srv/projects");
      await dialog
        .getByRole("button", { name: "Open projects", exact: true })
        .click();
      await dialog.getByRole("button", { name: "sample", exact: true }).click();
      await expect(chosen).toHaveText("/srv/projects/sample");
      expect(
        await page.evaluate(() => document.documentElement.scrollWidth),
      ).toBeLessThanOrEqual(width);
      await page.screenshot({
        path: test.info().outputPath("host-folder-picker.png"),
      });
      await dialog
        .getByRole("button", { name: "Start conversation", exact: true })
        .click();
      await expect.poll(() => creations.length).toBe(1);
      expect(creations[0]).toMatchObject({
        host: remote,
        cwd: "/srv/projects/sample",
      });
    });
  });
}

test("host details show measured main-to-host latency", async ({ page }) => {
  await offlineHost(page, "connected");
  await page.route("**/api/hosts/*/ping", (route) =>
    route.fulfill({ json: { round_trip_ms: 27.4 } }),
  );
  await page.goto("/");
  await expect(
    page.locator(".host-list").getByText("Remote hosts", { exact: true }),
  ).toBeVisible();
  await expect(
    page.locator(".host-list").getByRole("button", { name: /Local/ }),
  ).toHaveCount(0);
  await page
    .locator(".host-list")
    .getByRole("button", { name: /GPU.*Connected/ })
    .click();
  const dialog = page.getByRole("dialog", { name: "GPU", exact: true });
  await expect(dialog.getByRole("status")).toHaveText(
    "Main → host · 27 ms round trip",
  );
});

test("late home loading preserves path entry and its caret", async ({
  page,
  host,
}) => {
  let release;
  const home = new Promise((resolve) => {
    release = resolve;
  });
  await page.route("**/api/folders?*", async (route) => {
    await home;
    await route.fulfill({
      json: { path: host.home, parent: "/", entries: [], more: false },
    });
  });
  await page.goto("/");
  await page.getByRole("button", { name: "Other place…", exact: true }).click();
  const dialog = page.getByRole("dialog", {
    name: "New conversation",
    exact: true,
  });
  await dialog.getByRole("button", { name: "Type a path" }).click();
  const path = dialog.getByLabel("Folder on the host");
  const loading = dialog.getByRole("status", { name: "Loading folders…" });
  await expect(loading).toHaveCount(1);
  release();
  await expect(loading).toHaveCount(0);
  await expect(path).toHaveValue("");
  await path.fill(host.project);
  await dialog
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.getByLabel("Message or guidance")).toBeVisible();
});

for (const phase of ["connected", "connecting"]) {
  test(`reconnect is disabled while ${phase}`, async ({ page }) => {
    await offlineHost(page, phase);
    await page.goto("/");
    await page
      .locator(".host-list")
      .getByRole("button", { name: /GPU/ })
      .click();
    await expect(
      page
        .getByRole("dialog", { name: "GPU", exact: true })
        .getByRole("button", { name: "Reconnect", exact: true }),
    ).toBeDisabled();
  });
}

// Wherever a host says why it is not connected, that is an error notice.
test("a host's connection error is drawn as an error", async ({ page }) => {
  const error =
    "A complete four-target remote bundle for this exact build is required.";
  await offlineHost(page, "disconnected", false, error);
  await page.goto("/");
  const red = await page.evaluate(() => {
    const probe = document.body.appendChild(document.createElement("i"));
    probe.style.color = "var(--removed)";
    return getComputedStyle(probe).color;
  });
  await page.locator(".host-list").getByRole("button", { name: /GPU/ }).click();
  const detail = page.getByRole("dialog", { name: "GPU", exact: true });
  await expect(detail.getByRole("alert")).toHaveText(error);
  await expect(detail.getByRole("alert")).toHaveCSS("color", red);
  await page.keyboard.press("Escape");
  await page.getByRole("button", { name: "Other place…", exact: true }).click();
  const creating = page.getByRole("dialog", { name: "New conversation" });
  await creating
    .getByRole("combobox", { name: "Host", exact: true })
    .selectOption(remote);
  await expect(creating.getByRole("alert")).toHaveText(error);
  await expect(creating.getByRole("alert")).toHaveCSS("color", red);
});

test("adding a host submits with a click", async ({ page }) => {
  const observed = await offlineHost(page);
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
  await page.getByRole("button", { name: "Add host", exact: true }).click();
  const dialog = page.getByRole("dialog", {
    name: "Add remote host",
    exact: true,
  });
  await dialog.getByRole("textbox").fill("devbox");
  await dialog.getByRole("button", { name: "Connect", exact: true }).click();
  await expect
    .poll(() => observed.actions)
    .toEqual([{ action: "add", address: "devbox" }]);
});

for (const notification of [false, true]) {
  test(`${notification ? "notification" : "cold link"} resolves the conversation's execution host`, async ({
    page,
  }) => {
    await offlineHost(page, "connected", notification);
    const session = "a".repeat(32);
    await page.goto(
      notification ? "/" : `/#session=${session}&decision=decision`,
    );
    await expect(
      page.locator(".host-list").getByRole("button", { name: /GPU/ }),
    ).toBeVisible();
    await expect(connected(page)).toBeVisible();
    if (notification)
      await page.evaluate((id) => {
        navigator.serviceWorker.dispatchEvent(
          new MessageEvent("message", {
            data: { type: "OPEN_SESSION", id, decision: "decision" },
          }),
        );
      }, session);
    await expect
      .poll(() =>
        new URLSearchParams(new URL(page.url()).hash.slice(1)).get("host"),
      )
      .toBe(remote);
    await expect(page.locator(".execution-host")).toHaveText("GPU");
    await expect(
      page
        .getByRole("group", { name: "GPU", exact: true })
        .locator('[aria-current="page"]'),
    ).toContainText("GPU conversation");
    // The decision link is consumed after requesting focus; ownership remains.
    expect(
      new URLSearchParams(new URL(page.url()).hash.slice(1)).get("session"),
    ).toBe(session);
  });
}

test("an unknown host falls back to the full Local conversation list", async ({
  page,
  host,
  request,
}) => {
  await offlineHost(page);
  await savedConversation(host, request, "Local fallback");
  await page.goto(`/#host=${"d".repeat(32)}`);
  const local = page.getByRole("group", { name: "Local", exact: true });
  const row = local
    .locator(".session-row")
    .filter({ hasText: "Local fallback" });
  await expect(row.locator('[aria-haspopup="menu"]')).toBeVisible();
  await row.getByRole("button", { name: /^Local fallback/ }).click();
  await expect(page.locator(".execution-host")).toHaveText("Local");
});

test("choosing a disconnected host's own conversation reconnects once", async ({
  page,
}) => {
  const observed = await offlineHost(page);
  await page.goto(`/#host=${remote}`);
  await expect(connected(page)).toBeVisible();
  await page
    .getByRole("group", { name: "GPU", exact: true })
    .getByRole("button", { name: /^GPU conversation/ })
    .click();
  await expect
    .poll(() => observed.actions)
    .toEqual([{ action: "connect", id: remote }]);
});

test("the folder browser hides dot folders until asked, goes into one by its arrow and back by a crumb", async ({
  page,
  host,
}) => {
  await mkdir(join(host.home, ".hidden", "inner"), { recursive: true });
  await page.goto("/");
  await page.getByRole("button", { name: "Other place…", exact: true }).click();
  const dialog = page.getByRole("dialog", {
    name: "New conversation",
    exact: true,
  });
  const chosen = dialog.locator(".folder-picker-path");
  const crumbs = dialog.getByRole("navigation", { name: "Folder on the host" });
  const hidden = dialog.getByRole("button", { name: ".hidden", exact: true });
  await expect(chosen).toHaveText(host.home);
  await expect(hidden).toHaveCount(0);
  await dialog.getByRole("switch", { name: "Show hidden folders" }).check();
  await dialog.getByRole("button", { name: "Open .hidden" }).click();
  await expect(chosen).toHaveText(join(host.home, ".hidden"));
  await expect(
    dialog.getByRole("button", { name: "inner", exact: true }),
  ).toBeVisible();
  await expect(crumbs.getByRole("button")).toHaveText(["~", ".hidden", ""]);
  await crumbs.getByRole("button", { name: "~", exact: true }).click();
  await expect(chosen).toHaveText(host.home);
  await expect(hidden).toBeVisible();
});

test("known folders follow the chosen host without duplicate suggestions", async ({
  page,
  host,
  request,
}) => {
  await savedConversation(host, request, "First local folder");
  await savedConversation(host, request, "Second local folder");
  await offlineHost(page, "connected");
  await page.goto("/");
  await page.getByRole("button", { name: "Other place…", exact: true }).click();
  const dialog = page.getByRole("dialog", {
    name: "New conversation",
    exact: true,
  });
  const recent = dialog
    .getByRole("list", { name: "Recent on this host" })
    .getByRole("listitem");
  await expect(recent).toHaveCount(1);
  await expect(recent).toHaveAttribute("data-path", host.project);
  await dialog
    .getByRole("combobox", { name: "Host", exact: true })
    .selectOption(remote);
  await expect(recent).toHaveCount(1);
  await expect(recent).toHaveAttribute("data-path", "/srv/project");
});

test("single-host headers and scheduled tasks omit redundant host controls", async ({
  page,
  session,
}) => {
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await expect(page.locator(".execution-host")).toHaveCount(0);
  await page.getByRole("button", { name: "Scheduled", exact: true }).click();
  await page.getByRole("button", { name: "New task", exact: true }).click();
  await expect(
    page.getByRole("combobox", { name: "Execution host", exact: true }),
  ).toHaveCount(0);
});

test("side conversations stay main-owned across execution host navigation", async ({
  page,
  session,
  command,
}) => {
  await selectProbeModel(command, session, { yolo: true });
  await offlineHost(page);
  await page.goto(`/#session=${session.id}`);
  await online(page);
  await page.getByLabel("Message or guidance").fill("/btw Side tool probe");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  const side = page.getByRole("complementary", { name: "Side conversation" });
  await expect(side).toContainText("SIDE-TOOL-COMPLETE");
  await page
    .getByRole("group", { name: "GPU", exact: true })
    .getByRole("button", { name: /^GPU conversation/ })
    .click();
  await expect(side).toHaveCount(0);
  await page.evaluate((id) => {
    location.hash = `session=${id}`;
  }, session.id);
  await expect(side).toContainText("SIDE-TOOL-COMPLETE");
  await side.getByRole("button", { name: "Dismiss side conversation" }).click();
  await page.reload();
  await online(page);
  await expect(side).toHaveCount(0);
});

test("offline gateway navigation does not request a remote connection", async ({
  page,
  context,
}) => {
  await offlineHost(page);
  await page.goto(`/#host=${remote}`);
  await expect(connected(page)).toBeVisible();
  // The hosts are read after the page connects: the network goes only once
  // the conversation is listed.
  const conversation = page
    .getByRole("group", { name: "GPU", exact: true })
    .getByRole("button", { name: /^GPU conversation/ });
  await expect(conversation).toBeVisible();
  await context.setOffline(true);
  await expect(connected(page)).toHaveCount(0);
  const posts = [];
  page.on("request", (request) => {
    if (request.url().endsWith("/api/hosts") && request.method() === "POST")
      posts.push(request.postDataJSON());
  });
  await conversation.click();
  await expect
    .poll(() =>
      new URLSearchParams(new URL(page.url()).hash.slice(1)).get("session"),
    )
    .toBe("a".repeat(32));
  expect(posts).toEqual([]);
});

test("host connection actions wait for the current request", async ({
  page,
}) => {
  await offlineHost(page, "reconnecting");
  const { promise: gate, resolve: release } = Promise.withResolvers();
  await page.route("**/api/hosts", async (route) => {
    if (route.request().method() !== "POST") return route.fallback();
    await gate;
    await route.fulfill({ json: { id: remote } });
  });
  await page.goto("/");
  await page.locator(".host-list").getByRole("button", { name: /GPU/ }).click();
  const dialog = page.getByRole("dialog", { name: "GPU", exact: true });
  try {
    await dialog
      .getByRole("button", { name: "Reconnect", exact: true })
      .click();
    await expect(
      dialog.getByRole("button", { name: "Disconnect", exact: true }),
    ).toBeDisabled();
  } finally {
    release();
  }
});

test("a scheduled draft retains its execution host while navigating", async ({
  page,
}) => {
  await offlineHost(page);
  await page.goto("/");
  await expect(connected(page)).toBeVisible();
  await page.getByRole("button", { name: "Scheduled", exact: true }).click();
  await page.getByRole("button", { name: "New task", exact: true }).click();
  await page.getByLabel("Name", { exact: true }).fill("Main-owned draft");
  await page
    .getByRole("group", { name: "GPU", exact: true })
    .getByRole("button", { name: /^GPU conversation/ })
    .click();
  await page.getByRole("button", { name: "Scheduled", exact: true }).click();
  await expect(page.getByLabel("Name", { exact: true })).toHaveValue(
    "Main-owned draft",
  );
  await expect(
    page.getByRole("combobox", { name: "Execution host", exact: true }),
  ).toHaveValue("");
});
