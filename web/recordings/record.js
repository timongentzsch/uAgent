// Films the recordings and writes each as docs/images/<name>.gif.
// Needs ffmpeg and a built host: UAGENT_TEST_BINARY, or build/debug/uagent.
import { execFileSync } from "node:child_process";
import { readdirSync, readFileSync } from "node:fs";
import { isAbsolute, join } from "node:path";

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
  // The fixture's folders go outside /tmp, /var/tmp and /dev: a project
  // inside one of those makes the sandbox say so in the terminal being
  // filmed.
  {
    stdio: "inherit",
    env: {
      ...process.env,
      UAGENT_FIXTURE_ROOT: process.env.XDG_RUNTIME_DIR || "/var/tmp",
    },
  },
);
const results = "test-results/recordings";
// One palette for the whole film keeps flat UI colours from shimmering.
const gif =
  "split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle";
const ffmpeg = (inputs, filter, out) => {
  execFileSync(
    "ffmpeg",
    ["-y", "-loglevel", "error", ...inputs, "-filter_complex", filter, out],
    { stdio: "inherit" },
  );
  console.log(`wrote ${out}`);
};
for (const folder of readdirSync(results, { withFileTypes: true })) {
  if (!folder.isDirectory()) continue;
  const files = readdirSync(join(results, folder.name));
  // A film of several windows says in film.json how they sit side by side.
  if (files.includes("film.json")) {
    const film = JSON.parse(
      readFileSync(join(results, folder.name, "film.json"), "utf8"),
    );
    const inputs = film.parts.flatMap((part) => [
      "-ss",
      String(part.start),
      "-t",
      String(film.seconds),
      "-i",
      isAbsolute(part.path) ? part.path : join(results, folder.name, part.path),
    ]);
    const parts = film.parts
      .map(
        (part, index) =>
          `[${index}:v]fps=12,crop=${part.crop.join(":")},scale=-2:${film.height}:flags=lanczos[v${index}]`,
      )
      .join(";");
    const row = `${film.parts.map((_, index) => `[v${index}]`).join("")}hstack=inputs=${film.parts.length},${gif}`;
    ffmpeg(inputs, `${parts};${row}`, `../docs/images/${film.name}.gif`);
    continue;
  }
  const video = files.find((name) => name.endsWith(".webm"));
  if (!video) continue;
  // "files.rec.js-Files" films docs/images/files.gif.
  const name = folder.name.split(".rec.")[0];
  ffmpeg(
    ["-i", join(results, folder.name, video)],
    `fps=12,scale=1000:-1:flags=lanczos,${gif}`,
    `../docs/images/${name}.gif`,
  );
}
