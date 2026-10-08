import { defineConfig, devices } from "@playwright/test";
// Playwright colors worker output by default. Preserve this repository's
// plain test logs without passing Node the conflicting NO_COLOR/FORCE_COLOR
// pair that emits one warning per worker.
delete process.env.NO_COLOR;
process.env.FORCE_COLOR = "0";
import { showcaseUrl } from "./tests/showcase-url.js";
export default defineConfig({
  testDir: "./tests",
  forbidOnly: !!process.env.CI,
  testMatch: "**/*.spec.js",
  // Each test has a host and ports of its own, so they run side by side.
  workers: process.env.CI ? 3 : 4,
  fullyParallel: true,
  // Deadlines, not delays: a test waits for an event and these only bound
  // how long. A turn against the real host takes seconds on a busy runner,
  // so the defaults leave room for one and the specs name no shorter ones.
  timeout: 60000,
  expect: { timeout: 15000 },
  // Sub-frame browser timing (restore vs. paint vs. async row hydration)
  // can strand one attempt of a scroll assertion a few pixels off; the
  // app code serializes everything serializable, and a single retry
  // covers the residual race without hiding systematic failures.
  retries: 1,
  // Geometry assertions must not sample a surface mid-transition.
  use: {
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
    reducedMotion: "reduce",
  },
  // The UI showcase (ui.html) is not part of the product build.
  webServer: {
    command: `npx vite --port ${new URL(showcaseUrl).port} --strictPort --host 127.0.0.1`,
    url: showcaseUrl,
    // A server already on the port may be another checkout's: fail, not reuse.
    reuseExistingServer: false,
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    {
      name: "webkit",
      testMatch:
        "**/{ui,ui-quality,browser,showcase,dismiss,history-anchor,scroll-restore,scroll-stick,coordinator}.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
});
