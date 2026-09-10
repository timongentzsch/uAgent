import { defineConfig, devices } from "@playwright/test";
export default defineConfig({
  testDir: "./tests",
  testMatch: "**/*.spec.js",
  workers: 1,
  fullyParallel: false,
  timeout: 30000,
  use: { baseURL: "http://127.0.0.1:8765", trace: "retain-on-failure" },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    {
      name: "webkit",
      testMatch: "**/ui.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
  webServer: {
    command:
      "python3 ../tests/web_host.py --binary ../build/release/uagent --port 8765 --fixture test-results/host.json",
    url: "http://127.0.0.1:8765/",
    reuseExistingServer: false,
    timeout: 30000,
  },
});
