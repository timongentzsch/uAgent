import "../composer/attachments.css";
import { TurnFooter } from "./turn-footer.tsx";
import Markdown, { prepareMarkdown } from "../../shared/markdown-view.tsx";
import "./message.css";
import { bytes, count } from "../../shared/quantities.ts";
import { Component, type ComponentProps } from "preact";
import { presentMessages, splitMentionTokens } from "./message-view.ts";
import type {
  PresentedBlock,
  Block,
  Asset,
  SessionRef,
  Exchange,
  LinkPart,
  Report,
} from "../../shared/types.ts";
import { useEffect, useMemo, useState } from "preact/hooks";
import { Minimize2, X } from "lucide-preact";
import {
  DisclosureRow,
  Mark,
  cleanText,
  EventRow,
  ErrorBoundary,
  Time,
} from "../../shared/ui.tsx";
import { MessageMenu } from "./message-menu.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { useBlockReader } from "../../state/block-reader.ts";
import { duration } from "../../shared/duration.ts";
import { ToolInline, ToolRow } from "./tool-row.tsx";
import { isRunningStatus, statusLine } from "../../shared/display.ts";

// The inspector opens a block's own work; a link part names it.
function linkTarget(block: Block, link: LinkPart): Block {
  return {
    ...block,
    agent_id: link.to === "agent" ? String(link.id) : undefined,
    activity_id: link.to === "activity" ? Number(link.id) : undefined,
    memory:
      link.to === "memory"
        ? { action: "", key: String(link.id), automatic: false }
        : undefined,
  };
}

