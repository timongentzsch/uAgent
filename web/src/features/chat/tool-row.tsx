import { DisclosureRow, Skeleton, LoadError, Time } from "../../shared/ui.tsx";
import DiffView from "./diff-view.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import { cleanText } from "../../shared/display.ts";
import type {
  FilePart,
  LinkPart,
  PresentedBlock,
  ToolPart,
} from "../../shared/types.ts";
import { StatusLed } from "../../shared/connection-status.tsx";
import { useContext } from "preact/hooks";
import { LiveActivities } from "../../state/live-activities.ts";
import { duration } from "../../shared/duration.ts";
import { bytes } from "../../shared/quantities.ts";
import { active } from "./activity-status.tsx";
import { getToolRow } from "./tool-preview.ts";

// A fence longer than any backtick run in the body, so code never ends early.
function fenced(text: string, language = "") {
  const longest = Math.max(
    2,
    ...(text.match(/`+/g) || []).map((run) => run.length),
  );
  const fence = "`".repeat(longest + 1);
  return `${fence}${language}\n${text}\n${fence}`;
}

// The input half of a native ToolView. Each tool chose its parts; this
// component only knows the closed vocabulary, never a tool name.
export function ToolInput({ parts }: { parts: ToolPart[] }) {
  return (
    <>
      {parts.map((part, index) =>
        part.kind === "command" ? (
          <pre class="tool-command" key={index}>
            {cleanText(part.text)}
          </pre>
        ) : part.kind === "code" ? (
          <figure class="tool-code" key={index}>
            {part.label && <figcaption>{part.label}</figcaption>}
            <Markdown text={fenced(cleanText(part.text), part.language)} />
          </figure>
        ) : part.kind === "fields" ? (
          <dl class="tool-fields" key={index}>
            {part.rows.map(([label, value]) => (
              <>
                <dt>{label}</dt>
                <dd>{cleanText(value)}</dd>
              </>
            ))}
          </dl>
        ) : null,
      )}
    </>
  );
}

// A link part opens the work it names in the inspector.
export type OpenLink = (link: LinkPart) => void;

const DIFF_LINES = 40;
const TAIL_LINES = 3;

function lastLines(text: string, count: number) {
  return text
    .split("\n")
    .filter((line) => line.trim())
    .slice(-count)
    .join("\n");
}

// The work a row started, while it still runs: its LED and elapsed time.
function useLive(block: PresentedBlock) {
  const link = block.parts?.find(
    (part): part is LinkPart => part.kind === "link",
  );
  const live = useContext(LiveActivities).find((item) =>
    link?.to === "agent"
      ? item.agent_id === String(link.id)
      : link?.to === "activity" && item.id === Number(link.id),
  );
  return live && active(live) ? live : undefined;
}

// One chrome for every tool call and receipt: a status dot, the view's verb
// and target, and on expand its input, full output and raw record.
export function ToolRow({
  block,
  running,
  output,
  text,
  expanding,
  loadError,
  retry,
  online,
  inspect,
  onToggle,
}: {
  block: PresentedBlock;
  running: boolean;
  output: string;
  text?: string;
  expanding: boolean;
  loadError: unknown;
  retry: () => void;
  online: boolean;
  inspect?: (id: string) => void;
  onToggle: (event: { currentTarget: { open: boolean } }) => void;
}) {
  const live = useLive(block);
  // Work the call started keeps the row in the present tense until it ends.
  let { title, subtitle } = getToolRow(block, running || !!live);
  if (live) {
    subtitle = [
      live.status,
      live.started_ms && duration(Math.max(0, Date.now() - live.started_ms)),
    ]
      .filter(Boolean)
      .join(" · ");
  }
  const failed = /fail|error|timed_out|denied/i.test(block.status || "");
  const diff = block.change?.includes("\n") ? block.change : "";
  return (
    <DisclosureRow
      className="tool-disclosure"
      label={title}
      status={subtitle}
      icon={
        <StatusLed
          state={running || live ? "running" : failed ? "failed" : "active"}
        />
      }
      onToggle={onToggle}
    >
      <div class="tool-body">
        <p class="small muted">
          {block.name}
          {block.source?.time && (
            <>
              {" · called "}
              <Time value={block.source.time} />
            </>
          )}
          {block.result_loaded && block.time && (
            <>
              {" · completed "}
              <Time value={block.time} />
            </>
          )}
        </p>
        <ToolInput parts={block.view?.input || []} />
        {!online && block.truncated && text == null && (
          <p class="small muted">
            Recent output only. Connect to load the full result.
          </p>
        )}
        {expanding && <Skeleton label="Loading full tool output…" />}
        {loadError && <LoadError error={loadError} retry={retry} />}
        {text ? (
          block.view?.output === "markdown" ? (
            <div class="tool-output">
              <Markdown text={output} />
            </div>
          ) : (
            <pre class="tool-output">{output}</pre>
          )
        ) : (
          block.result_loaded === false && (
            <p class="muted">
              Output is not loaded. Open the full tool input/output.
            </p>
          )
        )}
        {diff.split("\n").length > DIFF_LINES && (
          <DiffView text={cleanText(diff)} />
        )}
        {inspect && block.kind === "tool_result" && (
          <button
            onClick={() => inspect(block.detail_id || `t-${block.call_id}`)}
          >
            Tool input/output
          </button>
        )}
      </div>
    </DisclosureRow>
  );
}

// What stays visible under a row without expanding it: a change's diff, a
// command's last lines, and the files and work the call produced.
export function ToolInline({
  block,
  text,
  running,
  online,
  assets,
  open,
}: {
  block: PresentedBlock;
  text?: string;
  running: boolean;
  online: boolean;
  assets: string;
  open?: OpenLink;
}) {
  const diff = block.change?.includes("\n") ? block.change : "";
  const lines = diff.split("\n");
  const tail =
    block.view?.output === "tail" && !running
      ? lastLines(cleanText(block.tail ?? text ?? ""), TAIL_LINES)
      : "";
  const parts = block.parts || [];
  if (!diff && !tail && !parts.length) return null;
  return (
    <div class="tool-inline">
      {diff && (
        <DiffView text={cleanText(lines.slice(0, DIFF_LINES).join("\n"))} />
      )}
      {lines.length > DIFF_LINES && (
        <p class="small muted">
          {lines.length - DIFF_LINES} more lines · expand the row for the full
          diff
        </p>
      )}
      {tail && <pre class="tool-tail">{tail}</pre>}
      {parts.map((part, index) =>
        part.kind === "file" ? (
          <FileCard key={index} file={part} href={`${assets}${part.id}`} />
        ) : part.kind === "link" && open ? (
          <button
            key={index}
            type="button"
            class="quiet tool-link"
            disabled={!online}
            onClick={() => open(part)}
          >
            {part.label}
          </button>
        ) : null,
      )}
    </div>
  );
}

// A file the agent shared: previewed where the browser can show it safely
// (images; HTML in a sandboxed frame the server also sandboxes; PDF on a
// desktop), then its name, size and Open / Download.
function FileCard({ file, href }: { file: FilePart; href: string }) {
  const touch = matchMedia("(pointer: coarse)").matches;
  const preview = file.mime.startsWith("image/") ? (
    <img src={href} alt={file.name} loading="lazy" />
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
    <figure class="tool-file">
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
