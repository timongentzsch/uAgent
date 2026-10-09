import "./markdown.css";
import {
  maxPreparedMarkdownChars,
  wholeStreamingMarkdownChars,
} from "./limits.ts";
import { closedFence, streamingHead } from "./markdown-split.ts";
import { Component, type ComponentType } from "preact";
import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import type { MarkdownBlock } from "./markdown.ts";
import { CodeCopy, LoadError, DataText, usePlaceholder } from "./ui.tsx";

type Renderer = typeof import("./markdown.ts");
let renderer: Promise<Renderer> | undefined;
// Once loaded, streaming renders synchronously in the same frame as the text.
let loaded: Renderer | undefined;
const loadRenderer = () =>
  (renderer ??= import("./markdown.ts").then((module) => (loaded = module)));
const prepared = new Map<string, MarkdownBlock[]>();
let preparedChars = 0;
// What an entry really holds: its source plus every block's rendered HTML
// and source slice, which together run to several times the input.
const retained = (text: string, blocks: MarkdownBlock[]) =>
  blocks.reduce(
    (sum, block) => sum + block.html.length + block.source.length,
    text.length,
  );

// Completed pages are parsed before they become visible. Keep the same
// bounded result for the message component's first render; streaming text
// never enters this cache.
export async function prepareMarkdown(text: string): Promise<MarkdownBlock[]> {
  const cached = prepared.get(text);
  if (cached) return cached;
  const blocks = await (await loadRenderer()).renderMarkdownBlocks(text);
  // Concurrent preparation may already have inserted this same source.
  if (text.length <= maxPreparedMarkdownChars && !prepared.has(text)) {
    preparedChars += retained(text, blocks);
    prepared.set(text, blocks);
    for (const [oldest, entry] of prepared) {
      if (preparedChars <= maxPreparedMarkdownChars) break;
      preparedChars -= retained(oldest, entry);
      prepared.delete(oldest);
    }
  }
  return blocks;
}

