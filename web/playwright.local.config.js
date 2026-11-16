// The browser specs on a developer's Mac, in the Chrome installed there
// instead of Playwright's own build. The canvas specs fail in it for the
// environment, at a clean checkout too, so they run only with PW_CANVAS=1;
// CI runs them all in Playwright's browsers (playwright.config.js).
import base from "./playwright.config.js";

export default {
  ...base,
  projects: [
    {
      name: "chromium",
      use: { ...base.projects[0].use, channel: "chrome" },
      ...(process.env.PW_CANVAS ? {} : { testIgnore: "**/canvas.spec.js" }),
    },
  ],
};
