// Films the recordings and writes each as docs/images/<name>.gif.
// Needs ffmpeg and a built host: UAGENT_TEST_BINARY, or build/debug/uagent.
import { execFileSync } from "node:child_process";
import { readdirSync } from "node:fs";
import { join } from "node:path";

const only = process.argv.slice(2);
execFileSync(
  "npx",
  [
    "playwright",
    "test",
    "--config",
    "recordings/playwright.config.js",
    ...only,
  ],
  { stdio: "inherit" },
);
const results = "test-results/recordings";
for (const folder of readdirSync(results, { withFileTypes: true })) {
  if (!folder.isDirectory()) continue;
  const video = readdirSync(join(results, folder.name)).find((name) =>
    name.endsWith(".webm"),
  );
  // "files.rec.js-Files" films docs/images/files.gif.
  const name = folder.name.split(".rec.")[0];
  if (!video) continue;
  const out = `../docs/images/${name}.gif`;
  // One palette for the whole film keeps flat UI colours from shimmering.
  execFileSync(
    "ffmpeg",
    [
      "-y",
      "-loglevel",
      "error",
      "-i",
      join(results, folder.name, video),
      "-vf",
      "fps=12,scale=1000:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle",
      out,
    ],
    { stdio: "inherit" },
  );
  console.log(`wrote ${out}`);
}
