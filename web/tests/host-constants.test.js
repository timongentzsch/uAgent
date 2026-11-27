// Values the page repeats, from the native host or from its own modules where
// a file cannot import them, read from the source so a change on one side
// fails here rather than in a browser.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { localRoutes, protocol } from "../src/state/api.ts";
import { zoomSteps } from "../src/shared/layout.ts";
import { storageKeys } from "../src/shared/storage-keys.ts";
import * as limits from "../src/shared/limits.ts";

const UNIT = { KiB: 1024, MiB: 1024 * 1024 };

const source = (path) =>
  readFileSync(new URL(`../../${path}`, import.meta.url), "utf8");

// A constant's value in a header: a number, or KiB(n) or MiB(n).
function host(header, name) {
  const [, unit, value] =
    source(`include/${header}`).match(
      new RegExp(`constexpr \\w+ ${name} = (?:(KiB|MiB)\\()?(\\d+)\\)?;`),
    ) ?? [];
  assert.ok(value, `${name} in ${header} is a literal`);
  return Number(value) * (UNIT[unit] ?? 1);
}

test("mirrored constants match the native host", () => {
  for (const [web, header, name, scale = 1] of [
    [protocol, "transport/session.h", "kProtocol"],
    [limits.maxTitleChars, "core/limits.h", "kUserTitleChars"],
    [limits.maxSpaceNameChars, "core/limits.h", "kSpaceNameChars"],
    [limits.maxScheduleMinutes, "core/limits.h", "kSecondsPerYear", 60],
    [limits.maxDraftFiles, "app/asset_store.h", "kUploadCount"],
    [limits.maxTerminals, "core/limits.h", "kWebTerminals"],
    [limits.maxTextFileBytes, "core/limits.h", "kFileViewBytes"],
  ])
    assert.equal(web * scale, host(header, name), name);
  assert.match(
    source("include/core/fs.h"),
    new RegExp(`kUnfiledSpace = "${limits.scratchSpace}";`),
  );
});

// A session id is 16 to 64 lowercase hex characters on both sides.
test("the page reads session ids as the host makes them", () => {
  const [, least, most] = source("src/transport/session.cc").match(
    /value\.size\(\) >= (\d+) && value\.size\(\) <= (\d+) &&\s*value\.find_first_not_of\("0123456789abcdef"\)/,
  );
  for (const file of ["web/src/sw.ts", "web/src/app/use-boot.ts"]) {
    const bounds = [...source(file).matchAll(/a-f0-9\]\{(\d+),(\d+)\}/g)];
    assert.ok(bounds.length, file);
    for (const [, low, high] of bounds)
      assert.deepEqual([low, high], [least, most], file);
  }
});

// Each path the page calls is one the host serves.
test("every route of the page has a host handler", () => {
  const patterns = [
    ...new Set(
      readdirSync(new URL("../../src/web", import.meta.url))
        .filter((file) => file.endsWith(".cc"))
        .flatMap((file) => [
          ...source(`src/web/${file}`).matchAll(
            /(?:R"\((\/api\/[^"]*?)\)"|"(\/api\/[^"]+)")/g,
          ),
        ])
        .map(([, raw, plain]) => new RegExp(`^${raw ?? plain}$`)),
    ),
  ];
  // One 32-hex id fits every id the host's patterns take.
  const id = "0123456789abcdef0123456789abcdef";
  const paths = Object.entries(localRoutes).map(([name, route]) => [
    name,
    typeof route === "string"
      ? route
      : route(id, name === "fileOrigin" ? { path: "x" } : id),
  ]);
  for (const [name, path] of paths) {
    const bare = path.split("?")[0];
    // `assets` is the prefix an asset's id is added to.
    const served = name === "assets" ? `${bare}${id}` : bare;
    assert.ok(
      patterns.some((pattern) => pattern.test(served)),
      `${name}: ${served}`,
    );
  }
});

// The browser's page sizes, as the Browser popup offers them.
test("the Browser popup offers the host's viewports", () => {
  const host = [
    ...source("src/browser/runtime_control.cc").matchAll(
      /\{"(\w+)", (\d+), (\d+), (?:true|false)\}/g,
    ),
  ].map(([, name, width, height]) => `${name} ${width}x${height}`);
  const page = [
    ...source("web/src/features/browser/browser.tsx").matchAll(
      /<option value="(\w+)">\w+ · (\d+) × (\d+)<\/option>/g,
    ),
  ].map(([, name, width, height]) => `${name} ${width}x${height}`);
  assert.ok(host.length, "kViewports");
  assert.deepEqual(page, host);
});

// theme.js runs before any module, so it repeats what it needs.
test("theme.js repeats the keys, zoom range and panes it restores", () => {
  const theme = source("web/public/theme.js");
  for (const key of ["theme", "zoom", "panes"])
    assert.ok(theme.includes(`"${storageKeys[key]}"`), key);
  const [, low, high] = theme.match(/Math\.min\((\d+), Math\.max\((\d+),/);
  assert.deepEqual(
    [Number(high), Number(low)],
    [zoomSteps[0], zoomSteps.at(-1)],
  );
  assert.ok(theme.includes(`|| "${limits.defaultZoom}"`), "defaultZoom");
  const panes = (list) =>
    [...list.matchAll(/"([\w-]+)"/g)].map(([, n]) => n).sort();
  const restored = theme.match(/for \(const name of \[([^\]]*)\]/)[1];
  const typed = source("web/src/shared/device-prefs.ts").match(
    /export type Pane = ([^;]*);/,
  )[1];
  assert.deepEqual(panes(restored), panes(typed));
});
