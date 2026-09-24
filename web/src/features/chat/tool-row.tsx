import { DisclosureRow, Skeleton, LoadError, Time } from "../../shared/ui.tsx";
import DiffView from "./diff-view.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import { cleanText } from "../../shared/display.ts";
import type { PresentedBlock, ToolPart } from "../../shared/types.ts";

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
        ) : (
          <dl class="tool-fields" key={index}>
            {part.rows.map(([label, value]) => (
              <>
                <dt>{label}</dt>
                <dd>{cleanText(value)}</dd>
              </>
            ))}
          </dl>
        ),
      )}
    </>
  );
}

// One chrome for every tool call and result. Titles and the diff-only choice
// come from getToolRow; how input and output read comes from the native view.
export function ToolRow({
  block,
  title,
  subtitle,
  running,
  diffOnly,
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
  title: string;
  subtitle: string;
  running: boolean;
  diffOnly: boolean;
  output: string;
  text?: string;
  expanding: boolean;
  loadError: unknown;
  retry: () => void;
  online: boolean;
  inspect?: (id: string) => void;
  onToggle: (event: { currentTarget: { open: boolean } }) => void;
}) {
  return (
    <DisclosureRow
      className="tool-disclosure"
      label={title}
      status={subtitle}
      icon={
        running ? (
          <span class="status-led running" aria-hidden="true" />
        ) : undefined
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
        {!diffOnly && <ToolInput parts={block.view?.input || []} />}
        {!online && block.truncated && text == null && (
          <p class="small muted">
            Recent output only. Connect to load the full result.
          </p>
        )}
        {expanding && <Skeleton label="Loading full tool output…" />}
        {loadError && <LoadError error={loadError} retry={retry} />}
        {!diffOnly &&
          (text ? (
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
          ))}
        {block.change && <DiffView text={cleanText(block.change)} />}
        {inspect && (
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
