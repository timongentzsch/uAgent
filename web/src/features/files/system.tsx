import { useMemo } from "preact/hooks";
import { api, routes } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import { MarkdownFile } from "../../shared/markdown-view.tsx";
import { LoadError, Spinner } from "../../shared/ui.tsx";
import Frame from "./frame.tsx";
import { readTokens } from "./tokens.ts";

// A design system's folder as one page: its README, the tokens its
// tokens.css declares, and its components' live previews, grouped as each
// preview's first line says (<!-- @dsCard group="Actions" height=88 -->).

interface Listing {
  entries: { name: string; dir: boolean }[];
}
const read = (id: string, path: string) =>
  api<{ text: string }>(routes.files(id, "read", path)).then(
    (value) => value.text,
    () => "",
  );

export default function System({
  id,
  dir,
  turn,
  page,
  url,
}: {
  id: string;
  dir: string;
  // Re-read when a turn starts or ends, as the rest of the pane is.
  turn: unknown;
  page: (path: string) => string;
  url: (path: string) => string;
}) {
  const at = (name: string) => (dir ? `${dir}/${name}` : name);
  const system = useResource(async () => {
    const [css, readme, listing] = await Promise.all([
      read(id, at("tokens.css")),
      read(id, at("README.md")),
      api<Listing>(routes.files(id, "list", at("components"))).catch(() => ({
        entries: [],
      })),
    ]);
    const names = listing.entries
      .filter((entry) => entry.dir)
      .map((entry) => entry.name);
    const previews = await Promise.all(
      names.map(async (name) => {
        const path = at(`components/${name}/preview.html`);
        const head = (await read(id, path)).split("\n", 1)[0];
        const group = /group="([^"]*)"/.exec(head)?.[1] ?? "";
        const height = Number(/height=(\d+)/.exec(head)?.[1]) || 120;
        return {
          name,
          path,
          group,
          height: Math.min(4000, Math.max(40, height)),
        };
      }),
    );
    return { css, readme, previews };
  }, [id, dir, turn]);
  const tokens = useMemo(
    () => readTokens(system.value?.css ?? ""),
    [system.value?.css],
  );
  if (system.error)
    return <LoadError error={system.error} retry={system.retry} />;
  if (!system.value) return <Spinner label="Reading the design system…" />;
  // A colour as a swatch, its aliases followed within the same theme.
  const swatch = (value: string, theme: string): string => {
    const alias = /^var\(--([\w-]+)\)$/.exec(value)?.[1];
    const token = alias && tokens.colors.find((color) => color.name === alias);
    return token ? swatch(token.values[theme] ?? token.value, theme) : value;
  };
  const groups = [
    ...new Set(system.value.previews.map((preview) => preview.group)),
  ];
  return (
    <div class="system">
      {system.value.readme && (
        <MarkdownFile text={system.value.readme} base={dir} url={url} />
      )}
      {tokens.colors.length > 0 && (
        <section aria-label="Colours">
          <h3 class="system-title">Colours</h3>
          <table class="system-colors">
            <thead>
              <tr>
                {tokens.themes.map((theme) => (
                  <th key={theme} class="small">
                    {theme}
                  </th>
                ))}
                <th class="small">Token</th>
              </tr>
            </thead>
            <tbody>
              {tokens.colors.map((color) => (
                <tr key={color.name}>
                  {tokens.themes.map((theme) => {
                    const value = color.values[theme] ?? color.value;
                    return (
                      <td key={theme}>
                        <span
                          class="system-swatch"
                          title={value}
                          style={{ background: swatch(value, theme) }}
                        />
                      </td>
                    );
                  })}
                  <td>
                    <b>{color.name}</b>{" "}
                    <span class="muted small">{color.usage}</span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      )}
      {tokens.text.length > 0 && (
        <section aria-label="Type">
          <h3 class="system-title">Type</h3>
          {tokens.text.map((style) => (
            <p key={style.name} class="system-row">
              <span style={style.value}>{style.name}</span>
              <span class="muted small">{style.usage || style.value}</span>
            </p>
          ))}
        </section>
      )}
      {(["space", "radius", "shadow", "font"] as const).map(
        (scale) =>
          tokens.scales[scale].length > 0 && (
            <section key={scale} aria-label={scale}>
              <h3 class="system-title">{scale}</h3>
              {tokens.scales[scale].map((token) => (
                <p key={token.name} class="system-row">
                  <b>{token.name}</b>
                  <code>{token.value}</code>
                  <span class="muted small">{token.usage}</span>
                </p>
              ))}
            </section>
          ),
      )}
      {groups.map((group) => (
        <section key={group} aria-label={group || "Components"}>
          <h3 class="system-title">{group || "Components"}</h3>
          {system
            .value!.previews.filter((preview) => preview.group === group)
            .map((preview) => (
              <figure key={preview.name} class="system-preview">
                <figcaption class="small muted">{preview.name}</figcaption>
                <div
                  class="system-frame"
                  style={{ height: `${preview.height}px` }}
                >
                  <Frame src={page(preview.path)} title={preview.name} />
                </div>
              </figure>
            ))}
        </section>
      ))}
    </div>
  );
}
