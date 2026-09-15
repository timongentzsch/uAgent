import "./attachments.css";
import { TurnFooter } from "./statistics.tsx";
import Markdown from "./markdown-view.tsx";
import DiffView from "./diff-view.tsx";
import "./message.css";
import { bytes, count } from "./quantities.ts";
import { Component, type ComponentProps } from "preact";
import { presentMessages } from "./message-view.ts";
import type {
  PresentedBlock,
  Block,
  SessionRef,
  Exchange,
  Report,
} from "./types.ts";
import { useEffect, useMemo, useState } from "preact/hooks";
import { Activity as ActivityIcon, Brain, Minimize2 } from "lucide-preact";
import {
  DisclosureRow,
  Mark,
  cleanText,
  Skeleton,
  LoadError,
  EventRow,
} from "./ui.tsx";
import { MessageMenu } from "./message-menu.tsx";
import { useBlockReader } from "./block-reader.ts";
import { getToolPreview } from "./tool-preview.ts";
import {
  formatDateTime,
  formatDuration,
  formatTime,
  isRunningStatus,
  previewBody,
  statusLine,
  stringifyArgs,
} from "./display.ts";

function MessageView({
  block,
  online,
  session,
  inspect,
  report,
  statistics,
  activity,
  recall,
  http,
  read,
}: {
  block: PresentedBlock;
  online: boolean;
  session: SessionRef;
  read?: (
    id: string,
    raw: boolean,
    signal?: AbortSignal,
  ) => Promise<{ text: string }>;
  inspect?: (id: string) => void;
  report: Report;
  statistics?: (block: Block) => void;
  activity?: (block: Block) => void;
  recall?: (block: PresentedBlock) => void;
  http?: (exchanges: Exchange[]) => void;
}) {
  const [full, setFull] = useState<string | null>(null);
  const [expanding, setExpanding] = useState(false);
  const [expanded, setExpanded] = useState(false);
  const [loadError, setLoadError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const text = full ?? block.text;
  const tool = block.kind === "tool_result";
  const load = useBlockReader(session, read);
  useEffect(() => {
    if (
      !online ||
      (!tool && block.kind !== "activity") ||
      !expanded ||
      !block.truncated ||
      full !== null
    )
      return;
    const abort = new AbortController();
    setExpanding(true);
    setLoadError(null);
    load(
      block.detail_id || (tool ? `t-${block.call_id}` : block.id),
      tool,
      abort.signal,
    )
      .then((page) => {
        if (!abort.signal.aborted)
          setFull(tool ? JSON.parse(page.text).response : page.text);
      })
      .catch((error) => {
        if (!abort.signal.aborted) setLoadError(error);
      })
      .finally(() => {
        if (!abort.signal.aborted) setExpanding(false);
      });
    return () => abort.abort();
  }, [
    online,
    tool,
    expanded,
    block.truncated,
    block.detail_id,
    block.call_id,
    session.id,
    retry,
  ]);
  const argumentsText = stringifyArgs(block.arguments);
  const input = useMemo(
    () => (expanded ? previewBody(argumentsText) : ""),
    [expanded, argumentsText],
  );
  const output = useMemo(
    () => (expanded ? previewBody(text) : ""),
    [expanded, text],
  );
  if (block.kind === "compaction" && block.compaction)
    return (
      <EventRow
        title="Context compacted"
        time={block.time}
        icon={<Minimize2 />}
      >
        <p>
          {block.compaction.messages_before} → {block.compaction.messages_after}{" "}
          messages in model context. Conversation history is retained.
        </p>
        <p class="muted">
          {block.compaction.automatic ? "Automatic" : "Manual"} ·{" "}
          {formatDuration(block.compaction.duration_ms)}
        </p>
      </EventRow>
    );
  if (block.kind === "activity")
    return (
      <EventRow
        title={
          block.activity?.label ||
          cleanText(text).split("\n")[0] ||
          "Background activity"
        }
        time={block.time}
        status={
          isRunningStatus(block.status) && block.duration_ms == null
            ? "Running\u2026"
            : block.status
        }
        icon={block.memory ? <Brain /> : <ActivityIcon />}
        onToggle={(event) => setExpanded(event.currentTarget.open)}
      >
        {expanding && <Skeleton label="Loading event details…" />}
        {loadError && (
          <LoadError error={loadError} retry={() => setRetry(retry + 1)} />
        )}
        <Markdown text={cleanText(text)} />
        {activity &&
          (block.memory?.key || block.agent_id || block.activity_id) && (
            <button
              class="quiet"
              disabled={!online}
              onClick={() => activity(block)}
            >
              {block.memory
                ? "View current memory"
                : block.agent_id
                  ? "View subagent"
                  : "View activity"}
            </button>
          )}
      </EventRow>
    );
  if (block.summary)
    return (
      <TurnFooter summary={block.summary} open={() => statistics?.(block)} />
    );
  // Attribution, not authorship: user uploads read as "you"; every agent
  // row carries the mark. The header below is identical on every row — no
  // per-step variants, so chrome and spacing can never drift apart.
  const userOwned =
    block.kind === "user" ||
    (block.kind === "attachment" && block.origin !== "tool");
  const agentRow =
    block.kind === "assistant" ||
    (block.kind === "attachment" && block.origin === "tool");
  const actor =
    userOwned || tool
      ? userOwned
        ? "you"
        : null
      : block.kind === "assistant" || agentRow
        ? Mark
        : block.kind;
  return (
    <article
      className={`message ${tool ? "tool" : userOwned ? "user" : "response"}${block.turn_root === block.id ? " turn-start" : ""}`}
    >
      {!tool && (
        <header>
          {actor === Mark ? <Mark /> : actor && <span>{actor}</span>}
          {block.status && (
            <span class="muted">
              {statusLine({
                status: block.status,
                duration_ms: block.duration_ms,
              })}
            </span>
          )}
          {
            <time dateTime={block.time} title={formatDateTime(block.time)}>
              {formatTime(block.time)}
            </time>
          }
          {recall &&
            online &&
            block.status === "Guidance queued" &&
            block.request_id && (
              <button
                class="quiet"
                aria-label="Recall guidance to composer"
                title="Recall to composer"
                onClick={() => recall(block)}
              >
                ×
              </button>
            )}
          <MessageMenu
            label="Message menu"
            block={block}
            statistics={statistics}
            http={http}
          />
        </header>
      )}
      {tool && (() => {
        const preview = getToolPreview(block);
        const running =
          block.duration_ms == null && isRunningStatus(block.status);
        return (
        <div class="tool-row-head">
          <DisclosureRow
            className="tool-disclosure"
            label={preview.title}
            status={preview.subtitle}
            icon={
              running ? (
                <span class="status-led running" aria-hidden="true" />
              ) : undefined
            }
            onToggle={(event) => setExpanded(event.currentTarget.open)}
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
              {argumentsText && <pre>{input}</pre>}
              {!online && block.truncated && full === null && (
                <p class="small muted">
                  Recent output only. Connect to load the full result.
                </p>
              )}
              {expanding && <Skeleton label="Loading full tool output…" />}
              {loadError && (
                <LoadError
                  error={loadError}
                  retry={() => setRetry(retry + 1)}
                />
              )}
              {text ? (
                <pre>{output}</pre>
              ) : (
                block.result_loaded === false && (
                  <p class="muted">
                    Output is not loaded. Open the full tool input/output.
                  </p>
                )
              )}
              {block.change && <DiffView text={cleanText(block.change)} />}
              {inspect && (
                <button
                  onClick={() =>
                    inspect(block.detail_id || `t-${block.call_id}`)
                  }
                >
                  Tool input/output
                </button>
              )}
            </div>
          </DisclosureRow>
          <MessageMenu
            label="Tool menu"
            block={block}
            statistics={statistics}
            http={http}
          />
        </div>
        );
      })()}
      {block.reasoning && (
        <DisclosureRow
          className="thinking"
          label="Thinking"
          status={block.streaming ? "streaming" : undefined}
        >
          <Markdown text={block.reasoning} streaming={block.streaming} />
        </DisclosureRow>
      )}
      {!tool && text && <Markdown text={text} streaming={block.streaming} />}
      {block.deliveries?.map((item) => (
        <p class="small muted">
          {item.name} · {item.delivery}
        </p>
      ))}
      {!online && !!block.files?.length && (
        <p class="small muted">Attachments are available when connected.</p>
      )}
      {!!block.files?.length && (
        <div class="attachments">
          {block.files
            .filter((file) => typeof file === "object")
            .map((file) => (
              <a
                class="file-chip"
                key={file.id}
                aria-disabled={!online}
                onClick={(event) => {
                  if (!online) event.preventDefault();
                }}
                href={`/api/sessions/${session.id}/assets/${file.id}`}
                download={file.name}
              >
                {online && file.image && (
                  <img
                    loading="lazy"
                    src={`/api/sessions/${session.id}/assets/${file.id}`}
                    alt={file.name}
                  />
                )}
                <span>
                  <span title={`${file.bytes.toLocaleString()} bytes`}>
                    {file.name} · {bytes(file.bytes)}
                  </span>
                </span>
              </a>
            ))}
        </div>
      )}
      {!tool && block.truncated && (
        <button
          disabled={expanding}
          onClick={async () => {
            if (full !== null) {
              setFull(null);
              return;
            }
            setExpanding(true);
            try {
              const page = await load(block.id, false);
              setFull(page.text);
            } catch (error) {
              report(error);
            } finally {
              setExpanding(false);
            }
          }}
        >
          {expanding
            ? "Loading…"
            : full !== null
              ? "Show less"
              : "Show full message"}
        </button>
      )}
      {block.error && <p role="status">{block.error}</p>}
      {(block.unavailable_images || 0) > 0 && (
        <p class="muted">
          {count(block.unavailable_images)} historical image(s) have no retained
          browser asset reference.
        </p>
      )}
    </article>
  );
}

type MessageProps = ComponentProps<typeof MessageView>;

function messagePropsEqual(before: MessageProps, after: MessageProps): boolean {
  const x = before.block;
  const y = after.block;
  return (
    before.online === after.online &&
    before.session.id === after.session.id &&
    (before.session.generation || "") === (after.session.generation || "") &&
    before.read === after.read &&
    before.inspect === after.inspect &&
    before.report === after.report &&
    before.statistics === after.statistics &&
    before.activity === after.activity &&
    before.recall === after.recall &&
    before.http === after.http &&
    x.kind === y.kind &&
    x.text === y.text &&
    x.reasoning === y.reasoning &&
    !!x.streaming === !!y.streaming &&
    x.status === y.status &&
    x.truncated === y.truncated &&
    x.error === y.error &&
    x.time === y.time &&
    x.name === y.name &&
    x.detail_id === y.detail_id &&
    x.call_id === y.call_id &&
    x.activity_id === y.activity_id &&
    x.agent_id === y.agent_id &&
    (x.files?.length || 0) === (y.files?.length || 0) &&
    (x.http?.length || 0) === (y.http?.length || 0) &&
    x.arguments === y.arguments &&
    x.summary === y.summary &&
    x.compaction === y.compaction &&
    x.memory === y.memory
  );
}

// Outer class skips re-render when fields are equal, so a streaming delta
// updates 1-2 rows instead of reconciling all 256. Inner view keeps hooks
// (expanded/full) and disclosure state.
export class Message extends Component<MessageProps> {
  shouldComponentUpdate(next: MessageProps) {
    return !messagePropsEqual(this.props, next);
  }
  render(props: MessageProps) {
    return <MessageView {...props} />;
  }
}

export type MessageRowsProps = Omit<ComponentProps<typeof Message>, "block"> & {
  blocks: Block[];
};

// Flat list with stable keys, except native exploration groups which nest
// adjacent rows for disclosure. presentMessages is memoized per blocks array;
// per-row shouldComponentUpdate skips unchanged rows during streaming.
export function MessageRows({ blocks, ...props }: MessageRowsProps) {
  const rows = useMemo(() => presentMessages(blocks), [blocks]);
  return (
    <>
      {rows.map((block) =>
        block.children ? (
          <DisclosureRow
            className="message exploration"
            key={block.key}
            label={block.activity?.group?.label || "Exploration"}
            icon={<Mark />}
          >
            <div className="exploration-children">
              {block.children.map((child) => (
                <Message key={child.key || child.id} block={child} {...props} />
              ))}
            </div>
          </DisclosureRow>
        ) : (
          <Message key={block.key || block.id} block={block} {...props} />
        ),
      )}
    </>
  );
}
