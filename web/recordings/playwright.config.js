// The recordings under docs/images: each .rec.js is one film. Run through
// `npm run record`, which turns the videos into the GIFs.
import { defineConfig, devices } from "@playwright/test";
export default defineConfig({
  testDir: ".",
  testMatch: "**/*.rec.js",
  outputDir: "../test-results/recordings",
  workers: 1,
  timeout: 120000,
  use: {
    ...devices["Desktop Chrome"],
    colorScheme: "dark",
    video: "on",
  },
});
