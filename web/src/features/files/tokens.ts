// A design system's colours read back from its tokens.css, per theme, for
// the Theme section under Tweaks. The file the pages load is the only
// source; nothing is compiled from it.

export interface Tokens {
  // In file order: the first block's (its [data-theme], else "default"),
  // then each further [data-theme] block's.
  themes: string[];
  colors: { name: string; value: string; values: Record<string, string> }[];
}

const COLOR = /^(#[0-9a-f]{3,8}\b|(rgb|hsl|oklch|oklab|lab|lch|color)a?\()/i;

export function readTokens(css: string): Tokens {
  const tokens: Tokens = { themes: [], colors: [] };
  const colors = new Map<string, Tokens["colors"][number]>();
  const color = (value: string): boolean => {
    const alias = /^var\(--([\w-]+)\)$/.exec(value);
    return alias ? colors.has(alias[1]) : COLOR.test(value);
  };
  // Overrides for a medium (touch, more contrast) are not tokens of their own.
  const plain = css.replace(/@media[^{]*\{(?:[^{}]*\{[^{}]*\})*[^{}]*\}/g, "");
  for (const [, head, body] of plain.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    const selector = head.replace(/\/\*[\s\S]*?\*\//g, "").trim();
    // A theme block, as the artifact skill writes them; anything fancier
    // (:not(), media queries) is not one.
    if (selector.includes(":not(")) continue;
    const theme = /\[data-theme="?([\w-]+)"?\]/.exec(selector)?.[1];
    const id = theme ?? (selector.includes(":root") ? "default" : undefined);
    if (!id) continue;
    for (const [, name, raw] of body.matchAll(/--([\w-]+)\s*:\s*([^;]+);/g)) {
      const value = raw.trim();
      const known = colors.get(name);
      if (!known && !color(value)) continue;
      // A theme is a block that sets colours.
      if (!tokens.themes.includes(id)) tokens.themes.push(id);
      if (known) known.values[id] = value;
      else {
        const entry = { name, value, values: { [id]: value } };
        colors.set(name, entry);
        tokens.colors.push(entry);
      }
    }
  }
  // An unnamed :root that only repeats a named theme is that theme.
  const same = (a: string, b: string) =>
    tokens.colors.every(
      (token) =>
        (token.values[a] ?? token.value) === (token.values[b] ?? token.value),
    );
  if (tokens.themes.some((id) => id !== "default" && same("default", id)))
    tokens.themes = tokens.themes.filter((id) => id !== "default");
  return tokens;
}
