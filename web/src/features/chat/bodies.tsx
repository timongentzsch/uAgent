import { Fragment } from "preact";
import type { ComponentChildren } from "preact";
import { useContext, useLayoutEffect, useRef, useState } from "preact/hooks";
import { Button, LoadError, Skeleton } from "../../shared/ui.tsx";
import DiffView from "../../shared/diff-view.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import { cleanText } from "../../shared/display.ts";
import type {
  ActivityTarget,
  BodyKind,
  FilePart,
  LinkPart,
  Node,
  SessionRef,
  ToolPart,
} from "../../shared/types.ts";
import { ImageTile } from "../../shared/attachments.tsx";
import { HostContext } from "../../state/host-context.ts";
import { bytes } from "../../shared/quantities.ts";
import { coarsePointer } from "../../shared/layout.ts";
import { useApi } from "../../state/api.ts";
import { useNodeReader } from "../../state/node-reader.ts";
import { useResource } from "../../shared/use-resource.ts";
import { MessageActions } from "./message-actions.ts";

// Every part comes from the node; nothing here knows which tool made it.

// A fence longer than any backtick run in the body, so code never ends early.
function fenced(text: string, language = "") {
  const longest = Math.max(
    2,
    ...(text.match(/`+/g) || []).map((run) => run.length),
  );
  const fence = "`".repeat(longest + 1);
  return `${fence}${language}\n${text}\n${fence}`;
}

function Input({ parts }: { parts: ToolPart[] }) {
  return (
    <>
      {parts.map((part, index) =>
        part.kind === "command" ? (
          <pre class="node-command" key={index}>
            {cleanText(part.text)}
          </pre>
        ) : part.kind === "code" ? (
          <figure class="node-code" key={index}>
            {part.label && <figcaption class="small">{part.label}</figcaption>}
            <Markdown text={fenced(cleanText(part.text), part.language)} />
          </figure>
        ) : part.kind === "fields" ? (
          <dl class="node-fields" key={index}>
            {part.rows.map(([label, value]) => (
              <Fragment key={label}>
                <dt>{label}</dt>
                <dd>{cleanText(value)}</dd>
              </Fragment>
            ))}
          </dl>
        ) : part.kind === "link" && part.to === "browser" ? (
          <BrowserLink key={index} label={part.label} />
        ) : null,
      )}
    </>
  );
}

function BrowserLink({ label }: { label: string }) {
  const host = useContext(HostContext);
  if (!host) return <p class="small muted">{label}</p>;
  return (
    <Button
      variant="quiet"
      class="node-action small"
      onClick={() => host.setModal({ type: "browser" })}
    >
      {label}
    </Button>
  );
}

// A body whose content is its parts (an image, a link, fields) reads as text.
type Body = (props: {
  node: Node;
  text: string;
  more: boolean;
  online: boolean;
  loadFull: () => void;
}) => ComponentChildren;
const Text: Body = ({ text }) => <pre class="node-text plain">{text}</pre>;
const bodies: Record<BodyKind, Body> = {
  text: Text,
  markdown: ({ text }) => (
    <div class="node-text">
      <Markdown text={text} />
    </div>
  ),
  tail: (props) => <Tail {...props} />,
  diff: ({ node, text }) => (
    <DiffView
      text={text}
      counts={node.result && [node.result.added ?? 0, node.result.removed ?? 0]}
    />
  ),
  image: Text,
  link: Text,
  fields: Text,
};

// Opens scrolled to its end; a long one shows only its end until loaded.
function Tail({ node, text, more, online, loadFull }: Parameters<Body>[0]) {
  const box = useRef<HTMLPreElement>(null);
  const shown = (more && node.body?.tail) || text;
  useLayoutEffect(() => {
    if (box.current) box.current.scrollTop = box.current.scrollHeight;
  }, [shown]);
  return (
    <pre
      class="node-text node-tail"
      ref={box}
      tabIndex={0}
      role="region"
      aria-label="Output"
    >
      {more && (
        <Button
          variant="quiet"
          class="node-action small"
          disabled={!online}
          onClick={loadFull}
        >
          Load earlier output
        </Button>
      )}
      {shown}
    </pre>
  );
}

const linkTarget = (node: Node, link: LinkPart): ActivityTarget => ({
  id: node.id,
  label: link.label,
  state: node.state,
  text: node.body?.text,
  command: node.command,
  activity_id: link.to === "activity" ? Number(link.id) : undefined,
  memory:
    link.to === "memory"
      ? { action: "", key: String(link.id), automatic: false }
      : undefined,
});

export function NodeBody({
  node,
  online,
  session,
}: {
  node: Node;
  online: boolean;
  session: SessionRef;
}) {
  const { routes, command } = useApi();
  const report = useContext(HostContext)?.report;
  const { read, inspect, activity, open } = useContext(MessageActions);
  const load = useNodeReader(session, read);
  const body = node.body;
  // A diff cut short loads whole as the row opens; other long bodies when
  // asked for.
  const [wantFull, setWantFull] = useState(body?.kind === "diff");
  const full = useResource(
    (signal) =>
      online && wantFull && body?.truncated
        ? load(node.id, false, signal).then((page) => page.text)
        : undefined,
    [online, wantFull, body?.truncated, node.id, session.id],
  );
  const loaded = wantFull && full.value !== undefined;
  const loading = online && wantFull && !!body?.truncated && !loaded;
  const text = cleanText((loaded && full.value) || body?.text || "");
  const Draw = body && bodies[body.kind];
  const assets = routes.assets(session.id);
  const parts = node.parts || [];
  const actions = [
    body?.truncated && !wantFull && body.kind !== "tail" && (
      <Button
        variant="quiet"
        class="node-action small"
        disabled={!online}
        onClick={() => setWantFull(true)}
      >
        Full output
      </Button>
    ),
    node.moved && (
      <Button
        variant="quiet"
        class="node-action small"
        disabled={!online}
        onClick={() =>
          void command("move", session, { cwd: node.moved?.from }).catch(report)
        }
      >
        Move back
      </Button>
    ),
    inspect && node.kind === "call" && (
      <Button
        variant="quiet"
        class="node-action small"
        onClick={() => inspect(node.id)}
      >
        Input and output
      </Button>
    ),
    ...parts.map(
      (part) =>
        part.kind === "link" &&
        part.to !== "browser" &&
        activity && (
          <Button
            variant="quiet"
            class="node-action small"
            disabled={!online}
            onClick={() =>
              part.to === "agent"
                ? open?.(String(part.id))
                : activity(linkTarget(node, part))
            }
          >
            {part.label}
          </Button>
        ),
    ),
  ].filter(Boolean);
  return (
    <div class="node-body">
      {node.input && <Input parts={node.input} />}
      {Draw && text && (
        <Draw
          node={node}
          text={text}
          more={!!body.truncated && !loaded}
          online={online}
          loadFull={() => setWantFull(true)}
        />
      )}
      {loading && <Skeleton label="Loading full output…" />}
      {full.error && <LoadError error={full.error} retry={full.retry} />}
      {parts.map(
        (part, index) =>
          part.kind === "file" && (
            <SharedFile key={index} file={part} href={`${assets}${part.id}`} />
          ),
      )}
      {actions.length > 0 && (
        <p class="node-actions">
          {actions.map((action, index) => (
            <Fragment key={index}>{action}</Fragment>
          ))}
        </p>
      )}
    </div>
  );
}

// Previewed only where the browser can show it safely: images, HTML in a
// sandboxed frame the server also sandboxes, PDF on a desktop.
export function SharedFile({ file, href }: { file: FilePart; href: string }) {
  const touch = coarsePointer();
  const preview = file.mime.startsWith("image/") ? (
    <ImageTile src={href} name={file.name} />
  ) : file.mime === "text/html" ? (
    <iframe
      src={href}
      title={file.name}
      sandbox="allow-scripts allow-forms allow-popups"
      loading="lazy"
    />
  ) : file.mime === "application/pdf" && !touch ? (
    <iframe src={href} title={file.name} loading="lazy" />
  ) : null;
  return (
    <figure class="node-file">
      {preview}
      <figcaption>
        <strong>{cleanText(file.name)}</strong>
        <span class="muted">{bytes(file.bytes)}</span>
        {/^(image\/|application\/pdf$|text\/html$)/.test(file.mime) && (
          <a href={href} target="_blank" rel="noopener">
            Open
          </a>
        )}
        <a href={`${href}?download=1`} download={file.name}>
          Download
        </a>
      </figcaption>
    </figure>
  );
}
