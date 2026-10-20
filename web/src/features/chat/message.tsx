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
  AssistantBlock,
  PresentedBlock,
  Block,
  Asset,
  SessionRef,
  LinkPart,
  ToolRowBlock,
} from "../../shared/types.ts";
import { useContext, useEffect, useId, useMemo, useState } from "preact/hooks";
import { TimePrefsContext, formatMoment } from "../../shared/time.ts";
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
import { Avatar } from "../../shared/avatar.tsx";
import { MemberAvatar } from "../../shared/member-card.tsx";
import { AttachmentList, ImageTile } from "../../shared/attachments.tsx";
import { routes } from "../../state/api.ts";
import { useBlockReader } from "../../state/block-reader.ts";
import { duration } from "../../shared/duration.ts";
import { ToolInline, ToolRow } from "./tool-row.tsx";
import {
  assetsOf,
  capitalize,
  diffCounts,
  formatStat,
  isRunningStatus,
  statusLine,
} from "../../shared/display.ts";
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
  files?: (Asset | string)[];
  assets: string;
}) {
  const file = assetsOf(files).find((item) => item.id === id);
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
  const prefs = useContext(TimePrefsContext);
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
  // A turn's answer whose time and menu its footer carries, given to that
  // footer as `answer` and to the answer itself as `footed`.
  answer?: PresentedBlock;
  footed?: boolean;
};

