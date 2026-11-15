import { useEffect, useMemo, useState } from "preact/hooks";
import mermaid from "mermaid";
import { Button, CodeCopy, Modal, Notice } from "./ui.tsx";
import { useResource } from "./use-resource.ts";
import {
  dropOldest,
  maxDiagramCacheEntries,
  maxDiagramEdges,
  maxDiagramSourceChars,
  maxDiagramSvgChars,
} from "./limits.ts";

// Mermaid's global renderer is serialized. The SVG shows as an image, so
// diagram content never creates active elements in the app DOM; its size
// comes from the viewBox, so the <img> reserves its box before decode.
type RenderedDiagram = { svg: string; width: number; height: number };
const cache = new Map<string, RenderedDiagram>();
let queue = Promise.resolve();
let sequence = 0;
function diagramSize(svg: string): { width: number; height: number } {
  const match = svg.match(
    /viewBox="([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)"/,
  );
  if (!match) return { width: 0, height: 0 };
  const width = Number(match[3]) - Number(match[1]);
  const height = Number(match[4]) - Number(match[2]);
  if (
    !Number.isFinite(width) ||
    !Number.isFinite(height) ||
    width <= 0 ||
    height <= 0
  )
    return { width: 0, height: 0 };
  return { width: Math.round(width), height: Math.round(height) };
}
function diagram(source: string, dark: boolean): Promise<RenderedDiagram> {
  if (source.length > maxDiagramSourceChars)
    return Promise.reject(new Error("Diagram is too large"));
  const key = `${dark}:${source}`;
  if (cache.has(key)) return Promise.resolve(cache.get(key)!);
  const result = queue.then(async () => {
    if (cache.has(key)) return cache.get(key)!;
    const style = getComputedStyle(document.documentElement);
    const token = (name: string) => style.getPropertyValue(`--${name}`).trim();
    mermaid.initialize({
      startOnLoad: false,
      securityLevel: "strict",
      htmlLabels: false,
      maxTextSize: maxDiagramSourceChars,
      maxEdges: maxDiagramEdges,
      suppressErrorRendering: true,
      theme: "base",
      themeVariables: {
        darkMode: dark,
        background: token("bg"),
        primaryColor: token("raised"),
        primaryTextColor: token("text"),
        primaryBorderColor: token("muted"),
        lineColor: token("muted"),
        secondaryColor: token("hover"),
        tertiaryColor: token("surface"),
        fontFamily: "monospace",
      },
    });
    const { svg } = await mermaid.render(`diagram-${++sequence}`, source);
    const rendered = { svg, ...diagramSize(svg) };
    if (svg.length < maxDiagramSvgChars) {
      cache.set(key, rendered);
      dropOldest(cache, maxDiagramCacheEntries);
    }
    return rendered;
  });
  queue = result.then(
    () => {},
    () => {},
  );
  return result;
}
export default function Diagram({ source }: { source: string }) {
  const [dark, setDark] = useState(
    document.documentElement.dataset.theme === "dark",
  );
  const rendered = useResource(() => diagram(source, dark), [source, dark]);
  const url = useMemo(
    () =>
      rendered.value && !rendered.error
        ? URL.createObjectURL(
            new Blob([rendered.value.svg], { type: "image/svg+xml" }),
          )
        : "",
    [rendered.value, rendered.error],
  );
  useEffect(() => () => URL.revokeObjectURL(url), [url]);
  const size = rendered.value || { width: 0, height: 0 };
  const error =
    rendered.error && "Diagram could not be rendered. Source is shown below.";
  const [expanded, setExpanded] = useState(false);
  useEffect(() => {
    const observer = new MutationObserver(() =>
      setDark(document.documentElement.dataset.theme === "dark"),
    );
    observer.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ["data-theme"],
    });
    return () => observer.disconnect();
  }, []);
  return (
    <div class="diagram">
      <div class="diagram-preview">
        {url ? (
          <Button
            variant="quiet"
            class="diagram-open"
            aria-label="Expand diagram"
            onClick={() => setExpanded(true)}
          >
            <img
              src={url}
              alt="Mermaid diagram"
              width={size.width || undefined}
              height={size.height || undefined}
            />
          </Button>
        ) : error ? (
          <Notice tone="error">{error}</Notice>
        ) : (
          <p class="muted small" role="status">
            Rendering diagram…
          </p>
        )}
      </div>
      {/* Never open by default: closing it as the image lands would shrink the
          box it just grew. */}
      <details>
        <summary class="small">Show source</summary>
        <div class="code-block">
          <pre>
            <code>{source}</code>
          </pre>
          <span class="code-copy">
            <CodeCopy text={source} />
          </span>
        </div>
      </details>
      {expanded && (
        <Modal
          title="Diagram"
          close={() => setExpanded(false)}
          size="xl"
          className="diagram-expanded"
        >
          <img src={url} alt="Mermaid diagram" />
        </Modal>
      )}
    </div>
  );
}
