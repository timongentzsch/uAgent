import { test as base, expect } from "@playwright/test";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { readFile } from "node:fs/promises";
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

// The UI showcase is a development page: the Vite dev server serves it
// (see playwright.config.js), not the native host.
export { showcaseUrl as SHOWCASE_URL } from "./showcase-url.js";

// No test may end with an uncaught page error.
const failOnPageErrors = async ({ page }, use) => {
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.stack || error.message));
  await use(page);
  expect(errors, "uncaught page errors").toEqual([]);
};

// Showcase-only tests: no native host.
export const showcaseTest = base.extend({ page: failOnPageErrors });

export const test = base.extend({
  paired: [true, { option: true }],
  page: failOnPageErrors,
  host: async ({}, use, testInfo) => {
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
