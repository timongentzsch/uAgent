import "../../shared/attachments.css";
import { TurnFooter } from "./turn-footer.tsx";
import Markdown, { prepareMarkdown } from "../../shared/markdown-view.tsx";
import "./message.css";
import { count } from "../../shared/quantities.ts";
import { Component } from "preact";
import {
  presentMessages,
  recallable,
  splitMentionTokens,
  unsent,
} from "../../shared/message-view.ts";
import type {
  ActivityTarget,
  PresentedBlock,
  Block,
  Asset,
  SessionRef,
  LinkPart,
  ToolRowBlock,
} from "../../shared/types.ts";
import { useContext, useEffect, useId, useMemo, useState } from "preact/hooks";
import { formatMoment } from "../../shared/time.ts";
import { devicePrefs } from "../../shared/device-prefs.ts";
import { useStored } from "../../shared/storage.ts";
import { useResource } from "../../shared/use-resource.ts";
import { MessageActions } from "./message-actions.ts";
import { Mail, Minimize2, X } from "lucide-preact";
import {
  Button,
  DisclosureRow,
  cleanText,
  EventRow,
  ErrorBoundary,
  IconButton,
  Time,
} from "../../shared/ui.tsx";
import { MessageMenu } from "./message-menu.tsx";
import { AttachmentList, ImageTile } from "../../shared/attachments.tsx";
import { routes } from "../../state/api.ts";
import { useBlockReader } from "../../state/block-reader.ts";
import { duration } from "../../shared/duration.ts";
import { ToolInline, ToolRow } from "./tool-row.tsx";
import { capitalize, formatStat, isRunning } from "../../shared/display.ts";
import { DetailContext } from "../../shared/verbosity.ts";

// The inspector opens a block's own work; a link part names it.
function linkTarget(block: ToolRowBlock, link: LinkPart): ActivityTarget {
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
  assets,
}: {
  id: string;
  alt: string;
  files?: Asset[];
  assets: string;
}) {
  const file = files?.find((item) => item.id === id);
  if (!file) return <span class="muted">@{alt} (attachment removed)</span>;
  const href = `${assets}${file.id}`;
  return file.image ? (
    <span class="mention-image">
      <ImageTile src={href} name={file.name} />
    </span>
  ) : (
    <a class="file-chip mention-chip" href={href} download={file.name}>
      @{file.name}
    </a>
  );
}

// Who wrote a row and when, e.g. "You, 10:32": the row's accessible name
// and its menu's. Smart and absolute styles only, so it needs no ticking.
function useRowName(who: string, time?: string) {
  const [prefs] = useStored(devicePrefs.time);
  const date = time ? new Date(time) : null;
  if (!date || Number.isNaN(date.getTime())) return who;
  const style = prefs.style === "absolute" ? "absolute" : "smart";
  return `${who}, ${formatMoment(date, { ...prefs, style })}`;
}

// Deliveries worth a line: how the model got a file only when it was not
// the file itself (a reference, a vision-model description).
const AS_SENT = new Set(["Image", "Document", "Text", "Audio", "Video"]);

type MessageProps = {
  block: PresentedBlock;
  online: boolean;
  session: SessionRef;
};

