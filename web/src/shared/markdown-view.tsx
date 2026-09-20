import "./markdown.css";
import { Component, type ComponentType } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { MarkdownBlock } from "./markdown.ts";
import { CodeCopy, LoadError } from "./ui.tsx";

let renderer: Promise<typeof import("./markdown.ts")> | undefined;
const prepared = new Map<string, MarkdownBlock[]>();
let preparedChars = 0;
const MAX_PREPARED_CHARS = 4_000_000;

// Completed pages are parsed before they become visible. Keep the same
// bounded result for the message component's first render; streaming text
// never enters this cache.
export async function prepareMarkdown(text: string): Promise<MarkdownBlock[]> {
  const cached = prepared.get(text);
  if (cached) return cached;
  renderer ??= import("./markdown.ts");
  const blocks = await (await renderer).renderMarkdownBlocks(text);
  if (text.length <= MAX_PREPARED_CHARS) {
    prepared.set(text, blocks);
    preparedChars += text.length;
    while (preparedChars > MAX_PREPARED_CHARS) {
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

const STREAM_MAX_CHARS = 16000;
const STREAM_MIN_INTERVAL_MS = 120;
const STREAM_SCAN_LINES = 40;

function streamingHead(source: string): string {
  const lines = source.split("\n");
  const start = Math.max(1, lines.length - STREAM_SCAN_LINES);
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
  const [streamBlocks, setStreamBlocks] = useState<MarkdownBlock[]>([]);
  const [streamTail, setStreamTail] = useState(text);
  const pending = useRef({ active: true, text, running: false });
  pending.current.text = text;
  const stream = useRef({
    text: "",
    tail: "",
    rendered: 0,
    timer: 0,
    lastRun: 0,
  });
  // Progressive streaming render: at most one pass per
  // STREAM_MIN_INTERVAL_MS over the closed prefix, with the unfinished
  // tail staying plain. Stable block keys keep completed DOM subtrees
  // untouched, so growth never flips rendered structure; the completion
  // pass below then only fills the tail instead of swapping plain text
  // for markdown in one height wave.
  useEffect(() => {
    if (!streaming || !progressive) {
      setStreamBlocks([]);
      return;
    }
    prefetchMarkdown(text);
    const state = stream.current;
    state.text = text;
    if (text.length > STREAM_MAX_CHARS) {
      // Long turns stay plain until completion: a full-document parse per
      // keystroke would jank the turn it decorates.
      setStreamBlocks([]);
      setStreamTail(text);
      state.rendered = 0;
      return;
    }
    if (state.timer) return; // trailing pass already scheduled
    const wait = Math.max(
      0,
      STREAM_MIN_INTERVAL_MS - (Date.now() - state.lastRun),
    );
    state.timer = window.setTimeout(() => {
      state.timer = 0;
      state.lastRun = Date.now();
      const value = state.text;
      const head = streamingHead(value);
      if (head.length < stream.current.rendered) {
        // A later opener unbalanced the fences: keep the frozen frame and
        // let the plain tail cover everything past it. Rendered structure
        // never unmounts mid-stream.
        const frozenTail = value.slice(stream.current.rendered);
        if (stream.current.tail !== frozenTail) {
          stream.current.tail = frozenTail;
          setStreamTail(frozenTail);
        }
        return;
      }
      stream.current.rendered = head.length;
      const tail = value.slice(head.length);
      if (stream.current.tail !== tail) {
        stream.current.tail = tail;
        setStreamTail(tail);
      }
      if (!head.trim()) {
        setStreamBlocks([]);
        return;
      }
      const pendingRenderer = (renderer ??= import("./markdown.ts"));
      pendingRenderer
        .then((module) => module.renderMarkdownBlocks(head))
        .then((output) => {
          // A newer pass started while this one parsed; it will paint.
          if (stream.current.text !== value) return;
          setStreamBlocks(output);
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
    },
    [],
  );
  if (streaming) {
    const startLine =
      text.slice(0, stream.current.rendered).split("\n").length - 1;
    // NOTE: deliberately not `.markdown`. Completion-timing contracts
    // (asserted in ui.spec.js) observe `.message.response > .markdown`
    // appearing at block completion with stable nodes; publishing the
    // progressive tree under that selector would hand out nodes the
    // completion swap then replaces (selection loss, removals). The
    // stream tree swaps for the final render exactly like the old
    // plain-text path did.
    return (
      <div class="markdown-stream">
        {streamBlocks.map((block) => (
          <div key={block.key} data-anchor-id={block.key.split(":")[0]}>
            <RenderedBlock block={block} streaming />
          </div>
        ))}
        {plainSegments(streamTail, startLine).map((segment) => (
          <div
            key={`plain-${segment.line}`}
            data-anchor-id={String(segment.line)}
            class="plain"
          >
            {segment.text}
          </div>
        ))}
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
