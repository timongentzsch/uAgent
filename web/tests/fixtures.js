import { test as base, expect } from "@playwright/test";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

export { expect };

// Cold lazy-module tests stay independent of precaching: a valid, empty
// service worker never serves a chunk from cache.
export const withoutServiceWorker = (page) =>
  page.route("**/sw.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }),
  );

// The composer sends and attaches nothing until the event stream has caught
// up. A click waits for that; a key press, setInputFiles or a dispatched
// drop does not, and what it carried is lost.
export const online = (page) =>
  expect(page.locator('.composer input[type="file"]')).toBeEnabled();

// New ▾ Folder…: asks for a path on the host, ready to start a conversation.
export async function newFolder(page, path) {
  await page.getByRole("button", { name: "New", exact: true }).click();
  await page.getByRole("menuitem", { name: "Folder…" }).click();
  await page.getByLabel("Folder on the host").fill(path);
}

// newFolder, then starts the conversation and waits for its runtime.
export async function startInFolder(page, path) {
  await newFolder(page, path);
  await page
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(page.locator(".composer .status-led.active")).toBeVisible();
}

// Opens a session's folder's panel from the place's name in the list, and
// returns the panel's id.
export async function openPanel(page, session) {
  await page.goto(`/#session=${session.id}`);
  // Narrow layouts keep the sessions in a drawer.
  const drawer = page.getByLabel("Open conversations");
  const button = page.getByTitle(/^Lead agent of /).first();
  await expect(drawer.or(button)).toBeVisible();
  if (await drawer.isVisible()) await drawer.click();
  await button.click();
  await expect(page.getByRole("heading", { name: /^Lead · / })).toBeVisible();
  return new URL(page.url()).hash.match(/session=([a-f0-9]+)/)[1];
}

// Puts a session on the model the host's scripted provider answers as
// (tests/web_host.py); with `yolo`, its tools run without asking.
export async function selectProbeModel(
  command,
  session,
  { yolo = false } = {},
) {
  const target = { session_id: session.id, generation: session.generation };
  await command("model", {
    ...target,
    operation: "select",
    model: "mock/model-b",
  });
  if (yolo) await command("permissions", { ...target, mode: "yolo" });
}

// openPanel, with the panel on the probe model once its runtime is up.
export async function openProbePanel(page, session, command, request) {
  const id = await openPanel(page, session);
  let panel;
  await expect
    .poll(async () => {
      panel = (await (await request.get(`/api/sessions/${id}`)).json())
        .metadata;
      return panel.generation;
    })
    .toBeTruthy();
  await selectProbeModel(command, panel);
  return id;
}

// Saves a conversation the way the host keeps one, for the list to show;
// returns its file.
export async function saveHistory(
  host,
  file,
  { id, title, turns, messages, kinds, cwd, display },
) {
  const directory = `${host.home}/.uagent/history`;
  await mkdir(directory, { recursive: true });
  const header = {
    format: 3,
    cwd: cwd || host.project,
    model: "test",
    session_id: id,
    title,
    turns,
  };
  const state = {
    messages,
    message_kinds: kinds || messages.map((message) => message.role),
    archive: [],
    archive_dropped_segments: 0,
    context_tokens: 0,
    context_window: 0,
    usage: {},
    last_sent_prompt: "",
    display: display || {},
    delivered_mail: [],
  };
  const path = `${directory}/${file}`;
  await writeFile(path, `${JSON.stringify(header)}\n${JSON.stringify(state)}`, {
    mode: 0o600,
  });
  return path;
}

// What a wider header shows as a button beside the title (Settings, Files,
// Open browser, Edit instructions) and a phone keeps in the conversation's
// menu. A button's name may say more after it ("Open browser, agent
// working"). Without a conversation a phone has no menu and keeps Settings.
export async function headerAction(page, name) {
  const head = page.locator(".conversation-head");
  const menu = head.getByRole("button", { name: "Conversation menu" });
  const phone = await page.evaluate(
    () => matchMedia("(max-width: 600px)").matches,
  );
  if (phone && (await menu.count())) {
    await menu.click();
    return page.getByRole("menuitem", { name, exact: true }).click();
  }
  await page
    .getByRole("button", { name: new RegExp(`^${name}\\b`) })
    .filter({ visible: true })
    .first()
    .click();
}

// A question set from the model: single and multiple choice, an image and
// previews on the options.
export const SAMPLE_ASK = {
  id: "ask-sample",
  kind: "ask",
  route: "panel",
  prompt: "Which storage should the cache use?",
  questions: [
    {
      header: "Storage",
      question: "Which storage should the cache use?",
      options: [
        { label: "Memory", description: "Fastest; lost on restart." },
        { label: "Disk", description: "Survives restarts; slower reads." },
      ],
    },
    {
      header: "Platforms",
      question: "Which platforms must it support?",
      multi_select: true,
      options: [
        { label: "macOS", description: "Apple silicon and Intel." },
        { label: "Linux", description: "glibc distributions." },
        { label: "Windows", description: "Windows 11 and later." },
      ],
    },
    {
      header: "Layout",
      question: "Which layout should the settings page use?",
      options: [
        {
          label: "Sidebar",
          description: "Sections listed on the left, one open at a time.",
          image: {
            id: "fedcba9876543210fedcba9876543210",
            name: "sidebar.svg",
            path: "mockups/sidebar.svg",
          },
          preview:
            "+------+----------+\n| Nav  | Section  |\n+------+----------+",
        },
        {
          label: "Tabs",
          description: "Sections as tabs above one scrolling page.",
          preview:
            "[ A ][ B ][ C ]\n+--------------+\n|   Section    |\n+--------------+",
        },
      ],
    },
  ],
};

