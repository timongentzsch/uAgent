// Layout is built from the shared pieces in shared/ui.tsx. A feature's rule
// or element that is its own all the same says why, in a comment that begins
// "raw:" above it; anything else found here fails.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as found from "./layout-rules.js";
import { breakpoints } from "../src/shared/breakpoints.ts";

const held = (name, hint) =>
  test(name, () => assert.deepEqual(found[hint](), []));

held("text is cut short by .truncate", "truncations");
held("a button's size comes from its variant or its list", "buttonSizes");
held("a field shares a row through InputRow", "flexedFields");
held("bars and rows of controls are the shared ones", "wrappers");

// A query cannot read a custom property, so each size is named once in
// shared/breakpoints.ts and the build writes it into the queries: a size in a
// stylesheet's query, or a second breakpoint for the same edge, fails here.
test("container and media queries ask only about the named breakpoints", () => {
  const { sizes, names } = found.queries();
  assert.deepEqual(sizes, []);
  assert.deepEqual(names, Object.keys(breakpoints).sort());
});