// Warm the renderer chunk outside the critical path. Called while a turn
// streams (and once at boot), so the plain-to-markdown switch at
// completion only pays the parse, never the chunk download.
export function prefetchMarkdown(text = "") {
  void loadRenderer();
  if (/\$|\\[([]/.test(text)) void import("./math.ts");
  if (/```|~~~/.test(text)) void import("./highlight.ts");
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

function CodeBlock({
  source,
  html,
  incomplete = false,
}: {
  source: string;
  html: string;
  // Still streaming: shown as it arrives, copyable once its fence closes.
  incomplete?: boolean;
}) {
  return (
    <div class="code-block" data-incomplete={incomplete || undefined}>
      <div dangerouslySetInnerHTML={{ __html: html }} />
      {!incomplete && (
        <span class="code-copy">
          <CodeCopy text={source} />
        </span>
      )}
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
    // Code blocks follow `streaming`: an open fence gains Copy on completion.
    return !(
      (!before.block.code || before.streaming === after.streaming) &&
      before.block.html === after.block.html &&
      before.block.source === after.block.source &&
      before.block.code?.mermaid === after.block.code?.mermaid
    );
  }

  render({ block, streaming }: RenderedBlockProps) {
    if (block.code) {
      const open = !!streaming && !closedFence(block.source);
      if (block.code.mermaid && !open)
        return <DiagramLeaf source={block.code.text} />;
      return (
        <CodeBlock
          source={block.code.text}
          html={block.html}
          incomplete={open}
        />
      );
    }
    return <div dangerouslySetInnerHTML={{ __html: block.html }} />;
  }
}

type MarkdownProps = {
  text: string;
  streaming?: boolean;
  // False keeps the plain-text-while-streaming path: hidden surfaces
  // (e.g. reasoning inside a never-opened disclosure) must not pay
  // renderer work for content the user may never see.
  progressive?: boolean;
};

// Inside a Placeholder the text is sample prose: its paragraphs as bars in
// the rendered container, with no parser work.
export default function Markdown(props: MarkdownProps) {
  return usePlaceholder() ? (
    <div class="markdown">
      {props.text.split(/\n{2,}/).map((paragraph, index) => (
        <p key={index}>
          <DataText>{paragraph}</DataText>
        </p>
      ))}
    </div>
  ) : (
    <RenderedMarkdown {...props} />
  );
}

function RenderedMarkdown({
  text,
  streaming,
  progressive = true,
}: MarkdownProps) {
  const [rendered, setRendered] = useState<{
    text: string;
    blocks: MarkdownBlock[];
  }>(() => ({ text, blocks: prepared.get(text) || [] }));
  const [error, setError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const [, setReady] = useState(!!loaded);
  const pending = useRef({ active: true, text, running: false });
  pending.current.text = text;
  // The finished part of a streaming reply, parsed once per boundary.
  const head = useRef<{
    text: string;
    blocks: MarkdownBlock[];
    env: Record<string, unknown>;
  }>({ text: "", blocks: [], env: {} });
  // The last streamed frame stays on screen until the final parse lands.
  const last = useRef<{ text: string; blocks: MarkdownBlock[] }>({
    text: "",
    blocks: [],
  });
  const live = !!streaming && progressive;
  useEffect(() => {
    if (!live || loaded) return;
    let active = true;
    void loadRenderer().then(() => active && setReady(true));
    return () => {
      active = false;
    };
  }, [live]);
  const streamed = useMemo(() => {
    if (!live || !loaded) return null;
    const boundary = streamingHead(text);
    const cached = head.current;
    if (boundary !== cached.text) {
      // Long replies append new blocks instead of re-parsing everything.
      const append =
        !!cached.text &&
        boundary.startsWith(cached.text) &&
        boundary.length > wholeStreamingMarkdownChars;
      // Appended parts share the head's references (an earlier `[ref]:`).
      const env = append ? cached.env : {};
      head.current = {
        text: boundary,
        env,
        blocks: append
          ? [
              ...cached.blocks,
              ...loaded.markdownBlocks(
                boundary.slice(cached.text.length + 1),
                cached.text.split("\n").length,
                env,
              ),
            ]
          : boundary
            ? loaded.markdownBlocks(boundary, 0, env)
            : [],
      };
    }
    const tail = text.slice(boundary.length);
    const offset = boundary ? boundary.split("\n").length - 1 : 0;
    const blocks = [
      ...head.current.blocks,
      ...loaded.markdownBlocks(loaded.healTail(tail), offset, {
        // Its own copy: a half-typed `[ref]:` in the tail must not stick.
        references: {
          ...((head.current.env.references as object | undefined) || {}),
        },
      }),
    ];
    last.current = { text, blocks };
    return blocks;
  }, [text, live, loaded]);
  useEffect(() => {
    // Full render runs when the block completes (and on retry): the
    // streaming frames already loaded the renderer chunks, so completion
    // only pays the parse.
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
  // Hidden streaming text (unopened reasoning) stays one plain node.
  if (streaming && !progressive) return <div class="plain">{text}</div>;
  const blocks =
    streamed ||
    prepared.get(text) ||
    (rendered.text === text && rendered.blocks.length
      ? rendered.blocks
      : null) ||
    (last.current.text === text ? last.current.blocks : null);
  // One container from the first token to the final render, so completion
  // swaps no element; `data-streaming` marks a reply still being written.
  // Blocks carry source-line anchors the history controller keeps in view.
  return (
    <div class="markdown" data-streaming={streaming || undefined}>
      {blocks
        ? blocks.map((block) => (
            <div key={block.key} data-anchor-id={block.key.split(":")[0]}>
              <RenderedBlock block={block} streaming={streaming} />
            </div>
          ))
        : !error &&
          plainSegments(text, 0).map((segment) => (
            <div
              key={`plain-${segment.line}`}
              data-anchor-id={String(segment.line)}
              class="plain"
            >
              {segment.text}
            </div>
          ))}
      {error && (
        <div>
          <LoadError error={error} retry={() => setRetry(retry + 1)} />
          <pre>{text}</pre>
        </div>
      )}
    </div>
  );
}
