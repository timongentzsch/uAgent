import { AgentAssignment } from "../../shared/agent-assignment.tsx";
import { AgentSender } from "./agent-sender.tsx";
import { MessageText } from "./message-text.tsx";
import {
  agentLink,
  agentBatch,
  agentText,
  messageExcerpt,
} from "../../shared/message-view.ts";
import { Fragment } from "preact";
import type { ComponentChildren } from "preact";
import {
  useContext,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { Button, LoadError, Skeleton } from "../../shared/ui.tsx";
import DiffView from "../../shared/diff-view.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import { capitalize, cleanText, shortAgentName } from "../../shared/display.ts";
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
import { fenced } from "../../shared/format.ts";
import { MessageActions } from "./message-actions.ts";

// A numbered excerpt takes its bytes from the result body. Number only the
// displayed lines, so opening a row never duplicates the file on the wire.
const numbered = (part: ToolPart) =>
  part.kind === "code" &&
  Number.isSafeInteger(part.first_line) &&
  part.first_line! > 0;

function Parts({
  parts,
  node,
  assets,
  online,
  output,
}: {
  output?: string;
  parts: ToolPart[];
  node: Node;
  assets: string;
  online: boolean;
}) {
  const host = useContext(HostContext);
  const { activity, openFile } = useContext(MessageActions);
  return parts.map((part, index) => {
    switch (part.kind) {
      case "command":
        return (
          <pre class="node-command" key={index}>
            {cleanText(part.text)}
          </pre>
        );
      case "markdown":
        return <Markdown key={index} text={cleanText(part.text)} />;
      case "source": {
        const label = part.title || part.url;
        return (
          <div class="node-source" key={index}>
            {/^https?:\/\//i.test(part.url) ? (
              <a href={part.url} target="_blank" rel="noopener noreferrer">
                {label}
              </a>
            ) : (
              <span>{label}</span>
            )}
            {part.title && <small class="muted">{part.url}</small>}
            {part.excerpt && <p>{cleanText(part.excerpt)}</p>}
          </div>
        );
      }
      case "code":
        return (
          <figure class="node-code" key={index}>
            {part.label && <figcaption class="small">{part.label}</figcaption>}
            {numbered(part) && output !== undefined ? (
              <pre class="node-text">
                {output
                  .replace(/\n$/, "")
                  .split("\n")
                  .map((line, i) => `${part.first_line! + i}\t${line}`)
                  .join("\n")}
              </pre>
            ) : (
              <Markdown text={fenced(cleanText(part.text), part.language)} />
            )}
          </figure>
        );
      case "fields":
        return (
          <dl class="node-fields" key={index}>
            {part.rows.map(([label, value], row) => (
              <Fragment key={row}>
                <dt>{label}</dt>
                <dd>{cleanText(value)}</dd>
              </Fragment>
            ))}
          </dl>
        );
      case "file":
        return <SharedFile key={index} file={part} href={assets + part.id} />;
      case "link": {
        if (part.to === "agent") return null; // Paired with its assignment below.
        const open =
          part.to === "browser"
            ? host && (() => host.setModal({ type: "browser" }))
            : part.to === "file"
              ? openFile && (() => openFile({ path: String(part.id) }))
              : activity && (() => activity(linkTarget(node, part)));
        return open ? (
          <Button
            key={index}
            variant="quiet"
            class="node-action small"
            disabled={part.to !== "browser" && !online}
            onClick={open}
          >
            {part.label}
          </Button>
        ) : (
          <span key={index} class="small muted">
            {part.label}
          </span>
        );
      }
    }
  });
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

function AgentReply({
  text,
  assets,
  refs,
}: {
  text: string;
  assets: string;
  refs: boolean;
}) {
  const [open, setOpen] = useState(false);
  const excerpt = messageExcerpt(text);
  return (
    <>
      <MessageText text={open ? text : excerpt} assets={assets} refs={refs} />
      {excerpt !== text && (
        <Button
          variant="quiet"
          class="message-expand small"
          aria-expanded={open}
          onClick={() => setOpen(!open)}
        >
          {open ? "Show less" : "Show full message"}
        </Button>
      )}
    </>
  );
}

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
  const { inspect, openFile } = useContext(MessageActions);
  const load = useNodeReader(session);
  const body = node.body;
  const parts = node.parts || [];
  const initial = agentBatch(node);
  // A diff loads as it opens. The full batch only loads when requested: a
  // truncated JSON preview may have ended inside an assignment or report.
  const [wantFull, setWantFull] = useState(body?.kind === "diff");
  const full = useResource(
    (signal) => {
      if (!online || !wantFull || !body?.truncated) return;
      return load(node.id, !!initial, signal).then(({ text }) => {
        if (!initial) return { text, complete: true };
        const exchange = JSON.parse(text);
        return {
          text:
            typeof exchange?.response === "string"
              ? (exchange.response as string)
              : text,
          complete: exchange?.complete !== false,
        };
      });
    },
    [online, wantFull, body?.truncated, node.id, session.id, !!initial],
  );
  const loaded = wantFull && full.value !== undefined;
  const loading =
    online && wantFull && !!body?.truncated && !loaded && !full.error;
  const text = cleanText(loaded ? full.value!.text : body?.text || "");
  const Draw = body && (bodies[body.kind] || Text);
  const assets = routes.assets(session.id);
  const batch = useMemo(
    () => (loaded ? agentBatch(node, text) : initial),
    [node, loaded, text, initial],
  );
  const replies =
    batch?.replies ??
    parts.filter(agentLink).map((link) => ({
      link,
      assignment: link.label,
      state: link.state,
      report: link.report,
    }));
  const formatted = batch?.recognized || (batch && body?.truncated && !loaded);
  const displayParts = parts.filter((part) => {
    switch (part.kind) {
      case "fields":
        return part.rows.length > 0;
      case "command":
      case "markdown":
        return !!part.text;
      case "code":
        return !!part.text || (numbered(part) && !!body);
      case "source":
        return !!part.url;
      case "file":
        return !!part.id;
      case "link":
        return ["activity", "memory", "browser", "file"].includes(part.to);
      default:
        return false;
    }
  });
  // Parts-owned previews leave the complete exchange in the inspector. Keep
  // plain text for failed calls and for parts an older client cannot draw.
  const structured =
    node.state === "ok" &&
    body &&
    ["fields", "image", "link"].includes(body.kind) &&
    parts.length > 0 &&
    displayParts.length + parts.filter(agentLink).length === parts.length;
  const drawText =
    !!text && (!formatted || node.state === "failed") && !structured;
  const actions = [
    body?.truncated &&
      !wantFull &&
      body.kind !== "tail" &&
      (!structured || initial || !inspect || displayParts.some(numbered)) && (
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
  ].filter(Boolean);
  const call = node.kind === "call";
  const input = !formatted && node.input?.length ? node.input : undefined;
  const context = { node, online, assets };
  return (
    <div class="node-body">
      {input && (
        <div
          class="node-section"
          role={call ? "group" : undefined}
          aria-label={call ? "Request" : undefined}
        >
          {call && <small class="muted">Request</small>}
          <Parts parts={input} {...context} />
        </div>
      )}
      {(drawText || displayParts.length > 0) && (
        <div
          class="node-section"
          role={call ? "group" : undefined}
          aria-label={call ? "Response" : undefined}
        >
          {call && <small class="muted">Response</small>}
          {drawText && Draw && body && (
            <Draw
              node={node}
              text={text}
              more={!!body.truncated && !loaded}
              online={online}
              loadFull={() => setWantFull(true)}
            />
          )}
          {displayParts.length > 0 && (
            <Parts parts={displayParts} output={text} {...context} />
          )}
        </div>
      )}
      {(formatted || (structured && !drawText)) && !inspect && text && (
        <details>
          <summary>Recorded output</summary>
          <pre class="node-text plain">{text}</pre>
        </details>
      )}
      {replies.map(({ link, assignment, state, report }, index) => (
        <div class="node-agent" key={link?.id || index}>
          {link ? (
            <AgentSender
              id={String(link.id)}
              title={assignment}
              outcome={state}
              live
              online={online}
            />
          ) : (
            <strong>{shortAgentName(assignment || state || "Agent")}</strong>
          )}
          {batch && (
            <AgentAssignment
              title={assignment}
              name={shortAgentName(assignment)}
            />
          )}
          {!link && assignment && state && (
            <small class="muted">{capitalize(state)}</small>
          )}
          {report && (
            <AgentReply
              text={agentText(report, state)}
              refs={!!openFile}
              assets={assets}
            />
          )}
        </div>
      ))}
      {loaded && !full.value?.complete && (
        <p class="small muted">Only a preview was retained.</p>
      )}
      {loading && <Skeleton label="Loading full output…" />}
      {full.error && <LoadError error={full.error} retry={full.retry} />}
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
