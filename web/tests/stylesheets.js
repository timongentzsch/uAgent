// The stylesheets, parsed once for the tests that hold them to rules.
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

export function* tree(dir, extensions, base = "") {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(base, entry.name);
    if (entry.isDirectory())
      yield* tree(join(dir, entry.name), extensions, path);
    else if (extensions.some((end) => entry.name.endsWith(end))) yield path;
  }
}

// Every declaration in the stylesheets: where it is, the selector (and
// at-rules) it sits in, its property and its value. A sheet page's grid is
// another document's, with a palette of its own and none of the app's scales.
export const declarations = [];
export const sheets = [
  ...[...tree("src", [".css"])].map((path) => ({
    path,
    css: readFileSync(join("src", path), "utf8"),
  })),
  { path: "sheet/sheet.css", css: readFileSync("sheet/sheet.css", "utf8") },
];
export const page = ({ where }) => where.startsWith("sheet/");
for (const { path, css: raw } of sheets) {
  const css = raw.replace(/\/\*[\s\S]*?\*\//g, (comment) =>
    comment.replace(/[^\n]/g, " "),
  );
  const rules = [];
  let start = 0;
  for (let at = 0; at < css.length; at++) {
    const char = css[at];
    if (char !== "{" && char !== "}" && char !== ";") continue;
    const text = css.slice(start, at);
    if (char === "{") rules.push(text.trim());
    else {
      const colon = text.indexOf(":");
      if (colon > 0 && text.trim())
        declarations.push({
          where: `${path}:${css.slice(0, start + text.search(/\S/)).split("\n").length}`,
          rule: rules.join(" "),
          selector: rules.filter((rule) => !rule.startsWith("@")).join(" "),
          property: text.slice(0, colon).trim(),
          value: text.slice(colon + 1).trim(),
        });
      if (char === "}") rules.pop();
    }
    start = at + 1;
  }
}
