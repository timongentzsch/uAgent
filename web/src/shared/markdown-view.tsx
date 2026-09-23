import "./markdown.css";
import {
  maxPreparedMarkdownChars,
  maxProgressiveMarkdownChars,
  progressiveMarkdownIntervalMs,
  progressiveMarkdownScanLines,
} from "./limits.ts";
import { Component, type ComponentType } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { MarkdownBlock } from "./markdown.ts";
import { CodeCopy, LoadError } from "./ui.tsx";

let renderer: Promise<typeof import("./markdown.ts")> | undefined;
const prepared = new Map<string, MarkdownBlock[]>();
let preparedChars = 0;

// Completed pages are parsed before they become visible. Keep the same
// bounded result for the message component's first render; streaming text
// never enters this cache.
export async function prepareMarkdown(text: string): Promise<MarkdownBlock[]> {
  const cached = prepared.get(text);
  if (cached) return cached;
  renderer ??= import("./markdown.ts");
  const blocks = await (await renderer).renderMarkdownBlocks(text);
  if (text.length <= maxPreparedMarkdownChars) {
    // Concurrent preparation may already have inserted this same source.
    if (!prepared.has(text)) preparedChars += text.length;
    prepared.set(text, blocks);
    while (preparedChars > maxPreparedMarkdownChars) {
      const oldest = prepared.keys().next().value;
      if (oldest === undefined) break;
      preparedChars -= oldest.length;
      prepared.delete(oldest);
    }
  }
  return blocks;
}