function MessageView({ block, online, session }: MessageProps) {
  const {
    read,
    inspect,
    report,
    statistics,
    activity,
    recall,
    retry: resend,
    branch,
    http,
  } = useContext(MessageActions);
  const { policy } = useContext(DetailContext);
  // A level that shows output opens the row on the preview it holds.
  const [expanded, setExpanded] = useState(policy.open);
  // The full text loads when asked for: by opening the row, or from a row
  // or output box that is already open, or by Show full message.
  const [wantFull, setWantFull] = useState(false);
  const mail =
    block.kind === "user" && block.origin === "mail" ? block : undefined;
  const call = block.kind === "tool_result" ? block : undefined;
  const tool = !!call;
  const reply = block.kind === "assistant" ? block : undefined;
  const outgoing = block.kind === "user" ? block : undefined;
  const files = "files" in block ? block.files : undefined;
  const deliveries = "deliveries" in block ? block.deliveries : undefined;
  const unavailableImages =
    "unavailable_images" in block ? block.unavailable_images : undefined;
  const load = useBlockReader(session, read);
  const fullText = useResource(
    (signal) =>
      online && wantFull && block.truncated
        ? load(
            call?.detail_id || (call ? `t-${call.call_id}` : block.id),
            tool,
            signal,
          ).then((page): string =>
            tool ? JSON.parse(page.text).response : page.text,
          )
        : undefined,
    [
      online,
      wantFull,
      block.truncated,
      call?.detail_id,
      call?.call_id,
      session.id,
    ],
  );
  const full = wantFull ? (fullText.value ?? null) : null;
  const expanding =
    online && wantFull && !!block.truncated && full === null && !fullText.error;
  // A row shows why its whole text did not load; a message reports it.
  const row = block.kind === "tool_result" || block.kind === "activity";
  // biome-ignore lint/correctness/useExhaustiveDependencies: reports each failure once
  useEffect(() => {
    if (!row && fullText.error) report(fullText.error);
  }, [fullText.error]);
  const text = full ?? block.text ?? "";
  // A long diff arrives as its opening; the row shows the stored whole,
  // read once per session and retry.
  const diff = useResource(
    (signal) =>
      online && call?.change_path
        ? load(call.detail_id || `t-${call.call_id}`, false, signal).then(
            (page) => page.text,
          )
        : undefined,
    [online, call?.change_path, session.id],
  );
  const change = diff.value;
  const output = useMemo(
    () => (expanded ? cleanText(text) : ""),
    [expanded, text],
  );
  // Attribution, not authorship: the user's bar and agent rows carry no
  // label; only other kinds name themselves.
  // Receipts (memory saves, finished background work) read as tool rows.
  const userOwned =
    block.kind === "user" ||
    (block.kind === "attachment" && block.origin !== "tool");
  const agentRow =
    block.kind === "assistant" ||
    (block.kind === "attachment" && block.origin === "tool");
  const nameId = useId();
  const name = useRowName(
    userOwned
      ? "You"
      : agentRow
        ? "Assistant"
        : row
          ? call?.name || "Tool"
          : capitalize(block.kind),
    block.time,
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
  // Another session's or the harness's words: an event with its sender's
  // label, never the person's bar. The label is the text's own opening
  // bracket ("[thread event, not a user message] …").
  if (mail) {
    const match = /^\[([^\]]+)\]\s*([\s\S]*)$/.exec(text || "");
    const label = match?.[1] ?? "Message";
    const body = match?.[2] ?? "";
    // The first line is the event; what follows it is the detail.
    const [first, ...rest] = body.split("\n");
    const detail = rest.join("\n").trim() || body;
    return (
      <EventRow
        title={`${label.replace(", not a user message", "")} · ${first}`}
        time={block.time}
        icon={<Mail />}
        messageId={block.key || block.id}
      >
        <p>{cleanText(detail)}</p>
      </EventRow>
    );
  }
  if (block.kind === "turn_summary" && block.summary)
    return (
      <TurnFooter
        summary={block.summary}
        session={session}
        open={() => statistics?.(block)}
      />
    );
  const actor = userOwned || row || agentRow ? null : block.kind;
  const running = row && isRunning(block.state);
  const assets = routes.assets(session.id);
  const header = !row;
  return (
    <article
      data-message-id={block.key || block.id}
      data-sequence={block.sequence}
      className={`message ${row ? "tool" : userOwned ? "user" : "response"}${block.turn_root === block.id ? " turn-start" : ""}`}
      aria-labelledby={nameId}
    >
      <h3 class="sr-only" id={nameId}>
        {name}
      </h3>
      {header && (
        <header>
          {actor && <span class="actor">{actor}</span>}
          {outgoing?.status && <span class="muted">{outgoing.status}</span>}
          <Time value={block.time} />
          {recall && outgoing && recallable(outgoing, online) && (
            <IconButton
              label={
                unsent(outgoing)
                  ? "Return message to composer"
                  : "Recall guidance to composer"
              }
              title="Return to composer"
              onClick={() => recall(outgoing)}
            >
              <X />
            </IconButton>
          )}
          <MessageMenu
            label={`Actions for ${name}`}
            block={block}
            statistics={statistics}
            http={http}
            branch={userOwned ? branch : undefined}
          />
        </header>
      )}
      {row && (
        <>
          <div class="tool-row-head">
            <ToolRow
              block={block}
              running={running}
              output={output}
              text={text}
              expanding={expanding}
              loadError={fullText.error || diff.error}
              retry={fullText.error ? fullText.retry : diff.retry}
              online={online}
              inspect={inspect}
              open={policy.open}
              loadFull={
                block.truncated && full === null
                  ? () => setWantFull(true)
                  : undefined
              }
              onToggle={(event) => {
                setExpanded(event.currentTarget.open);
                if (event.currentTarget.open) setWantFull(true);
              }}
            />
            <MessageMenu
              label={`Actions for ${name}`}
              block={block}
              statistics={statistics}
              http={http}
            />
          </div>
          <ToolInline
            block={change && call ? { ...call, change } : block}
            text={text}
            loaded={full !== null}
            loadFull={() => setWantFull(true)}
            online={online}
            assets={assets}
            open={activity && ((link) => activity(linkTarget(block, link)))}
          />
        </>
      )}
      {reply?.reasoning && policy.reasoning !== "hidden" && (
        <DisclosureRow
          className="thinking"
          label="Thinking"
          open={policy.reasoning === "open"}
          status={reply.streaming ? "streaming" : undefined}
        >
          {/* Reasoning stays plain while streaming: a never-opened
              disclosure must not pay renderer work. */}
          <Markdown
            text={reply.reasoning}
            streaming={reply.streaming}
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
                streaming={reply?.streaming}
              />
            ) : null
          ) : (
            <MentionFile
              key={`m-${part.mention.id}-${index}`}
              id={part.mention.id}
              alt={part.mention.alt}
              files={files}
              assets={assets}
            />
          ),
        )}
      {deliveries
        ?.filter((item) => !AS_SENT.has(item.delivery))
        .map((item) => (
          <p class="small muted" key={item.name}>
            {item.name} · {item.delivery}
          </p>
        ))}
      {!online && !!files?.length && (
        <p class="small muted">Attachments are available when connected.</p>
      )}
      {online && !!files?.length && !row && (
        <AttachmentList files={files} href={(id) => `${assets}${id}`} />
      )}
      {!row && block.truncated && (
        <Button disabled={expanding} onClick={() => setWantFull(full === null)}>
          {expanding
            ? "Loading…"
            : full !== null
              ? "Show less"
              : "Show full message"}
        </Button>
      )}
      {outgoing && (outgoing.error || (resend && unsent(outgoing))) && (
        <p role="alert" class={unsent(outgoing) ? "failure unsent" : undefined}>
          {outgoing.error || outgoing.status}
          {resend && unsent(outgoing) && (
            <Button
              size="compact"
              disabled={!online}
              onClick={() => resend(outgoing)}
            >
              Retry
            </Button>
          )}
        </p>
      )}
      {(unavailableImages || 0) > 0 && (
        <p class="muted">
          {count(unavailableImages)} historical image(s) have no retained
          browser asset reference.
        </p>
      )}
    </article>
  );
}

