import { useEffect, useState } from "preact/hooks";
import mermaid from "mermaid";
import { CodeCopy, Modal } from "./ui.tsx";

// Serialize Mermaid's global renderer and bound retained SVGs. SVG is displayed
// as an image, so diagram content never creates active elements in the app DOM.
const cache = new Map<string, string>();
let queue = Promise.resolve();
let sequence = 0;
function diagram(source: string, dark: boolean): Promise<string> {
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
    if (svg.length < 512000) {
      cache.set(key, svg);
      while (cache.size > 24) cache.delete(cache.keys().next().value!);
    }
    return svg;
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
            new Blob([value], { type: "image/svg+xml" }),
          );
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
          <img src={url} alt="Mermaid diagram" />
        </button>
      )}
      {error && (
        <p class="muted small" role="status">
          {error}
        </p>
      )}
      <details open={!url}>
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
