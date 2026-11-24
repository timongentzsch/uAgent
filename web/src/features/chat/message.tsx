import { MessageText } from "./message-text.tsx";
import { AgentSender } from "./agent-sender.tsx";
import { ArtifactCard } from "./artifact-card.tsx";
import "../../shared/attachments.css";
import "./message.css";
import { isPrepared, prepareMarkdown } from "../../shared/markdown-view.tsx";
import { count } from "../../shared/quantities.ts";
import {
  agentReport,
  messageExcerpt,
  queuedGuidance,
  recallable,
  shownText,
  splitMentionTokens,
  unsent,
} from "../../shared/message-view.ts";
import type { Node, SessionRef } from "../../shared/types.ts";
import { useContext, useEffect, useId, useState } from "preact/hooks";
import { formatMoment } from "../../shared/time.ts";
import { devicePrefs } from "../../shared/device-prefs.ts";
import { useStored } from "../../shared/storage.ts";
import { useResource } from "../../shared/use-resource.ts";
import { MessageActions } from "./message-actions.ts";
import { ChevronRight, X } from "lucide-preact";
import { Button, IconButton, Notice, Time } from "../../shared/ui.tsx";
import { MessageMenu } from "./message-menu.tsx";
import { AttachmentList } from "../../shared/attachments.tsx";
import { useApi } from "../../state/api.ts";
import { useNodeReader } from "../../state/node-reader.ts";
import { isRow } from "../../shared/tree.ts";
import { TurnFooter } from "./turn-footer.tsx";
import { SharedFile } from "./bodies.tsx";

// Smart and absolute time styles only, so it needs no ticking.
function useRowName(who: string, time?: string) {
  const [prefs] = useStored(devicePrefs.time);
  const date = time ? new Date(time) : null;
  if (!date || Number.isNaN(date.getTime())) return who;
  const style = prefs.style === "absolute" ? "absolute" : "smart";
  return `${who}, ${formatMoment(date, { ...prefs, style })}`;
}

const AUTHORS: Record<string, string> = {
  agent: "Agent report",
  activity: "Background result",
  message: "Message",
};

// Deliveries worth a line: how the model got a file only when it was not
// the file itself (a reference, a vision-model description).
const AS_SENT = new Set(["Image", "Document", "Text", "Audio", "Video"]);

export function MessageView({
  node,
  answer,
  parentReply,
  online,
  session,
}: {
  node: Node;
  answer?: Node;
  parentReply?: boolean;
  online: boolean;
  session: SessionRef;
}) {
  if (node.kind === "footer")
    return <TurnFooter node={node} answer={answer} session={session} />;
  return node.origin === "tool" ? (
    <BroughtFiles node={node} online={online} session={session} />
  ) : (
    <Message
      node={node}
      online={online}
      session={session}
      parentReply={parentReply}
    />
  );
}

function BroughtFiles({
  node,
  online,
  session,
}: {
  node: Node;
  online: boolean;
  session: SessionRef;
}) {
  const { routes } = useApi();
  const assets = routes.assets(session.id);
  if (!node.files?.length) return null;
  return online ? (
    <AttachmentList files={node.files} href={(id) => `${assets}${id}`} />
  ) : (
    <p class="small muted">Attachments are available when connected.</p>
  );
}

