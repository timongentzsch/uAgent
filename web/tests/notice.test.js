// A notice's tone is its colour, so each must read (4.5:1) on every surface
// a notice sits on, in both palettes.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const read = (path) => readFileSync(`src/shared/${path}`, "utf8");
const luminance = (hex) => {
  const [r, g, b] = (hex.length === 4 ? hex.replace(/\w/g, "$&$&") : hex)
    .match(/\w\w/g)
    .map((pair) => parseInt(pair, 16) / 255)
    .map((c) => (c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
};

test("each tone of a notice reads on every surface, in both palettes", () => {
  const tones = read("ui.tsx")
    .match(/toneIcons = \{(.+?)\}/s)[1]
    .match(/\w+(?=:)/g);
  assert.deepEqual(tones, ["error", "warning", "info"]);
  const styles = read("styles/primitives.css");
  const palettes = read("styles/tokens.css").split("color-scheme:").slice(1);
  assert.equal(palettes.length, 2);
  for (const palette of palettes) {
    const token = (name) =>
      luminance(palette.match(new RegExp(`--${name}: (#\\w+);`))[1]);
    for (const tone of tones) {
      // Info has no rule: it takes the text's colour.
      const ink =
        styles.match(
          new RegExp(`data-tone="${tone}"[^}]+var\\(--(\\w+)\\)`),
        )?.[1] ?? "text";
      for (const surface of ["bg", "surface", "raised"]) {
        const [dark, light] = [token(ink), token(surface)].sort(
          (a, b) => a - b,
        );
        assert.ok(
          (light + 0.05) / (dark + 0.05) >= 4.5,
          `${tone} on --${surface}`,
        );
      }
    }
  }
});
