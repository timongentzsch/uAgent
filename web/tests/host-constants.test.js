// Constants the page mirrors from the native host, read from its headers so
// a change on one side fails here rather than in a browser.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { protocol } from "../src/state/api.ts";
import * as limits from "../src/shared/limits.ts";

const UNIT = { KiB: 1024, MiB: 1024 * 1024 };

// A constant's value in a header: a number, or KiB(n) or MiB(n).
function host(header, name) {
  const source = readFileSync(
    new URL(`../../include/${header}`, import.meta.url),
    "utf8",
  );
  const [, unit, value] =
    source.match(
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
    [limits.maxUploadBytes, "app/asset_store.h", "kUploadBytes"],
    [limits.maxTerminals, "core/limits.h", "kWebTerminals"],
  ])
    assert.equal(web * scale, host(header, name), name);
});
