import "./attachments.css";
import { TurnFooter } from "./statistics.tsx";
import Markdown from "./markdown-view.tsx";
import { duration } from "./duration.ts";
import "./message.css";
import { bytes, count } from "./quantities.ts";
import { observeResize } from "./layout.ts";
import type { ComponentProps } from "preact";
import { presentMessages, showsHeader } from "./message-view.ts";
import type {
  PresentedBlock,
  Block,
  SessionRef,
  Exchange,
  Report,
} from "./types.ts";
import {
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import {
  ChevronRight,
  Activity as ActivityIcon,
  Brain,
  Minimize2,
} from "lucide-preact";
import { readPages } from "./store.ts";
import { Mark, cleanText, Skeleton, LoadError, EventRow } from "./ui.tsx";
import { Menu, MenuItem } from "./popover.tsx";
import { formatBody } from "./format.ts";

export function Message({
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
  const load = (id: string, raw: boolean, signal?: AbortSignal) =>
    read
      ? read(id, raw, signal)
      : readPages(
          `/api/sessions/${session.id}?detail=${encodeURIComponent(id)}${raw ? "&raw=1" : ""}`,
          signal,
        );
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
  const argumentsText =
    typeof block.arguments === "string"
      ? block.arguments
      : JSON.stringify(block.arguments, null, 2);
  const input = useMemo(
    () => (expanded ? formatBody(cleanText(argumentsText)) : ""),
    [expanded, argumentsText],
  );
  const output = useMemo(
    () => (expanded ? formatBody(cleanText(text)) : ""),
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
          {duration(block.compaction.duration_ms)}
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
        status={block.status}
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
  const exchanges = block.http || block.source?.http;
  // Attribution, not authorship: user uploads read as "you"; every new
  // assistant response carries the mark, and a turn that opens with tool
  // calls (no thinking or text) gets its single mark on the first tool row.
  const userOwned =
    block.kind === "user" ||
    (block.kind === "attachment" && block.origin !== "tool");
  const agentRow =
    block.kind === "assistant" ||
    (block.kind === "attachment" && block.origin === "tool");
  const actor = userOwned ? (
    "you"
  ) : block.kind === "assistant" ? (
    <Mark />
  ) : agentRow ? (
    block.firstOfTurn && <Mark />
  ) : (
    block.kind
  );
  // One header per step: user turns always open, agent rows only flag the
  // turn's first row. Bodies that follow share the step, not the chrome.
  const showHeader = showsHeader(block);
  return (
    <article
      data-message-id={block.id}
      data-turn-root={block.turn_root}
      className={`message ${tool ? "tool" : userOwned ? "user" : "response"} ${block.turn_root === block.id ? "turn-start" : ""}${!tool && !showHeader ? " grouped" : ""}`}
    >
      {(tool || showHeader) && (
        <header>
          {tool ? (
            <button
              class="quiet tool-toggle"
              aria-expanded={expanded}
              onClick={() => setExpanded(!expanded)}
            >
              <span class="tool-icons">
                {showHeader && block.firstOfTurn && <Mark />}
                <ChevronRight class={expanded ? "expanded" : ""} />
              </span>
              <strong title={block.activity?.label || block.name}>
                {block.activity?.label || block.name || "tool"}
              </strong>
              <span class="muted">
                {block.activity?.category && `${block.activity.category} · `}
                {block.status || "pending"}
                {block.duration_ms != null &&
                  ` · ${duration(block.duration_ms)}`}
              </span>
            </button>
          ) : showHeader && actor ? (
            <span>{actor}</span>
          ) : null}
          {!tool && showHeader && block.status && (
            <span class="muted">
              {block.status}
              {block.duration_ms != null && ` · ${duration(block.duration_ms)}`}
            </span>
          )}
          {showHeader && (
            <time
              dateTime={block.time}
              title={
                block.time
                  ? new Date(block.time).toLocaleString()
                  : "Time not recorded"
              }
            >
              {block.time
                ? new Date(block.time).toLocaleTimeString([], {
                    hour: "2-digit",
                    minute: "2-digit",
                  })
                : "—"}
            </time>
          )}
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
          {showHeader && (statistics || http) && (
            <Menu label="Message menu">
              {statistics && (
                <MenuItem onClick={() => statistics(block)}>
                  Statistics
                </MenuItem>
              )}
              {block.source && statistics && (
                <MenuItem onClick={() => statistics(block.source!)}>
                  Model call statistics
                </MenuItem>
              )}
              {http &&
                (block.kind === "assistant" ||
                  (exchanges?.length || 0) > 0) && (
                  <MenuItem onClick={() => http(exchanges || [])}>
                    HTTP request/response
                  </MenuItem>
                )}
            </Menu>
          )}
        </header>
      )}
      {block.reasoning && (
        <details class="thinking">
          <summary>Thinking{block.streaming ? " · streaming" : ""}</summary>
          <Markdown text={block.reasoning} streaming={block.streaming} />
        </details>
      )}
      {tool
        ? expanded && (
            <div class="tool-body">
              {block.source?.time && (
                <p class="small muted">
                  Called {new Date(block.source.time).toLocaleString()}
                  {block.result_loaded &&
                    block.time &&
                    ` · completed ${new Date(block.time).toLocaleString()}`}
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
              {block.change && (
                <pre class="diff">{cleanText(block.change)}</pre>
              )}
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
          )
        : text && <Markdown text={text} streaming={block.streaming} />}
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

type MessageRowsProps = Omit<ComponentProps<typeof Message>, "block"> & {
  blocks: Block[];
};

export function MessageRows({ blocks, ...props }: MessageRowsProps) {
  const rows = useMemo(() => presentMessages(blocks), [blocks]);
  return (
    <>
      {rows.map((block) =>
        block.children ? (
          <details
            class="message exploration"
            key={block.key}
            data-message-id={block.id}
            data-turn-root={block.turn_root}
          >
            <summary>
              {block.firstOfTurn && <Mark />}
              {block.activity?.group?.label}
            </summary>
            {block.children.map((child) => (
              <Message key={child.key || child.id} block={child} {...props} />
            ))}
          </details>
        ) : (
          <Message key={block.key || block.id} block={block} {...props} />
        ),
      )}
    </>
  );
}

export default function Messages({
  restoreScroll,
  ...props
}: MessageRowsProps & {
  restoreScroll: () => void;
}) {
  const content = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const element = content.current;
    if (!element) return;
    const stopObserving = observeResize(restoreScroll, element);
    restoreScroll();
    return stopObserving;
  }, [restoreScroll]);
  return (
    <div ref={content}>
      <MessageRows {...props} />
    </div>
  );
}
