import "./markdown.css";
import { Component, type ComponentType } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { MarkdownBlock } from "./markdown.ts";
import { CodeCopy, LoadError } from "./ui.tsx";

let renderer: Promise<typeof import("./markdown.ts")> | undefined;

function closedFence(source: string) {
  const lines = source.split("\n");
  const opening = lines[0].match(/^\s*(`{3,}|~{3,})/);
  return (
    !opening ||
    lines.slice(1).some((line) => {
      const match = line.match(/^\s*(`{3,}|~{3,})\s*$/);
      return (
        !!match &&
        match[1][0] === opening[1][0] &&
        match[1].length >= opening[1].length
      );
    })
  );
}

function escapeHTML(value: string) {
  return value.replace(
    /[&<>]/g,
    (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[character]!,
  );
}

function CodeBlock({ source, html }: { source: string; html: string }) {
  return (
    <div class="code-block">
      <div dangerouslySetInnerHTML={{ __html: html }} />
      <span class="code-copy">
        <CodeCopy text={source} />
      </span>
    </div>
  );
}

function DiagramLeaf({ source }: { source: string }) {
  const [Diagram, setDiagram] = useState<ComponentType<{
    source: string;
  }> | null>(null);
  useEffect(() => {
    let active = true;
    import("./diagram.tsx").then(({ default: component }) => {
      if (active) setDiagram(() => component);
    });
    return () => {
      active = false;
    };
  }, []);
  return Diagram ? (
    <Diagram source={source} />
  ) : (
    <CodeBlock
      source={source}
      html={`<pre><code>${escapeHTML(source)}</code></pre>`}
    />
  );
}

interface RenderedBlockProps {
  block: MarkdownBlock;
  streaming?: boolean;
}

class RenderedBlock extends Component<RenderedBlockProps> {
  shouldComponentUpdate(after: RenderedBlockProps) {
    const before = this.props;
    return !(
      (!before.block.code?.mermaid || before.streaming === after.streaming) &&
      before.block.html === after.block.html &&
      before.block.source === after.block.source &&
      before.block.code?.mermaid === after.block.code?.mermaid
    );
  }

  render({ block, streaming }: RenderedBlockProps) {
    if (block.code) {
      if (block.code.mermaid && (!streaming || closedFence(block.source)))
        return <DiagramLeaf source={block.code.text} />;
      return <CodeBlock source={block.code.text} html={block.html} />;
    }
    return <div dangerouslySetInnerHTML={{ __html: block.html }} />;
  }
}

export default function Markdown({
  text,
  streaming,
}: {
  text: string;
  streaming?: boolean;
}) {
  const [blocks, setBlocks] = useState<MarkdownBlock[]>([]);
  const [error, setError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const pending = useRef({ active: true, text, running: false });
  pending.current.text = text;
  useEffect(() => {
    // While streaming, render cheap plain text synchronously. Full markdown
    // (highlight, math, mermaid) runs once when the block completes, avoiding
    // per-token parses and partial-fence flicker.
    if (streaming) return;
    const state = pending.current;
    if (state.running) return;
    state.running = true;
    const paint = async () => {
      try {
        const value = state.text;
        renderer ??= import("./markdown.ts");
        const output = await (await renderer).renderMarkdownBlocks(value);
        if (!state.active) return;
        setBlocks(output);
        setError(null);
        if (value !== state.text) requestAnimationFrame(paint);
        else state.running = false;
      } catch (failure) {
        if (!state.active) return;
        renderer = undefined;
        state.running = false;
        setError(failure);
      }
    };
    requestAnimationFrame(paint);
  }, [text, streaming, retry]);
  useEffect(
    () => () => {
      pending.current.active = false;
    },
    [],
  );
  if (streaming) return <div class="plain">{text}</div>;
  return blocks.length || error ? (
    <div class="markdown">
      {blocks.map((block) => (
        <RenderedBlock key={block.key} block={block} streaming={streaming} />
      ))}
      {error && (
        <div class="renderer-fallback">
          <LoadError error={error} retry={() => setRetry(retry + 1)} />
          <pre>{text}</pre>
        </div>
      )}
    </div>
  ) : (
    <div class="plain">{text}</div>
  );
}