// Serves the session's snapshot with `pending` in it, and records the
// commands the page sends instead of reaching the host; returns the list.
export async function servePending(page, request, session, pending) {
  const snapshot = await (
    await request.get(`/api/sessions/${session.id}`)
  ).json();
  snapshot.pending = pending;
  await page.route(`**/api/sessions/${session.id}`, (route) =>
    route.fulfill({ json: snapshot }),
  );
  const commands = [];
  await page.route("**/api/command", (route) => {
    commands.push(route.request().postDataJSON());
    return route.fulfill({ json: { accepted: true } });
  });
  return commands;
}

// The app served by the Vite dev server (see playwright.config.js) instead of
// the native host.
export { devServerUrl as DEV_SERVER_URL } from "./dev-server.js";

// No test may end with an uncaught page error.
const failOnPageErrors = async ({ page }, use) => {
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.stack || error.message));
  await use(page);
  expect(errors, "uncaught page errors").toEqual([]);
};

// Tests of the dev-server app: no native host.
export const devServerTest = base.extend({ page: failOnPageErrors });

export const test = base.extend({
  paired: [true, { option: true }],
  // A host whose conversations start with no model chosen.
  noModel: [false, { option: true }],
  // A host in Auto approval whose decision model is wary of sending data.
  autoReview: [false, { option: true }],
  page: failOnPageErrors,
  host: async ({ noModel, autoReview }, use, testInfo) => {
    const path = testInfo.outputPath("host.json");
    const child = spawn("python3", [
      "../tests/web_host.py",
      "--binary",
      // CI builds the release binary only, and timings are measured on it;
      // otherwise the debug one, which links fast.
      process.env.UAGENT_TEST_BINARY ||
        `../build/${process.env.CI || testInfo.file.includes("performance.spec") ? "release" : "debug"}/uagent`,
      "--port",
      "0",
      "--fixture",
      path,
      ...(noModel ? ["--no-model"] : []),
      ...(autoReview ? ["--auto-review"] : []),
    ]);
    const exited = once(child, "exit");
    let output = "",
      host;
    child.stdout.on("data", (data) => {
      output += data;
    });
    child.stderr.on("data", (data) => {
      output += data;
    });
    try {
      await expect
        .poll(
          async () => {
            if (child.exitCode !== null) throw new Error(output);
            try {
              host = JSON.parse(await readFile(path, "utf8"));
              return true;
            } catch (error) {
              if (error.code !== "ENOENT" && !(error instanceof SyntaxError))
                throw error;
              return false;
            }
          },
          { timeout: 15000, intervals: [25, 50, 100] },
        )
        .toBe(true);
      await use(host);
    } finally {
      // The host is stopped whatever reading its log does: one left running
      // outlives the run.
      try {
        if (host && testInfo.status !== testInfo.expectedStatus) {
          await testInfo.attach("native-host.log", {
            body: await readFile(
              resolve(dirname(host.project), "web-host.log"),
            ),
            contentType: "text/plain",
          });
        }
      } finally {
        const timer = setTimeout(() => child.kill("SIGKILL"), 15000);
        child.kill("SIGTERM");
        const [code, signal] = await exited.finally(() => clearTimeout(timer));
        expect({ code, signal }, output).toEqual({ code: 0, signal: null });
      }
    }
  },
  baseURL: async ({ host }, use) => use(host.origin),
  storageState: async ({ host, paired }, use) => {
    if (!paired) return use(undefined);
    const response = await fetch(`${host.origin}/api/auth`, {
      method: "POST",
      headers: { Origin: host.origin, "Content-Type": "application/json" },
      body: JSON.stringify({ code: host.code, name: "Isolated browser test" }),
    });
    expect(response.status).toBe(200);
    const [name, ...value] = response.headers
      .get("set-cookie")
      .split(";")[0]
      .split("=");
    await use({
      cookies: [
        {
          name,
          value: value.join("="),
          domain: "127.0.0.1",
          path: "/",
          expires: -1,
          httpOnly: true,
          secure: false,
          sameSite: "Strict",
        },
      ],
      origins: [],
    });
  },
  command: async ({ host, request }, use) => {
    await use(async (kind, fields = {}) => {
      const response = await request.post("/api/command", {
        headers: { Origin: host.origin },
        data: {
          v: 2,
          kind,
          request_id: crypto.randomUUID().replaceAll("-", ""),
          ...fields,
        },
      });
      expect(response.ok(), await response.text()).toBe(true);
      return response.json();
    });
  },
  session: async ({ host, request, command }, use) => {
    let { session } = await command("create", { cwd: host.project });
    ({ session } = await command("activate", { session_id: session.id }));
    await expect
      .poll(async () => {
        const response = await request.get(`/api/sessions/${session.id}`);
        const snapshot = await response.json();
        session = snapshot.metadata;
        return session.status;
      })
      .toBe("idle");
    await use(session);
  },
});

// Chooses this device's detail level from the open conversation's menu.
export async function chooseDetail(page, level) {
  await page
    .locator(".conversation-head")
    .getByLabel("Conversation menu", { exact: true })
    .click();
  // One row opens the levels in place, with a way back.
  await page.getByRole("menuitem", { name: /^Detail/ }).click();
  await expect(page.getByText("On this device.")).toBeVisible();
  await page.getByRole("menuitemradio", { name: level, exact: true }).click();
}

// Opens the transcript row whose line holds `text`, and returns its node.
export async function openRow(scope, text) {
  const row = scope.locator(".node-row", { hasText: text }).first();
  await expect(row).toBeVisible();
  if ((await row.getAttribute("aria-expanded")) === "false") await row.click();
  await expect(row).toHaveAttribute("aria-expanded", "true");
  return row.locator("..");
}
