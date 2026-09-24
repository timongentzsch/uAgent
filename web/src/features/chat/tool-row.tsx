import { DisclosureRow, Skeleton, LoadError } from "../../shared/ui.tsx";
import DiffView from "./diff-view.tsx";
import { cleanText, formatDateTime } from "../../shared/display.ts";
import type { PresentedBlock } from "../../shared/types.ts";

// One chrome for every tool call and result. Titles and the diff-only choice
// come from getToolRow, so per-tool differences never live in this JSX.
export function ToolRow({
  block,
  title,
  subtitle,
  running,
  diffOnly,
  argumentsText,
  input,
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
  argumentsText: string;
  input: string;
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
        {block.source?.time && (
          <p class="small muted">
            Called {formatDateTime(block.source.time)}
            {block.result_loaded &&
              block.time &&
              ` · completed ${formatDateTime(block.time)}`}
          </p>
        )}
        <p class="small muted">{block.name}</p>
        {!diffOnly && argumentsText && <pre>{input}</pre>}
        {!online && block.truncated && text == null && (
          <p class="small muted">
            Recent output only. Connect to load the full result.
          </p>
        )}
        {expanding && <Skeleton label="Loading full tool output…" />}
        {loadError && <LoadError error={loadError} retry={retry} />}
        {!diffOnly &&
          (text ? (
            <pre>{output}</pre>
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
