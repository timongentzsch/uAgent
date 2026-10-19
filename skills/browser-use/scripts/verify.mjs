// Copyright 2026 Timon Gentzsch
// Usage: node verify.mjs --discover
//        node verify.mjs check.mjs [brief-revision]
// check.mjs exports default async ({ browser, page, target, check, scratch }).
import { createRequire } from "node:module";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { resolve, join } from "node:path";
import { pathToFileURL } from "node:url";
import { randomUUID } from "node:crypto";

export function discover(cwd = process.cwd()) {
  const roots = [cwd, join(cwd, "web")];
  try {
    roots.push(
      execFileSync("npm", ["root", "-g"], {
        encoding: "utf8",
        timeout: 5000,
        stdio: ["ignore", "pipe", "ignore"],
      }).trim(),
    );
  } catch {
    /* npm is optional when the workspace has Playwright. */
  }
  for (const root of roots) {
    const require = createRequire(join(resolve(root), "package.json"));
    for (const name of [
      "playwright",
      "playwright-core",
      "@playwright/cli/node_modules/playwright-core",
    ]) {
      try {
        // Global roots contain packages directly, workspace roots node_modules.
        let module;
        try {
          module = require.resolve(name);
        } catch {
          module = require.resolve(join(root, name));
        }
        const { chromium } = require(module);
        const candidates = [
          process.env.UAGENT_VERIFY_CHROME,
          chromium.executablePath(),
          "/usr/bin/google-chrome",
          "/usr/bin/chromium",
          "/usr/bin/chromium-browser",
          "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        ];
        const executablePath = candidates.find((p) => p && existsSync(p));
        if (executablePath) return { module, executablePath };
      } catch {
        /* Try another installed package. No downloads. */
      }
    }
  }
  throw new Error(
    "No installed Playwright + Chromium runtime found. Install playwright with its browser, or @playwright/cli and Chrome. Set UAGENT_VERIFY_CHROME for a custom executable.",
  );
}

export function target(value) {
  return /^(https?:|file:)/.test(value)
    ? value
    : pathToFileURL(resolve(value)).href;
}

export async function verify(entry, brief = "", timeoutMs = 60000) {
  if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 600000) {
    throw new Error("timeoutMs must be an integer from 1 to 600000");
  }
  const runtime = discover();
  const require = createRequire(import.meta.url);
  const { chromium } = require(runtime.module);
  const scratch = resolve(".uagent/scratch", `verify-${randomUUID()}`);
  mkdirSync(scratch, { recursive: true });
  writeFileSync(resolve(".uagent/scratch/.gitignore"), "*\n");
  const report = {
    status: "running",
    brief,
    entry: resolve(entry),
    runtime,
    checks: [],
  };
  const reportPath = join(scratch, "result.json");
  let browser;
  const save = () =>
    writeFileSync(reportPath, JSON.stringify(report, null, 2) + "\n");
  // Playwright owns a fresh temporary profile per launch. Never attach to the
  // user's Chrome or change its shared browser lease.
  let timedOut = false;
  let timer;
  const deadline = new Promise((_, reject) => {
    timer = setTimeout(() => {
      timedOut = true;
      reject(new Error(`Verification exceeded ${timeoutMs}ms`));
    }, timeoutMs);
  });
  const work = async () => {
    browser = await chromium.launch({
      executablePath: runtime.executablePath,
      headless: true,
      // Chrome also writes crash reports, MIME state and caches outside its
      // profile. Keep these in this job, inside the existing writable scope.
      env: {
        ...process.env,
        HOME: scratch,
        XDG_CONFIG_HOME: join(scratch, "config"),
        XDG_CACHE_HOME: join(scratch, "cache"),
        XDG_DATA_HOME: join(scratch, "data"),
      },
      timeout: Math.min(15000, timeoutMs),
    });
    if (timedOut) {
      await browser.close();
      return;
    }
    const page = await browser.newPage();
    page.setDefaultTimeout(10000);
    page.setDefaultNavigationTimeout(15000);
    const suite = await import(pathToFileURL(resolve(entry)).href);
    if (typeof suite.default !== "function")
      throw new Error("Check module must export a default async function");
    await suite.default({
      browser,
      page,
      target,
      scratch,
      check(name, passed, detail = "") {
        if (typeof passed !== "boolean")
          throw new Error(`Check ${name} must supply a boolean`);
        report.checks.push({ name, passed, detail });
      },
    });
    if (!report.checks.length) throw new Error("No checks recorded");
    if (!timedOut)
      report.status = report.checks.every((c) => c.passed)
        ? "passed"
        : "failed";
  };
  try {
    await Promise.race([work(), deadline]);
  } catch (error) {
    report.status = timedOut ? "timed_out" : "failed";
    report.error = String(error.stack || error);
  } finally {
    try {
      await browser?.close();
    } finally {
      clearTimeout(timer);
      save();
    }
  }
  console.log(
    JSON.stringify({
      status: report.status,
      report: reportPath,
      checks: report.checks.length,
    }),
  );
  return report.status === "passed";
}

if (
  process.argv[1] &&
  import.meta.url === pathToFileURL(resolve(process.argv[1])).href
) {
  try {
    if (process.argv[2] === "--discover")
      console.log(JSON.stringify(discover()));
    else if (!process.argv[2])
      throw new Error(
        "Usage: node verify.mjs --discover | check.mjs [brief-revision]",
      );
    else {
      const passed = await verify(
        process.argv[2],
        process.argv[3],
        process.argv[4] === undefined ? 60000 : Number(process.argv[4]),
      );
      // The suite may have left timers behind; its owned browser is closed.
      process.exit(passed ? 0 : 1);
    }
  } catch (error) {
    console.error(String(error));
    process.exitCode = 1;
  }
}
