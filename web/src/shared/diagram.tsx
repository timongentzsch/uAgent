import { useEffect, useState } from "preact/hooks";
import mermaid from "mermaid";
import { CodeCopy, Modal } from "./ui.tsx";

// Serialize Mermaid's global renderer and bound retained SVGs. SVG is displayed
// as an image, so diagram content never creates active elements in the app DOM.
// Dimensions come from the SVG viewBox so the <img> reserves its exact box
// before decode: an unsized image first lays out at intrinsic size, then
// collapses under max-width, and that grow/shrink pair nets to a phantom
// scroll gesture on engines without native scroll anchoring (WebKit).
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
  if (source.length > 20000)
    return Promise.reject(new Error("Diagram is too large"));
  const key = `${dark}:${source}`;
  if (cache.has(key)) return Promise.resolve(cache.get(key)!);
  const result = queue.then(async () => {
    if (cache.has(key)) return cache.get(key)!;
    mermaid.initialize({
      startOnLoad: false,
      securityLevel: "strict",
      htmlLabels: false,
      maxTextSize: 20000,
      maxEdges: 300,
      suppressErrorRendering: true,
      theme: "base",
      themeVariables: {
        darkMode: dark,
        background: dark ? "#000" : "#fff",
        primaryColor: dark ? "#151515" : "#f5f5f5",
        primaryTextColor: dark ? "#eee" : "#111",
        primaryBorderColor: dark ? "#aaa" : "#555",
        lineColor: dark ? "#aaa" : "#555",
        secondaryColor: dark ? "#222" : "#eee",
        tertiaryColor: dark ? "#111" : "#fafafa",
        fontFamily: "monospace",
      },
    });
    const { svg } = await mermaid.render(`diagram-${++sequence}`, source);
    const rendered = { svg, ...diagramSize(svg) };
    if (svg.length < 512000) {
      cache.set(key, rendered);
      while (cache.size > 24) cache.delete(cache.keys().next().value!);
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
  const [url, setURL] = useState("");
  const [size, setSize] = useState<{ width: number; height: number }>({
    width: 0,
    height: 0,
  });
  const [error, setError] = useState("");
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
  useEffect(() => {
    let active = true;
    let objectURL = "";
    setError("");
    diagram(source, dark)
      .then((value) => {
        if (active) {
          objectURL = URL.createObjectURL(
            new Blob([value.svg], { type: "image/svg+xml" }),
          );
          setSize({ width: value.width, height: value.height });
          setURL(objectURL);
        }
      })
      .catch(() => {
        if (active) {
          setURL("");
          setError("Diagram could not be rendered. Source is shown below.");
        }
      });
    return () => {
      active = false;
      if (objectURL) URL.revokeObjectURL(objectURL);
    };
  }, [source, dark]);
  return (
    <div class="diagram">
      {url && (
        <button
          class="quiet diagram-open"
          aria-label="Expand diagram"
          onClick={() => setExpanded(true)}
        >
          <img
            src={url}
            alt="Mermaid diagram"
            width={size.width || undefined}
            height={size.height || undefined}
          />
        </button>
      )}
      {error && (
        <p class="muted small" role="status">
          {error}
        </p>
      )}
      {/* Never flip open on resolve: closing the disclosure at the same
          commit the image lands shrinks the box it just grew, another
          phantom-gesture pair. Source stays one click away either way. */}
      <details>
        <summary>
          {url
            ? "Show source"
            : error
              ? "Diagram source"
              : "Rendering diagram…"}
        </summary>
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
          className="diagram-expanded"
        >
          <img src={url} alt="Mermaid diagram" />
        </Modal>
      )}
    </div>
  );
}
