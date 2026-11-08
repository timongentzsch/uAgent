// A design system's tokens.css read back as tokens: colours per theme, the
// spacing, radius, shadow and font scales, and the text styles, each with
// the comment after it as its usage. The file the pages load is the only
// source; nothing is compiled from it.

export interface Token {
  name: string;
  value: string;
  usage: string;
}
export interface Tokens {
  // "light" first: the :root block, then each [data-theme] block.
  themes: string[];
  colors: (Token & { values: Record<string, string> })[];
  scales: Record<"space" | "radius" | "shadow" | "font", Token[]>;
  text: Token[];
}

const COLOR = /^(#[0-9a-f]{3,8}\b|(rgb|hsl|oklch|oklab|lab|lch|color)a?\()/i;
const SCALES = ["space", "radius", "shadow", "font"] as const;

export function readTokens(css: string): Tokens {
  const tokens: Tokens = {
    themes: [],
    colors: [],
    scales: { space: [], radius: [], shadow: [], font: [] },
    text: [],
  };
  const colors = new Map<string, Tokens["colors"][number]>();
  const color = (value: string): boolean => {
    const alias = /^var\(--([\w-]+)\)$/.exec(value);
    return alias ? colors.has(alias[1]) : COLOR.test(value);
  };
  const blocks = /([^{}]+)\{([^{}]*)\}/g;
  for (const [, head, body] of css.matchAll(blocks)) {
    const selector = head.replace(/\/\*[\s\S]*?\*\//g, "").trim();
    const declarations = [
      ...body.matchAll(
        /((?:--)?[\w-]+)\s*:\s*([^;]+);[ \t]*(?:\/\*\s*([\s\S]*?)\s*\*\/)?/g,
      ),
    ];
    const style = /^\.text-([\w-]+)$/.exec(selector);
    if (style) {
      tokens.text.push({
        name: style[1],
        value: declarations
          .map(([, property, value]) => `${property}: ${value.trim()}`)
          .join("; "),
        usage: declarations.find(([, , , usage]) => usage)?.[3] ?? "",
      });
      continue;
    }
    // A theme block, as the design skill writes them; anything fancier
    // (:not(), media queries) is not one.
    if (selector.includes(":not(")) continue;
    const theme = /\[data-theme="?([\w-]+)"?\]/.exec(selector)?.[1];
    const id = theme ?? (selector.includes(":root") ? "light" : undefined);
    if (!id) continue;
    if (!tokens.themes.includes(id)) tokens.themes.push(id);
    for (const [, property, raw, usage = ""] of declarations) {
      if (!property.startsWith("--")) continue;
      const name = property.slice(2);
      const value = raw.trim();
      const scale = SCALES.find((prefix) => name.startsWith(`${prefix}-`));
      const known = colors.get(name);
      if (known) known.values[id] = value;
      else if (scale) tokens.scales[scale].push({ name, value, usage });
      else if (color(value)) {
        const entry = { name, value, usage, values: { [id]: value } };
        colors.set(name, entry);
        tokens.colors.push(entry);
      }
    }
  }
  return tokens;
}