// Inline @-mention reference. Resolves against the message's own files so
// a renamed file shows its current name; a removed one degrades to muted
// text instead of a broken image. No files (assistant rows echoing the
// syntax): the splitter never emits mentions, so this stays unreachable.
function MentionFile({
  id,
  alt,
  files,
  sessionId,
  online,
}: {
  id: string;
  alt: string;
  files?: (Asset | string)[];
  sessionId: string;
  online: boolean;
}) {
  const file = files?.find(
    (item): item is Asset => typeof item === "object" && item.id === id,
  );
  if (!file) return <span class="muted">@{alt} (attachment removed)</span>;
  const href = `/api/sessions/${sessionId}/assets/${file.id}`;
  return file.image && online ? (
    <img
      class="mention-image"
      src={href}
      alt={file.name}
      title={file.name}
      loading="lazy"
    />
  ) : (
    <a class="file-chip mention-chip" href={href} download={file.name}>
      @{file.name}
    </a>
  );
}

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
  // A command's output box can ask for the full text without expanding.
  const [wantFull, setWantFull] = useState(false);
  const [loadError, setLoadError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  const text = full ?? block.text;
  const tool = block.kind === "tool_result";
  const load = useBlockReader(session, read);
  useEffect(() => {
    if (
      !online ||
      (!tool && block.kind !== "activity") ||
      !(expanded || wantFull) ||
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
    wantFull,
    block.truncated,
    block.detail_id,
    block.call_id,
    session.id,
    retry,
  ]);
  const output = useMemo(
    () => (expanded ? cleanText(text) : ""),
    [expanded, text],
  );
  if (block.kind === "compaction" && block.compaction)
    return (
      <EventRow
        title="Context compacted"
        time={block.time}
        icon={<Minimize2 />}
        messageId={block.key || block.id}
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
  if (block.summary)
    return (
      <TurnFooter summary={block.summary} open={() => statistics?.(block)} />
    );
  // Attribution, not authorship: user uploads read as "you"; every agent
  // row carries the mark. The header below is identical on every row — no
  // per-step variants, so chrome and spacing can never drift apart.
  // Receipts (memory saves, finished background work) read as tool rows.
  const row = tool || block.kind === "activity";
  const userOwned =
    block.kind === "user" ||
    (block.kind === "attachment" && block.origin !== "tool");
  const agentRow =
    block.kind === "assistant" ||
    (block.kind === "attachment" && block.origin === "tool");
  const actor =
    userOwned || row
      ? userOwned
        ? "you"
        : null
      : block.kind === "assistant" || agentRow
        ? Mark
        : block.kind;
  return (
    <article
      data-message-id={block.key || block.id}
      className={`message ${row ? "tool" : userOwned ? "user" : "response"}${block.turn_root === block.id ? " turn-start" : ""}`}
    >
      {!row && (
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
          <Time value={block.time} />
          {recall &&
            online &&
            block.status === "Guidance queued" &&
            block.request_id && (
              <button
                class="quiet icon-button"
                aria-label="Recall guidance to composer"
                title="Recall to composer"
                onClick={() => recall(block)}
              >
                <X aria-hidden="true" />
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
      {row &&
        (() => {
          const running =
            block.duration_ms == null && isRunningStatus(block.status);
          return (
            <>
              <div class="tool-row-head">
                <ToolRow
                  block={block}
                  running={running}
                  output={output}
                  text={text}
                  expanding={expanding}
                  loadError={loadError}
                  retry={() => setRetry(retry + 1)}
                  online={online}
                  inspect={inspect}
                  onToggle={(event) => setExpanded(event.currentTarget.open)}
                />
                <MessageMenu
                  label="Tool menu"
                  block={block}
                  statistics={statistics}
                  http={http}
                />
              </div>
              <ToolInline
                block={block}
                text={text}
                loaded={full !== null}
                loadFull={() => setWantFull(true)}
                online={online}
                assets={`/api/sessions/${session.id}/assets/`}
                open={activity && ((link) => activity(linkTarget(block, link)))}
              />
            </>
          );
        })()}
      {block.reasoning && (
        <DisclosureRow
          className="thinking"
          label="Thinking"
          status={block.streaming ? "streaming" : undefined}
        >
          {/* Reasoning stays plain while streaming: a never-opened
              disclosure must not pay renderer work. */}
          <Markdown
            text={block.reasoning}
            streaming={block.streaming}
            progressive={false}
          />
        </DisclosureRow>
      )}
      {!row &&
        text &&
        splitMentionTokens(text).map((part, index) =>
          "text" in part ? (
            part.text ? (
              // Segments render independently: emphasis spanning a token
              // keeps its words but loses the styling (rare, accepted).
              <Markdown
                key={`t-${index}`}
                text={part.text}
                streaming={block.streaming}
              />
            ) : null
          ) : (
            <MentionFile
              key={`m-${part.mention.id}-${index}`}
              id={part.mention.id}
              alt={part.mention.alt}
              files={block.files}
              sessionId={session.id}
              online={online}
            />
          ),
        )}
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
      {!row && block.truncated && (
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

// An Explored row changes when a step is added or one of them settles.
const stepsKey = (block: PresentedBlock) =>
  (block.children || [])
    .map((step) => `${step.key || step.id}:${step.status}:${step.duration_ms}`)
    .join();

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
    x.parts === y.parts &&
    x.tail === y.tail &&
    stepsKey(x) === stepsKey(y) &&
    (x.files?.length || 0) === (y.files?.length || 0) &&
    (x.http?.length || 0) === (y.http?.length || 0) &&
    x.arguments === y.arguments &&
    x.view === y.view &&
    x.summary === y.summary &&
    x.compaction === y.compaction &&
    x.memory === y.memory &&
    x.replay?.title === y.replay?.title &&
    x.replay?.summary === y.replay?.summary &&
    (x.duration_ms ?? null) === (y.duration_ms ?? null) &&
    (x.change ?? "") === (y.change ?? "")
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
    return (
      <ErrorBoundary>
        {props.block.kind === "explore" ? (
          <ExploredRow {...props} />
        ) : (
          <MessageView {...props} />
        )}
      </ErrorBoundary>
    );
  }
}

// A run of read-only calls as one row: "Explored · 3 files, 2 searches",
// expanding to the calls themselves.
function ExploredRow(props: MessageProps) {
  const steps = props.block.children || [];
  const running = steps.some(
    (step) => step.duration_ms == null && isRunningStatus(step.status),
  );
  const files = steps.filter((step) => step.name === "read_path").length;
  const searches = steps.filter((step) => step.name === "grep").length;
  const other = steps.length - files - searches;
  const counts = [
    files && `${count(files)} file${files === 1 ? "" : "s"}`,
    searches && `${count(searches)} search${searches === 1 ? "" : "es"}`,
    other && `${count(other)} other`,
  ]
    .filter(Boolean)
    .join(", ");
  return (
    <article
      data-message-id={props.block.key}
      className="message tool explored"
    >
      <DisclosureRow
        className="tool-disclosure"
        label={running ? "Exploring" : "Explored"}
        status={counts}
        icon={<StatusLed state={running ? "running" : "active"} />}
      >
        {steps.map((step) => (
          <Message key={step.key || step.id} {...props} block={step} />
        ))}
      </DisclosureRow>
    </article>
  );
}

export type MessageRowsProps = Omit<ComponentProps<typeof Message>, "block"> & {
  blocks: Block[];
};

// Prepare only the bounded completed page about to be mounted. The same
// parser cache is read synchronously by Markdown on its first render.
export async function prepareHistoryBlocks(blocks: Block[]) {
  const texts = new Set<string>();
  for (const block of presentMessages(blocks)) {
    if (block.streaming || block.kind === "compaction" || block.summary)
      continue;
    if (block.kind === "activity") {
      if (block.text) texts.add(cleanText(block.text));
      continue;
    }
    if (block.kind === "tool_result") continue;
    const text = block.text || "";
    for (const part of splitMentionTokens(text)) {
      if ("text" in part && part.text) texts.add(part.text);
    }
  }
  await Promise.all([...texts].map((text) => prepareMarkdown(text)));
}

// Flat list with stable keys: one row per message, tool call or attachment.
// presentMessages is memoized per blocks array; per-row shouldComponentUpdate
// skips unchanged rows during streaming.
// Keys are scoped to the session: stable message IDs are local to a
// conversation, so a reused instance must never carry expansion,
// disclosure or markdown state from another conversation for the same ID.
export function MessageRows({ blocks, ...props }: MessageRowsProps) {
  const rows = useMemo(() => presentMessages(blocks), [blocks]);
  const scope = props.session?.id ? `${props.session.id}:` : "";
  return (
    <>
      {rows.map((row) => (
        <Message key={`${scope}${row.key || row.id}`} block={row} {...props} />
      ))}
    </>
  );
}
