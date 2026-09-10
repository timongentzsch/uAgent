import "./attachments.css";
import Markdown from "./markdown-view.tsx";
import { duration } from "./duration.ts";
import "./message.css";
import { bytes, count } from "./quantities.ts";
import { observeResize } from "./layout.ts";
import type { ComponentProps } from "preact";
import { presentMessages } from "./message-view.ts";
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
import { ChevronRight } from "lucide-preact";
import { readPages } from "./store.ts";
import { Mark, cleanText, Skeleton, LoadError } from "./ui.tsx";
import { Menu, MenuItem } from "./popover.tsx";
import { formatBody } from "./format.ts";

function Message({
  block,
  session,
  inspect,
  image,
  report,
  statistics,
  activity,
  http,
}: {
  block: PresentedBlock;
  session: SessionRef;
  inspect: (id: string) => void;
  image: (url: string) => void;
  report: Report;
  statistics: (block: Block) => void;
  activity: (block: Block) => void;
  http: (exchanges: Exchange[]) => void;
}) {
  const [full, setFull] = useState<string | null>(null);
  const [expanding, setExpanding] = useState(false);
  const [expanded, setExpanded] = useState(false);
  const [loadError, setLoadError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const text = full ?? block.text;
  const tool = block.kind === "tool_result";
  useEffect(() => {
    if (!tool || !expanded || !block.truncated || full !== null) return;
    const abort = new AbortController();
    setExpanding(true);
    setLoadError(null);
    readPages(
      `/api/sessions/${session.id}?detail=${encodeURIComponent(block.detail_id || `t-${block.call_id}`)}&raw=1`,
      abort.signal,
    )
      .then((page) => {
        if (!abort.signal.aborted) setFull(JSON.parse(page.text).response);
      })
      .catch((error) => {
        if (!abort.signal.aborted) setLoadError(error);
      })
      .finally(() => {
        if (!abort.signal.aborted) setExpanding(false);
      });
    return () => abort.abort();
  }, [
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
  const exchanges = block.http || block.source?.http;
  return (
    <article
      data-message-id={block.id}
      data-turn-root={block.turn_root}
      className={`message ${tool ? "tool" : ["user", "attachment"].includes(block.kind) ? "user" : "response"} ${block.turn_root === block.id ? "turn-start" : ""}`}
    >
      <header>
        {tool ? (
          <button
            class="quiet tool-toggle"
            aria-expanded={expanded}
            onClick={() => setExpanded(!expanded)}
          >
            <ChevronRight class={expanded ? "expanded" : ""} />
            <strong>{block.name || "tool"}</strong>
            <span class="muted">
              {block.status || "pending"}
              {block.duration_ms != null && ` · ${duration(block.duration_ms)}`}
            </span>
          </button>
        ) : (
          <span>
            {block.kind === "attachment" ? (
              "you"
            ) : block.kind === "user" ? (
              "you"
            ) : block.kind === "assistant" ? (
              <Mark />
            ) : (
              block.kind
            )}
          </span>
        )}
        {!tool && block.status && (
          <span class="muted">
            {block.status}
            {block.duration_ms != null && ` · ${duration(block.duration_ms)}`}
          </span>
        )}
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
        <Menu label="Message menu">
          <MenuItem onClick={() => statistics(block)}>Statistics</MenuItem>
          {block.source && (
            <MenuItem onClick={() => statistics(block.source!)}>
              Model call statistics
            </MenuItem>
          )}
          {(block.kind === "assistant" || (exchanges?.length || 0) > 0) && (
            <MenuItem onClick={() => http(exchanges || [])}>
              HTTP request/response
            </MenuItem>
          )}
        </Menu>
      </header>
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
              {argumentsText && <pre>{input}</pre>}
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
              <button
                onClick={() => inspect(block.detail_id || `t-${block.call_id}`)}
              >
                Tool input/output
              </button>
            </div>
          )
        : text && <Markdown text={text} streaming={block.streaming} />}
      {!block.files?.length && (block.images?.length || 0) > 0 && (
        <div class="images">
          {block.images?.map((id) => (
            <button
              class="image"
              key={id}
              onClick={() => image(`/api/sessions/${session.id}/assets/${id}`)}
            >
              <img
                loading="lazy"
                alt="Attached image"
                src={`/api/sessions/${session.id}/assets/${id}`}
                onError={(event) => {
                  event.currentTarget.alt = "Image unavailable";
                }}
              />
            </button>
          ))}
        </div>
      )}
      {!!block.files?.length && (
        <div class="attachments">
          {block.files
            .filter((file) => typeof file === "object")
            .map((file) => (
              <a
                class="file-chip"
                key={file.id}
                href={`/api/sessions/${session.id}/assets/${file.id}`}
                download={file.name}
              >
                {file.image && (
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
              const page = await readPages(
                `/api/sessions/${session.id}?detail=${encodeURIComponent(block.id)}`,
              );
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
      {block.kind === "activity" && (
        <button class="quiet" onClick={() => activity(block)}>
          View activity
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

export default function Messages({
  blocks,
  restoreScroll,
  ...props
}: Omit<ComponentProps<typeof Message>, "block"> & {
  blocks: Block[];
  restoreScroll: () => void;
}) {
  const rows = useMemo(() => presentMessages(blocks), [blocks]);
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
      {rows.map((block) => (
        <Message key={block.key || block.id} block={block} {...props} />
      ))}
    </div>
  );
}