function Message({
  node,
  parentReply,
  online,
  session,
}: {
  node: Node;
  parentReply?: boolean;
  online: boolean;
  session: SessionRef;
}) {
  const { routes } = useApi();
  const {
    report,
    recall,
    retry: resend,
    branch,
    http,
    open,
    openFile,
  } = useContext(MessageActions);
  const [wantFull, setWantFull] = useState(false);
  const sender = agentReport(node);
  const reply = node.role === "assistant" || !!sender;
  const chrome = reply ? "response" : node.origin ? "mail" : "user inverse";
  const load = useNodeReader(session);
  const truncated = node.body?.truncated;
  const fullText = useResource(
    (signal) =>
      online && wantFull && truncated
        ? load(node.id, false, signal).then((page) => page.text)
        : undefined,
    [online, wantFull, truncated, node.id, session.id],
  );
  const full = wantFull ? (fullText.value ?? null) : null;
  const expanding =
    online && wantFull && !!truncated && full === null && !fullText.error;
  // biome-ignore lint/correctness/useExhaustiveDependencies: reports each failure once
  useEffect(() => {
    if (fullText.error) report(fullText.error);
  }, [fullText.error]);
  const text = shownText(node, full ?? node.body?.text ?? "");
  const excerpt = sender ? messageExcerpt(text) : text;
  const visible = wantFull ? text : excerpt;
  const assets = routes.assets(session.id);
  const nameId = useId();
  const name = useRowName(
    sender?.title ||
      (reply ? "Assistant" : (node.origin && AUTHORS[node.origin]) || "You"),
    node.time,
  );
  const side = node.agent?.outcome === "merged" && node.agent;
  const words = (!sender?.tool || wantFull) && visible && (
    <MessageText
      text={visible}
      streaming={node.streaming}
      refs={!!openFile}
      files={node.files}
      assets={assets}
    />
  );
  const meta = (!reply || sender) && (
    <span class="message-meta small">
      {node.status && <span>{node.status}</span>}
      <Time value={node.time} brief={!!sender} />
      {recall && recallable(node, online) && (
        <IconButton
          label={
            queuedGuidance(node)
              ? "Recall guidance to composer"
              : "Return message to composer"
          }
          title="Return to composer"
          onClick={() => recall(node)}
        >
          <X />
        </IconButton>
      )}
      <MessageMenu
        label={sender ? "Actions for this message" : `Actions for ${name}`}
        node={node}
        http={http}
        branch={branch}
      />
    </span>
  );
  return (
    <article
      data-message-id={node.id}
      data-sequence={node.sequence}
      className={`message ${chrome}`}
      aria-labelledby={nameId}
    >
      {!sender && (
        <h3
          class={parentReply ? "parent-speaker small" : "sr-only"}
          id={nameId}
        >
          {parentReply ? "µAgent" : name}
        </h3>
      )}
      {sender && (
        <div class="message-byline">
          <AgentSender
            nameId={nameId}
            id={sender.session}
            title={node.agent_label || sender.title}
            outcome={sender.tool ? undefined : sender.outcome}
            online={online}
          />
          {sender.tool && (
            <Button
              variant="quiet"
              class="message-expand request-tool small"
              aria-expanded={wantFull}
              onClick={() => setWantFull(!wantFull)}
            >
              <ChevronRight class="row-chevron" aria-hidden="true" />
              Requested {sender.tool}
            </Button>
          )}
          {(!sender.tool || wantFull) && meta}
        </div>
      )}
      {reply ? (
        words
      ) : (
        <div class="message-line">
          <div class="message-text">
            {node.origin === "activity" && (
              <div class="message-event">A background command finished</div>
            )}
            {side && (
              <div class="message-event">
                <span>Side chat “{side.title}” merged</span>
                {open && (
                  <Button
                    variant="quiet"
                    size="compact"
                    disabled={!online}
                    onClick={() => open(side.session)}
                  >
                    Open side chat
                  </Button>
                )}
              </div>
            )}
            {words}
          </div>
          {meta}
        </div>
      )}
      {node.deliveries
        ?.filter((item) => !AS_SENT.has(item.delivery))
        .map((item) => (
          <p class="small muted" key={item.name}>
            {item.name} · {item.delivery}
          </p>
        ))}
      {!online && !!node.files?.length && (
        <p class="small muted">Attachments are available when connected.</p>
      )}
      {online && !!node.files?.length && (
        <AttachmentList files={node.files} href={(id) => `${assets}${id}`} />
      )}
      {node.parts?.map((part, index) =>
        part.kind === "file" ? (
          <SharedFile key={index} file={part} href={`${assets}${part.id}`} />
        ) : (
          part.kind === "artifact" &&
          openFile && (
            <ArtifactCard
              key={index}
              session={session}
              sequence={node.sequence}
              part={part}
              open={openFile}
            />
          )
        ),
      )}
      {(!sender?.tool || !!fullText.error) &&
        (truncated || excerpt !== text) && (
          <Button
            variant="quiet"
            class="message-expand small"
            aria-expanded={wantFull}
            disabled={
              expanding ||
              (!online && !wantFull && excerpt === text && !!truncated)
            }
            onClick={() =>
              fullText.error && wantFull
                ? fullText.retry()
                : setWantFull(!wantFull)
            }
          >
            {expanding
              ? "Loading…"
              : wantFull && fullText.error
                ? "Retry full message"
                : wantFull
                  ? "Show less"
                  : "Show full message"}
          </Button>
        )}
      {(node.error || (resend && unsent(node))) && (
        <Notice
          tone="error"
          action={
            resend &&
            unsent(node) && (
              <Button
                size="compact"
                disabled={!online}
                onClick={() => resend(node)}
              >
                Retry
              </Button>
            )
          }
        >
          {node.error || node.status}
        </Notice>
      )}
      {(node.unavailable_images || 0) > 0 && (
        <p class="muted">
          {count(node.unavailable_images)} historical image(s) have no retained
          browser asset reference.
        </p>
      )}
    </article>
  );
}

// A page's texts not parsed yet: none for a page shown before.
export function unprepared(nodes: Node[]) {
  const texts = new Set<string>();
  for (const node of nodes) {
    if (isRow(node) || node.streaming || !node.body?.text) continue;
    const sender = agentReport(node);
    if (sender?.tool) continue;
    const text = shownText(node, node.body.text);
    for (const part of splitMentionTokens(
      sender ? messageExcerpt(text) : text,
    )) {
      if ("text" in part && part.text) texts.add(part.text);
    }
  }
  return [...texts].filter((text) => !isPrepared(text, true));
}

// Prepares only the page about to be mounted; Markdown reads the same
// parser cache synchronously on its first render.
export const prepareHistoryNodes = (nodes: Node[]) =>
  Promise.all(unprepared(nodes).map((text) => prepareMarkdown(text, true)));
