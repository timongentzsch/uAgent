import { defineConfig, devices } from "@playwright/test";
// Playwright colors worker output by default. Preserve this repository's
// plain test logs without passing Node the conflicting NO_COLOR/FORCE_COLOR
// pair that emits one warning per worker.
delete process.env.NO_COLOR;
process.env.FORCE_COLOR = "0";
import { devServerUrl } from "./tests/dev-server.js";
export default defineConfig({
  testDir: "./tests",
  forbidOnly: !!process.env.CI,
  testMatch: "**/*.spec.js",
  // Each test has a host and ports of its own, so they run side by side.
  workers: process.env.CI ? 2 : 4,
  fullyParallel: true,
  // Deadlines, not delays: a test waits for an event and these only bound
  // how long. A turn against the real host takes seconds on a busy runner,
  // so the defaults leave room for one and the specs name no shorter ones.
  timeout: 60000,
  expect: { timeout: 15000 },
  // No retries: a test that passes only sometimes has found a race.
  retries: 0,
  // Geometry assertions must not sample a surface mid-transition.
  use: {
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
    reducedMotion: "reduce",
  },
  // render-count.spec.js loads the app from the dev server.
  webServer: {
    command: `npx vite --port ${new URL(devServerUrl).port} --strictPort --host 127.0.0.1`,
    url: devServerUrl,
    // A server already on the port may be another checkout's: fail, not reuse.
    reuseExistingServer: false,
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    {
      name: "webkit",
      testMatch:
        "**/{btw,guidance-queue,phone-layout,reload,mobile-navigation,loading-surfaces,session-state,ui-quality,browser,model-picker,ask,menu,image-viewer,dismiss,history-anchor,scroll-restore,scroll-stick,search}.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
});
