import { defineConfig, devices } from "@playwright/test";
// Playwright colors worker output by default. Preserve this repository's
// plain test logs without passing Node the conflicting NO_COLOR/FORCE_COLOR
// pair that emits one warning per worker.
delete process.env.NO_COLOR;
process.env.FORCE_COLOR = "0";
export default defineConfig({
  testDir: "./tests",
  forbidOnly: !!process.env.CI,
  testMatch: "**/*.spec.js",
  workers: process.env.CI ? 2 : 1,
  fullyParallel: true,
  timeout: 30000,
  // Sub-frame browser timing (restore vs. paint vs. async row hydration)
  // can strand one attempt of a scroll assertion a few pixels off; the
  // app code serializes everything serializable, and a single retry
  // covers the residual race without hiding systematic failures.
  retries: process.env.CI ? 2 : 1,
  use: { trace: "retain-on-failure", screenshot: "only-on-failure" },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    {
      name: "webkit",
      testMatch:
        "**/{ui,ui-quality,browser,showcase,history-anchor,scroll-restore,scroll-stick}.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
});