// Rows stay the same objects while their blocks do (see presentMessages),
// so a streamed frame renders the 1-2 rows it touched instead of
// reconciling all 256. The inner view keeps hooks (expanded/full) and
// disclosure state.
class Message extends Component<MessageProps> {
  shouldComponentUpdate(next: MessageProps) {
    const prior = this.props;
    return (
      next.block !== prior.block ||
      next.online !== prior.online ||
      next.session.id !== prior.session.id ||
      next.session.generation !== prior.session.generation
    );
  }
  render(props: MessageProps) {
    return (
      <ErrorBoundary>
        {props.block.kind === "group" ? (
          <GroupRow {...props} />
        ) : (
          <MessageView {...props} />
        )}
      </ErrorBoundary>
    );
  }
}

// Folded rows as one row, expanding to the rows themselves: the label says
// what they did, the status what their edits changed.
function GroupRow(props: MessageProps) {
  const steps = props.block.children || [];
  const running = steps.some((step) =>
    step.kind === "tool_result" || step.kind === "activity"
      ? isRunning(step.state)
      : !("duration_ms" in step && step.duration_ms != null),
  );
  const lines = formatStat(
    steps.reduce<[number, number]>(
      (sum, step) => {
        const result = "result" in step ? step.result : undefined;
        return [sum[0] + (result?.added ?? 0), sum[1] + (result?.removed ?? 0)];
      },
      [0, 0],
    ),
  );
  return (
    <article
      data-message-id={props.block.key}
      // The rows it folds, so a find can open it to reach one.
      data-sequences={steps.map((step) => step.sequence).join(" ")}
      className="message tool group"
    >
      <DisclosureRow
        className={`tool-disclosure${running ? " running" : ""}`}
        label={props.block.label || ""}
        status={running ? "running" : lines || undefined}
      >
        {steps.map((step) => (
          <Message key={step.key || step.id} {...props} block={step} />
        ))}
      </DisclosureRow>
    </article>
  );
}

type MessageRowsProps = Omit<MessageProps, "block"> & {
  blocks: Block[];
};

// Prepare only the bounded completed page about to be mounted. The same
// parser cache is read synchronously by Markdown on its first render.
export async function prepareHistoryBlocks(blocks: Block[]) {
  const texts = new Set<string>();
  for (const block of presentMessages(blocks)) {
    if (
      (block.kind === "assistant" && block.streaming) ||
      block.kind === "compaction" ||
      block.kind === "turn_summary"
    )
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
// The rows are memoized per blocks array; per-row shouldComponentUpdate
// skips unchanged rows during streaming.
// Keys are scoped to the session: stable message IDs are local to a
// conversation, so a reused instance must never carry expansion,
// disclosure or markdown state from another conversation for the same ID.
// The verbosity level is part of the key too: a new level restyles every row
// from its own start, and until then a row stays as the person left it.
export function MessageRows({ blocks, ...props }: MessageRowsProps) {
  const { level, policy } = useContext(DetailContext);
  const rows = useMemo(() => presentMessages(blocks, policy), [blocks, policy]);
  const scope = `${props.session?.id || ""}:${level}:`;
  return (
    <>
      {rows.map((row) => (
        <Message key={`${scope}${row.key || row.id}`} block={row} {...props} />
      ))}
    </>
  );
}
