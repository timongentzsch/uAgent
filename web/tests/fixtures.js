import { test as base, expect } from "@playwright/test";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";

export { expect };

// What the Files pane's tests put into a conversation's folder: a page with
// an artboard (which makes a design), a slide with its notes in its last
// aside, a document on paper, and a sheet, each tab a table of typed rows.
export const artboard = (title, body, size = "1440x900") =>
  `<!doctype html><html><head><title>${title}</title>` +
  `<meta name="artboard" content="${size}"></head><body>${body}</body></html>`;
export const slide = (title, notes = "", body = "") =>
  artboard(
    title,
    `<h1>${title}</h1>${body}${notes ? `<aside>${notes}</aside>` : ""}`,
    "1920x1080 slide",
  );
export const paper = (title, body, sheet = "a5", head = "") =>
  `<!doctype html><html><head><title>${title}</title>` +
  `<meta name="print" content="flow ${sheet}">${head}</head><body>${body}</body></html>`;

export const sheet = (title, tabs) =>
  `<!doctype html>\n<meta charset="utf-8">\n<meta name="sheet" content="1">\n` +
  `<title>${title}</title>\n${Object.entries(tabs)
    .map(
      ([name, rows]) =>
        // A row is its cells; one that is a tag itself (its <col>s, a <tr>
        // with a class) is written whole, and a chart after the table.
        `<table data-sheet="${name}">\n${rows
          .filter((row) => !row.startsWith("<figure"))
          .map((row) => (/^<(col|tr)\b/.test(row) ? row : `<tr>${row}`))
          .join(
            "\n",
          )}\n${["</table>", ...rows.filter((row) => row.startsWith("<figure"))].join("\n")}`,
    )
    .join("\n")}\n`;
// A plan of three tabs: counts and a total over its tasks, a header band
// under the summary, a value read from another tab, and two cells in error.
export const SAMPLE_SHEET = sheet("Launch plan", {
  Tasks: [
    "<td>Done<td>=COUNTIF(B4:B6,A1)<td>",
    "<td><td><td>",
    "<th>Task<th>Status<th>Cost",
    "<td>Write the plan<td>Done<td>1200",
    "<td>Book the venue<td>Done<td>8400.5",
    "<td>Brief the press<td>To do<td>=1/0",
    '<td>Total<td><td data-format="#,##0.00">=SUM(C4:C5)',
  ],
  Inputs: [
    "<th>Name<th>Value",
    "<td>Budget<td>=Tasks!C7*2",
    "<td>Rate<td>=NOPE(1)",
  ],
  Notes: ["<td>Nothing yet"],
});

// Writes files into a folder by their paths in it, making its folders.
export async function writeFiles(root, files) {
  for (const [name, content] of Object.entries(files)) {
    await mkdir(dirname(join(root, name)), { recursive: true });
    await writeFile(join(root, name), content);
  }
}

// The conversation in a desktop's window, or a phone's.
export async function openSession(page, session, { phone } = {}) {
  await page.setViewportSize(
    phone ? { width: 390, height: 844 } : { width: 1440, height: 900 },
  );
  await page.goto(`/#session=${session.id}`);
}

// The conversation with Files open at a place in its folder: each folder of
// `path` opened in the tree, then its last part: a file, or with a closing
// slash the folder's own Open. Returns the pane.
export async function openFiles(page, session, path = "", options = {}) {
  await openSession(page, session, options);
  await headerAction(page, "Files");
  const files = page.getByRole(options.phone ? "dialog" : "region", {
    name: "Files",
  });
  for (const name of path.split("/").filter(Boolean))
    await files.getByRole("button", { name, exact: true }).click();
  if (path.endsWith("/"))
    await files
      .locator(`.tree-folder:has([data-path="${path.slice(0, -1)}"])`)
      .getByRole("button", { name: "Open", exact: true })
      .click();
  return files;
}

// A sheet written as sheets/plan.html (or at `options.path`, as text or as a
// workbook's bytes) and opened in the pane: its page, its tabs, its formula
// bar, a cell of a tab by its name ("B2"), and the file as it is on disk.
export async function openSheet(page, session, project, source, options) {
  const path = options?.path ?? "sheets/plan.html";
  await writeFiles(project, { [path]: source });
  const files = await openFiles(page, session, path, options);
  const { stage } = viewerOf(files);
  const bar = files.getByRole("group", { name: "Formula bar" });
  const columnOf = (name) =>
    [...name].reduce((n, letter) => n * 26 + letter.charCodeAt(0) - 64, 0);
  return {
    files,
    stage,
    bar,
    field: bar.getByRole("textbox", { name: "Cell contents" }),
    tabs: files.getByRole("tablist", { name: "Tabs" }).getByRole("tab"),
    cell: (tab, ref) => {
      const [, name, row] = /^([A-Z]+)(\d+)$/.exec(ref);
      return stage.locator(
        `table[data-sheet="${tab}"] [data-row="${row}"][data-column="${columnOf(name)}"]`,
      );
    },
    saved: (at = path) => readFile(join(project, at), "utf8"),
  };
}

