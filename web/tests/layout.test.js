// Layout is built from the shared pieces in shared/ui.tsx. A feature's rule
// or element that is its own all the same says why, in a comment that begins
// "raw:" above it; anything else found here fails.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as found from "./layout-rules.js";

const held = (name, hint) =>
  test(name, () => assert.deepEqual(found[hint](), []));

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
