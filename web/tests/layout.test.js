// Layout is built from the shared pieces in shared/ui.tsx. What predates
// them is listed in layout-baseline.json, which only shrinks: a finding not
// listed fails, and so does a listing that is no longer found.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as found from "./layout-rules.js";

const baseline = JSON.parse(readFileSync("tests/layout-baseline.json", "utf8"));
const held = (name, hint) =>
  test(name, () => assert.deepEqual(found[hint](), baseline[hint]));

held("text is cut short by .truncate", "truncations");
held("a button's size comes from its variant or its list", "buttonSizes");
held("a field shares a row through InputRow", "flexedFields");
held("bars and rows of controls are the shared ones", "wrappers");

// A query cannot read a custom property, so each width is written out. This
// is the whole set; a new one is a decision, not a number in a stylesheet.
test("container and media queries ask about a fixed set of sizes", () => {
  assert.deepEqual(found.breakpoints(), [
    "app max-height: 500px",
    "app min-width: 44.28rem",
    "column max-width: 43rem",
    "composer min-width: 40rem",
    "files max-width: 40rem",
    "files min-width: 40.0625rem",
    "media max-width: 600px",
    "media min-width: 601px",
  ]);
});