// The viewer's stage (its page) and its counter, in the pane.
export const viewerOf = (files) => ({
  stage: files.frameLocator(".page-stage iframe"),
  count: files.getByRole("group", { name: "Step" }).locator("output"),
});

// The serving host connection, independent of remote-host status rows.
export const connected = (page) =>
  page.getByRole("status").filter({ hasText: /^Connected$/ });

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

// The composer's totals open the statistics, which hold the way to the raw
// context.
export async function openRawContext(page) {
  await page
    .getByRole("button", { name: "Conversation statistics", exact: true })
    .click();
  await page
    .getByRole("dialog", { name: "Conversation statistics" })
    .getByRole("button", { name: "Raw context", exact: true })
    .click();
}

// Scrolls a transcript up from its end. It has to be there first: until the
// app has pinned it, this is no move up, and the next pin undoes it.
export async function leaveEnd(box) {
  await expect
    .poll(() =>
      box.evaluate((node) => {
        const room = node.scrollHeight - node.clientHeight;
        return room > 80 && room - node.scrollTop <= 2;
      }),
    )
    .toBe(true);
  // Told at once: a pin landing before the browser's own event would undo it.
  await box.evaluate((node) => {
    node.scrollTop = 24;
    node.dispatchEvent(new Event("scroll"));
  });
}

// New ▾ Folder…, a path on the host, Start: a conversation with its runtime.
// The dialog stays over it until it has loaded: typing before then is lost.
export async function startInFolder(page, path) {
  await page.getByRole("button", { name: "New", exact: true }).click();
  await page.getByRole("menuitem", { name: "Folder…" }).click();
  await page.getByLabel("Folder on the host").fill(path);
  const dialog = page.getByRole("dialog", { name: "New folder" });
  await dialog
    .getByRole("button", { name: "Start conversation", exact: true })
    .click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".composer .activity-button")).toHaveCount(0);
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

// A saved conversation with one exchange, as the host lists it: the list
// leaves out an empty one unless it is open.
export async function savedConversation(host, request, title, reply = "Done") {
  const id = crypto.randomUUID();
  await saveHistory(host, `saved-${id}.json`, {
    id,
    title,
    turns: 1,
    messages: [
      { role: "user", content: title },
      { role: "assistant", content: reply },
    ],
  });
  let found;
  await expect
    .poll(async () => {
      const response = await request.get("/api/sessions?refresh=1");
      const { sessions } = await response.json();
      found = sessions.find((item) => item.title === title);
      return !!found;
    })
    .toBe(true);
  return found;
}

// Opens the conversation menu's More view, with the menu already open.
export const openMore = (page) =>
  page.getByRole("menuitem", { name: "More", exact: true }).click();

// Brings the list into view: it is beside the conversation, or in a drawer.
// Which one can change as this runs, after a zoom or a resize, so it looks
// again until the list is there rather than acting on one look.
export const showList = (page) =>
  expect(async () => {
    const drawer = page.getByRole("button", { name: "Open conversations" });
    if (await drawer.isVisible()) await drawer.click({ timeout: 1000 });
    await expect(
      page.locator("dialog.sidebar, aside.sidebar").filter({ visible: true }),
    ).toHaveCount(1, { timeout: 1000 });
  }).toPass();

// What a wider header shows as a button beside the title and a narrow column
// keeps in the conversation's menu. A button's name may say more after it
// ("Open browser, agent working"). With no conversation there is no menu.
export async function headerAction(page, name) {
  const head = page.locator(".conversation-head");
  const menu = head.getByRole("button", { name: "Conversation menu" });
  const narrow = await head.evaluate(
    (node) =>
      getComputedStyle(node).getPropertyValue("--narrow").trim() === "1",
  );
  if (narrow && (await menu.count())) {
    if (name === "Settings") {
      await showList(page);
      return page
        .locator("dialog.sidebar, aside.sidebar")
        .getByRole("button", { name: "Settings", exact: true })
        .click();
    }
    await menu.click();
    if (name === "Open browser") await openMore(page);
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
  route: "parent",
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

// WebKit logs a request a reload cancels as a console error, fetch or event
// stream, even when the page handles it, and Playwright reports those as page
// errors. Chromium still catches a rejection nothing handles.
const cancelled = / due to access control checks\.$/;

// No test may end with an uncaught page error.
const failOnPageErrors = async ({ page }, use) => {
  const errors = [];
  page.on("pageerror", (error) => {
    if (!cancelled.test(error.message))
      errors.push(error.stack || error.message);
  });
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
      // CI names the binary it built; otherwise the debug one, which links
      // fast.
      process.env.UAGENT_TEST_BINARY || "../build/debug/uagent",
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
  await openMore(page);
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
