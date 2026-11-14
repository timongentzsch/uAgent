import { defineConfig, devices } from "@playwright/test";
// Plain worker logs. Passing Node both NO_COLOR and FORCE_COLOR emits one
// warning per worker.
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
  // Deadlines, not delays: a turn against the real host takes seconds on a
  // busy runner.
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
        "**/{action,btw,guidance-queue,phone-layout,reload,mobile-navigation,loading-surfaces,session-state,ui-quality,browser,model-picker,ask,menu,image-viewer,dismiss,history-anchor,scroll-restore,scroll-stick,search,screen-motion,canvas,sheet,sheet-edit,sheet-files,sheet-rules,sheet-suggest,management,selection,hosts,doc-edit}.spec.js",
      use: { ...devices["Desktop Safari"] },
    },
  ],
});
