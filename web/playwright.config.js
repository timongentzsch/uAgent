import { defineConfig, devices } from "@playwright/test";
export default defineConfig({
  testDir: "./tests",
  forbidOnly: !!process.env.CI,
  testMatch: "**/*.spec.js",
  workers: process.env.CI ? 2 : 1,
  fullyParallel: true,
  timeout: 30000,
  use: { trace: "retain-on-failure" },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    {
      name: "webkit",
      testMatch: "**/ui.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
});
