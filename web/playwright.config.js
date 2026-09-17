import { defineConfig, devices } from "@playwright/test";
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
      testMatch: "**/ui.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
});