// Warm the renderer chunk outside the critical path. Called while a turn
// streams (and once at boot), so the plain-to-markdown switch at
// completion only pays the parse, never the chunk download.
export function prefetchMarkdown(text = "") {
  renderer ??= import("./markdown.ts");
  if (/\$|\\[([]/.test(text)) void import("./math.ts");
  if (/```|~~~/.test(text)) void import("./highlight.ts");
}

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

// Progressive streaming split: text through the last blank line whose
// head has balanced fences, so a streaming pass never renders an open
// code block. Inline delimiters left unbalanced render literally until
// the next boundary — progressive enhancement, never a structural
// flip (block keys stay stable, see renderMarkdownBlocks).
function balancedFences(head: string): boolean {
  const fences = head.match(/^[ \t]*(```+|~~~+).*$/gm) || [];
  let ticks = 0;
  let tildes = 0;
  for (const fence of fences) {
    if (fence.trimStart().startsWith("`")) ticks++;
    else tildes++;
  }
  return ticks % 2 === 0 && tildes % 2 === 0;
}

function streamingHead(source: string): string {
  const lines = source.split("\n");
  const start = Math.max(1, lines.length - progressiveMarkdownScanLines);
  for (let i = lines.length - 1; i >= start; i--) {
    if (lines[i].trim() !== "") continue;
    const head = lines.slice(0, i).join("\n");
    if (!head.trim() || !balancedFences(head)) continue;
    return head;
  }
  return "";
}

function plainSegments(tail: string, startLine: number) {
  const parts = tail.split(/(\n[ \t]*\n)/);
  const segments: { line: number; text: string }[] = [];
  let line = startLine;
  for (let index = 0; index < parts.length; index += 2) {
    const value = parts[index] + (parts[index + 1] || "");
    if (value) segments.push({ line, text: value });
    line += (value.match(/\n/g) || []).length;
  }
  return segments;
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
    <div class="diagram">
      <div class="diagram-preview" role="status">
        Rendering diagram…
      </div>
      <details>
        <summary>Show source</summary>
        <CodeBlock
          source={source}
          html={`<pre><code>${escapeHTML(source)}</code></pre>`}
        />
      </details>
    </div>
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
  progressive = true,
}: {
  text: string;
  streaming?: boolean;
  // False keeps the plain-text-while-streaming path: hidden surfaces
  // (e.g. reasoning inside a never-opened disclosure) must not pay
  // renderer work for content the user may never see.
  progressive?: boolean;
}) {
  const [rendered, setRendered] = useState<{
    text: string;
    blocks: MarkdownBlock[];
  }>(() => ({ text, blocks: prepared.get(text) || [] }));
  const [error, setError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const [streamed, setStreamed] = useState<{
    head: string;
    blocks: MarkdownBlock[];
  }>({ head: "", blocks: [] });
  const pending = useRef({ active: true, text, running: false });
  pending.current.text = text;
  const stream = useRef({
    text,
    active: true,
    timer: 0,
    lastRun: 0,
  });
  stream.current.text = text;
  // Parse closed prefixes at the configured interval. The plain remainder
  // keeps incoming text visible while parsing; block keys preserve already
  // formatted subtrees during the stream.
  useEffect(() => {
    if (
      !streaming ||
      !progressive ||
      text.length > maxProgressiveMarkdownChars
    ) {
      setStreamed((previous) =>
        previous.head ? { head: "", blocks: [] } : previous,
      );
      return;
    }
    prefetchMarkdown(text);
    const state = stream.current;
    if (state.timer) return; // trailing pass already scheduled
    const wait = Math.max(
      0,
      progressiveMarkdownIntervalMs - (Date.now() - state.lastRun),
    );
    state.timer = window.setTimeout(() => {
      state.timer = 0;
      state.lastRun = Date.now();
      const head = streamingHead(state.text);
      if (!head.trim()) return;
      const pendingRenderer = (renderer ??= import("./markdown.ts"));
      pendingRenderer
        .then((module) => module.renderMarkdownBlocks(head))
        .then((output) => {
          if (
            !state.active ||
            state.text.length > maxProgressiveMarkdownChars ||
            !state.text.startsWith(head)
          )
            return;
          // Commit the prefix and its blocks together. Until parsing finishes,
          // the plain tail still contains every byte after the painted prefix.
          setStreamed((previous) =>
            previous.head.length >= head.length &&
            state.text.startsWith(previous.head)
              ? previous
              : { head, blocks: output },
          );
        })
        .catch(() => {
          // Keep the previous progressive frame; the plain tail covers.
        });
    }, wait);
    return () => {
      clearTimeout(state.timer);
      state.timer = 0;
    };
  }, [text, streaming, progressive]);
  useEffect(() => {
    // Full render runs when the block completes (and on retry): the
    // progressive passes above already warmed the renderer chunks, so
    // completion only pays the parse.
    if (streaming) return;
    const state = pending.current;
    if (state.running) return;
    state.running = true;
    const paint = async () => {
      try {
        const value = state.text;
        const output = await prepareMarkdown(value);
        if (!state.active) return;
        setRendered({ text: value, blocks: output });
        setError(null);
        if (value !== state.text) paint();
        else state.running = false;
      } catch (failure) {
        if (!state.active) return;
        renderer = undefined;
        state.running = false;
        setError(failure);
      }
    };
    // No rAF hop: effects already run post-paint, and the awaits yield.
    paint();
  }, [text, streaming, retry]);
  useEffect(
    () => () => {
      pending.current.active = false;
      stream.current.active = false;
    },
    [],
  );
  if (streaming) {
    const frame = text.startsWith(streamed.head)
      ? streamed
      : { head: "", blocks: [] };
    const startLine = frame.head.split("\n").length - 1;
    // `.markdown` identifies a completed render. Until then the history
    // controller uses these source-line anchors to preserve reading position.
    return (
      <div class="markdown-stream">
        {frame.blocks.map((block) => (
          <div key={block.key} data-anchor-id={block.key.split(":")[0]}>
            <RenderedBlock block={block} streaming />
          </div>
        ))}
        {plainSegments(text.slice(frame.head.length), startLine).map(
          (segment) => (
            <div
              key={`plain-${segment.line}`}
              data-anchor-id={String(segment.line)}
              class="plain"
            >
              {segment.text}
            </div>
          ),
        )}
      </div>
    );
  }
  const blocks =
    prepared.get(text) || (rendered.text === text ? rendered.blocks : []);
  return blocks.length || error ? (
    <div class="markdown">
      {blocks.map((block) => (
        <div key={block.key} data-anchor-id={block.key.split(":")[0]}>
          <RenderedBlock block={block} streaming={streaming} />
        </div>
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