function MessageView({ block, online, session, answer, footed }: MessageProps) {
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
    team,
    moderator,
    prompt,
  } = useContext(MessageActions);
  const { policy } = useContext(DetailContext);
  const [full, setFull] = useState<string | null>(null);
  const [expanding, setExpanding] = useState(false);
  // A level that shows output opens the row on the preview it holds.
  const [expanded, setExpanded] = useState(policy.open);
  // The full text loads when asked for: by opening the row, or from a row
  // or output box that is already open.
  const [wantFull, setWantFull] = useState(false);
  const [loadError, setLoadError] = useState<unknown>(null);
  const [retry, setRetry] = useState(0);
  // A chat member's message: shown under its name, without the name its
  // text opens with for the model ("Ada: …").
  const mail =
    block.kind === "user" && block.origin === "mail" ? block : undefined;
  const author = mail?.author;
  // Under the name its session has now: renamed since, what it wrote before
  // follows. One that has left keeps the name it wrote under.
  const seat = team?.find((item) => item.id === mail?.sender);
  const member = author && (seat?.member || author);
  const whole = full ?? block.text ?? "";
  const text =
    author && whole.startsWith(`${author}: `)
      ? whole.slice(author.length + 2)
      : whole;
  const call = block.kind === "tool_result" ? block : undefined;
  const tool = !!call;
  const reply = block.kind === "assistant" ? block : undefined;
  const outgoing = block.kind === "user" ? block : undefined;
  const files = "files" in block ? block.files : undefined;
  const deliveries = "deliveries" in block ? block.deliveries : undefined;
  const unavailableImages =
    "unavailable_images" in block ? block.unavailable_images : undefined;
  const took = "duration_ms" in block ? block.duration_ms : undefined;
  const load = useBlockReader(session, read);
  // biome-ignore lint/correctness/useExhaustiveDependencies: loads when asked for (wantFull, retry); the finished load (full) must not abort itself by re-running
  useEffect(() => {
    if (
      !online ||
      (!tool && block.kind !== "activity") ||
      !wantFull ||
      !block.truncated ||
      full !== null
    )
      return;
    const abort = new AbortController();
    setExpanding(true);
    setLoadError(null);
    load(
      call?.detail_id || (call ? `t-${call.call_id}` : block.id),
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
    wantFull,
    block.truncated,
    call?.detail_id,
    call?.call_id,
    session.id,
    retry,
  ]);
  // A long diff arrives as its opening; the row shows the stored whole.
  const [change, setChange] = useState<string>();
  // biome-ignore lint/correctness/useExhaustiveDependencies: reads the stored diff once per session and retry
  useEffect(() => {
    if (!online || !call?.change_path) return;
    const abort = new AbortController();
    load(call.detail_id || `t-${call.call_id}`, false, abort.signal)
      .then((page) => setChange(page.text))
      .catch((error) => abort.signal.aborted || setLoadError(error));
    return () => abort.abort();
  }, [online, call?.change_path, session.id, retry]);
  const output = useMemo(
    () => (expanded ? cleanText(text) : ""),
    [expanded, text],
  );
  // Attribution, not authorship: the user's bar and agent rows carry no
  // label; only other kinds name themselves.
  // Receipts (memory saves, finished background work) read as tool rows.
  const row = block.kind === "tool_result" || block.kind === "activity";
  const userOwned =
    (block.kind === "user" && !member) ||
    (block.kind === "attachment" && block.origin !== "tool");
  const agentRow =
    block.kind === "assistant" ||
    (block.kind === "attachment" && block.origin === "tool");
  const nameId = useId();
  const name = useRowName(
    member
      ? member
      : userOwned
        ? "You"
        : agentRow
          ? "Assistant"
          : row
            ? call?.name || "Tool"
            : capitalize(block.kind),
    block.time,
  );
  const answerName = useRowName("Assistant", answer?.time);
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
  if (mail && !member) {
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
      >
        {answer && (
          <>
            <Time value={answer.time} />
            <MessageMenu
              label={`Actions for ${answerName}`}
              block={answer}
              statistics={statistics}
              http={http}
            />
          </>
        )}
      </TurnFooter>
    );
  // In a chat with members the panel is named like everyone else.
  const panel =
    !!team?.length && block.kind === "assistant" ? moderator : undefined;
  const actor =
    member || panel || (userOwned || row || agentRow ? null : block.kind);
  const running = took == null && isRunningStatus(block.status);
  const assets = routes.assets(session.id);
  return (
    <article
      data-message-id={block.key || block.id}
      className={`message ${row ? "tool" : userOwned ? "user" : "response"}${member || panel ? " member" : ""}${block.turn_root === block.id ? " turn-start" : ""}`}
      aria-labelledby={nameId}
    >
      <h3 class="sr-only" id={nameId}>
        {name}
      </h3>
      {!row && !footed && (
        <header>
          {member &&
            (seat ? <MemberAvatar item={seat} /> : <Avatar name={member} />)}
          {actor && <span class="actor">{actor}</span>}
          {block.status && (
            <span class="muted">
              {statusLine({
                status: block.status,
                duration_ms: took,
              })}
            </span>
          )}
          <Time value={block.time} />
          {recall && outgoing && recallable(block, online) && (
            <IconButton
              label={
                unsent(block)
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
            prompt={
              prompt && (member || panel) ? () => prompt(member) : undefined
            }
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
              loadError={loadError}
              retry={() => setRetry(retry + 1)}
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
        <AttachmentList
          files={assetsOf(files)}
          href={(id) => `${assets}${id}`}
        />
      )}
      {!row && block.truncated && (
        <Button
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
        </Button>
      )}
      {(outgoing?.error || (resend && unsent(block))) && (
        <p role="alert" class={unsent(block) ? "failure unsent" : undefined}>
          {outgoing?.error || block.status}
          {resend && outgoing && unsent(block) && (
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

// A tool row shows its model message's time, HTTP log and ids, never its
// text, so the reply streaming beside it does not re-render it.
function sourceEqual(a?: AssistantBlock, b?: AssistantBlock) {
  return (
    a?.id === b?.id &&
    a?.time === b?.time &&
    (a?.http?.length || 0) === (b?.http?.length || 0)
  );
}

// Every block field counts, so a new field can never be silently ignored.
// Only arrays the projection rebuilds on each pass compare by their entries.
function blockEqual(x: PresentedBlock, y: PresentedBlock): boolean {
  if (x === y) return true;
  const a = x as unknown as Record<string, unknown>;
  const b = y as unknown as Record<string, unknown>;
  for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) {
    if (a[key] === b[key]) continue;
    if (key === "source") {
      if (!sourceEqual(x.source, y.source)) return false;
      continue;
    }
    const before = (a[key] || []) as PresentedBlock[];
    const after = (b[key] || []) as PresentedBlock[];
    if (key === "children") {
      if (
        before.length !== after.length ||
        !before.every((step, index) => blockEqual(step, after[index]))
      )
        return false;
    } else if (key !== "files" && key !== "http") return false;
    // Rebuilt arrays of the same entries: a changed entry still counts.
    else if (
      before.length !== after.length ||
      before.some((item, index) => item !== after[index])
    )
      return false;
  }
  return true;
}

function messagePropsEqual(before: MessageProps, after: MessageProps): boolean {
  return (
    before.online === after.online &&
    before.footed === after.footed &&
    (before.answer === after.answer ||
      (!!before.answer &&
        !!after.answer &&
        blockEqual(before.answer, after.answer))) &&
    before.session.id === after.session.id &&
    (before.session.generation || "") === (after.session.generation || "") &&
    blockEqual(before.block, after.block)
  );
}

// Outer class skips re-render when fields are equal, so a streaming delta
// updates 1-2 rows instead of reconciling all 256. Inner view keeps hooks
// (expanded/full) and disclosure state.
class Message extends Component<MessageProps> {
  shouldComponentUpdate(next: MessageProps) {
    return !messagePropsEqual(this.props, next);
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
  const running = steps.some(
    (step) =>
      !("duration_ms" in step && step.duration_ms != null) &&
      isRunningStatus(step.status),
  );
  const lines = formatStat(
    steps.reduce<[number, number]>(
      (sum, step) => {
        const [added, removed] = diffCounts(
          "change" in step ? step.change : undefined,
        );
        return [sum[0] + added, sum[1] + removed];
      },
      [0, 0],
    ),
  );
  return (
    <article data-message-id={props.block.key} className="message tool group">
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
  const chat = !!useContext(MessageActions).team?.length;
  const rows = useMemo(
    () => presentMessages(blocks, policy, chat),
    [blocks, policy, chat],
  );
  const scope = `${props.session?.id || ""}:${level}:`;
  // A turn's answer puts its time and menu on the turn's footer, under it.
  // In a chat the panel's answers keep their header, which names them.
  const footed = (index: number) =>
    !chat &&
    rows[index]?.kind === "assistant" &&
    rows[index + 1]?.kind === "turn_summary";
  return (
    <>
      {rows.map((row, index) => (
        <Message
          key={`${scope}${row.key || row.id}`}
          block={row}
          footed={footed(index)}
          answer={footed(index - 1) ? rows[index - 1] : undefined}
          {...props}
        />
      ))}
    </>
  );
}
