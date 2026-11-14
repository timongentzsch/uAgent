// What the layout tests look for in the stylesheets and the screens. Each
// finding is named by its file and selector (or class), so a moved line is
// not a new finding.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { declarations, sheets, tree } from "./stylesheets.js";

const central = (path) => path.startsWith("shared/styles/");
const file = ({ where }) => where.slice(0, where.lastIndexOf(":"));
// Each alternative of a rule's selector, and the element it ends on.
const parts = ({ selector }) =>
  selector.split(",").map((part) => part.replace(/\s+/g, " ").trim());
const last = (part) =>
  part
    .replace(/:(?:has|not|is|where)\([^)]*\)/g, "")
    .split(/[\s>+~]+/)
    .pop();
const ends = (rule, pattern) =>
  parts(rule).some((part) => pattern.test(last(part)));
const named = (rule) =>
  `${file(rule)} ${rule.selector.replace(/\s+/g, " ").trim()}`;
const unique = (all) => [...new Set(all)].sort();

// The same rule's declarations, by property.
const rules = new Map();
for (const declaration of declarations) {
  const key = `${file(declaration)}\n${declaration.rule}`;
  if (!rules.has(key)) rules.set(key, { ...declaration, set: new Map() });
  rules.get(key).set.set(declaration.property, declaration.value);
}
const scoped = [...rules.values()].filter(
  (rule) => !central(file(rule)) && !file(rule).startsWith("sheet/"),
);

// Text cut with an ellipsis is .truncate.
export const truncations = () =>
  unique(
    scoped
      .filter(
        ({ set }) =>
          set.get("text-overflow") === "ellipsis" &&
          set.get("white-space") === "nowrap",
      )
      .map(named),
  );

// A button's size is its variant's, or its list's density.
export const buttonSizes = () =>
  unique(
    scoped
      .filter(
        (rule) =>
          ends(rule, /^button\b|-button\b/) &&
          ["min-height", "padding", "padding-inline", "font-size"].some(
            (property) => rule.set.has(property),
          ),
      )
      .map(named),
  );

// A field shares a row through InputRow, which keeps its room.
export const flexedFields = () =>
  unique(
    scoped
      .filter(
        (rule) =>
          ends(
            rule,
            /^(?:input|textarea)(?![^\s]*checkbox)|\.text-control\b/,
          ) &&
          ["flex", "flex-grow", "flex-basis"].some((property) =>
            rule.set.has(property),
          ),
      )
      .map(named),
  );

// Every width or height a container or media query asks about.
export const breakpoints = () => {
  const found = new Set();
  for (const { css } of sheets)
    for (const [, kind, name, query] of css.matchAll(
      /@(container|media)\s+(?:(\w+)\s+)?([^{]*)\{/g,
    ))
      for (const [, edge, size] of query.matchAll(
        /\((m(?:ax|in)-(?:width|height)):\s*([\d.]+(?:px|rem))\)/g,
      ))
        found.add(`${kind === "media" ? "media" : name} ${edge}: ${size}`);
  return [...found].sort();
};

// A screen's bars and rows of controls are Bar, Cluster and Actions.
export const wrappers = () => {
  const found = [];
  for (const path of tree("src", [".tsx"])) {
    if (!/^(?:features|app)\//.test(path)) continue;
    for (const [, name] of readFileSync(join("src", path), "utf8").matchAll(
      /<(?:div|header|footer|form|span|section|nav)\b[^>]*?\bclass="[^"]*?\b([\w-]+-(?:actions|bar|toolbar|head))\b/g,
    ))
      found.push(`${path} ${name}`);
  }
  return unique(found);
};
