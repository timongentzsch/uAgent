// /theme.js runs before any module, written out from boot/theme.ts with the
// app's own keys, zoom range, panes and backgrounds: run here on a stub page.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import { themeScript } from "../src/boot/theme.ts";
import { storageKeys } from "../src/shared/storage-keys.ts";

function page(kept, dark = false) {
  const properties = {};
  const metas = [
    { media: "(prefers-color-scheme: light)", dataset: {}, content: "" },
    { media: "(prefers-color-scheme: dark)", dataset: {}, content: "" },
  ];
  const status = { content: "" };
  const root = {
    dataset: {},
    clientWidth: 390,
    clientHeight: 844,
    style: { setProperty: (name, value) => (properties[name] = value) },
  };
  runInNewContext(
    themeScript(readFileSync("src/shared/styles/tokens.css", "utf8")),
    {
      history: {},
      localStorage: { getItem: (key) => kept[key] ?? null },
      matchMedia: (query) => ({
        matches: query.includes("prefers-color-scheme: dark") && dark,
      }),
      document: {
        documentElement: root,
        querySelectorAll: () => metas,
        querySelector: () => status,
      },
    },
  );
  return { root, properties, metas, status };
}

test("the kept theme, zoom and pane sizes apply before the app loads", () => {
  const { root, properties, metas, status } = page({
    [storageKeys.theme]: "dark",
    [storageKeys.zoom]: "300",
    [storageKeys.panes]: JSON.stringify({ sidebar: 20, terminal: 12 }),
  });
  assert.equal(root.dataset.theme, "dark");
  assert.equal(properties["--zoom"], "2");
  assert.equal(properties["--sidebar-width"], "20rem");
  assert.equal(properties["--terminal-height"], "12rem");
  assert.deepEqual(
    metas.map((meta) => [meta.dataset.bg, meta.content]),
    [
      ["#fff", "#000"],
      ["#000", "#000"],
    ],
  );
  assert.equal(status.content, "black-translucent");
});

test("with nothing kept it follows the system at the default zoom", () => {
  const { root, properties, metas } = page({}, false);
  assert.equal(root.dataset.theme, "light");
  assert.equal(properties["--zoom"], "1");
  assert.deepEqual(
    metas.map((meta) => meta.content),
    ["#fff", "#000"],
  );
});
