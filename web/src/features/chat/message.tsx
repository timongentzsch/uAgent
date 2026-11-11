import { ArtifactCard } from "./artifact-card.tsx";
import "../../shared/attachments.css";
import "./message.css";
import Markdown, { prepareMarkdown } from "../../shared/markdown-view.tsx";
import { count } from "../../shared/quantities.ts";
import {
  queuedGuidance,
  recallable,
  shownText,
  splitMentionTokens,
  unsent,
} from "../../shared/message-view.ts";
import type { Asset, Node, SessionRef } from "../../shared/types.ts";
import { useContext, useEffect, useId, useState } from "preact/hooks";
import { formatMoment } from "../../shared/time.ts";
import { devicePrefs } from "../../shared/device-prefs.ts";
import { useStored } from "../../shared/storage.ts";
import { useResource } from "../../shared/use-resource.ts";
import { MessageActions } from "./message-actions.ts";
import { X } from "lucide-preact";
import { Button, IconButton, Time } from "../../shared/ui.tsx";
import { MessageMenu } from "./message-menu.tsx";
import { AttachmentList, ImageTile } from "../../shared/attachments.tsx";
import { useApi } from "../../state/api.ts";
import { useNodeReader } from "../../state/node-reader.ts";
import { isRow } from "../../shared/tree.ts";
import { TurnFooter } from "./turn-footer.tsx";
import { SharedFile } from "./bodies.tsx";

// Inline @-mention reference. Resolves against the message's own files so
// a renamed file shows its current name; a removed one degrades to muted
// text instead of a broken image. No files (replies echoing the syntax):
// the splitter never emits mentions, so this stays unreachable.
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
    <a class="chip file-chip mention-chip" href={href} download={file.name}>
      @{file.name}
    </a>
  );
}

// Who wrote a message and when, e.g. "You, 10:32": its accessible name and
// its menu's. Smart and absolute styles only, so it needs no ticking.
function useRowName(who: string, time?: string) {
  const [prefs] = useStored(devicePrefs.time);
  const date = time ? new Date(time) : null;
  if (!date || Number.isNaN(date.getTime())) return who;
  const style = prefs.style === "absolute" ? "absolute" : "smart";
  return `${who}, ${formatMoment(date, { ...prefs, style })}`;
}

// Who wrote a message in your place, for its accessible name.
const AUTHORS: Record<string, string> = {
  agent: "Agent report",
  activity: "Background result",
  message: "Message",
};

// Deliveries worth a line: how the model got a file only when it was not
// the file itself (a reference, a vision-model description).
const AS_SENT = new Set(["Image", "Document", "Text", "Audio", "Video"]);

// A node drawn whole rather than as a row: your message, with its time and
// menu on its first line; a reply or the files the agent handed you, which
// have no header line (an answer's time and menu sit at the end of its
// turn's footer); files a call brought into context; and a turn's footer.
export function MessageView({
  node,
  answer,
  online,
  session,
}: {
  node: Node;
  answer?: Node;
  online: boolean;
  session: SessionRef;
}) {
  if (node.kind === "footer")
    return <TurnFooter node={node} answer={answer} session={session} />;
  return node.origin === "tool" ? (
    <BroughtFiles node={node} online={online} session={session} />
  ) : (
    <Message node={node} online={online} session={session} />
  );
}

// Files a call's result brought into context, under that call.
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
  online,
  session,
}: {
  node: Node;
  online: boolean;
  session: SessionRef;
}) {
  const { routes } = useApi();
  const {
    read,
    report,
    recall,
    retry: resend,
    branch,
    http,
    open,
    openFile,
  } = useContext(MessageActions);
  // The whole text loads when asked for by Show full message.
  const [wantFull, setWantFull] = useState(false);
  const reply = node.role === "assistant";
  // What woke the conversation in your place has your message's line, not
  // its bubble.
  const chrome = reply ? "response" : node.origin ? "mail" : "user inverse";
  const load = useNodeReader(session, read);
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
  const assets = routes.assets(session.id);
  const nameId = useId();
  const name = useRowName(
    reply ? "Assistant" : (node.origin && AUTHORS[node.origin]) || "You",
    node.time,
  );
  const agent = node.agent;
  const words =
    text &&
    splitMentionTokens(text).map((part, index) =>
      "text" in part ? (
        part.text ? (
          // Segments render independently: emphasis spanning a token
          // keeps its words but loses the styling (rare, accepted).
          <Markdown
            key={`t-${index}`}
            text={part.text}
            streaming={node.streaming}
            refs={!!openFile}
          />
        ) : null
      ) : (
        <MentionFile
          key={`m-${part.mention.id}-${index}`}
          id={part.mention.id}
          alt={part.mention.alt}
          files={node.files}
          assets={assets}
        />
      ),
    );
  return (
    <article
      data-message-id={node.id}
      data-sequence={node.sequence}
      className={`message ${chrome}`}
      aria-labelledby={nameId}
    >
      <h3 class="sr-only" id={nameId}>
        {name}
      </h3>
      {reply ? (
        words
      ) : (
        <div class="message-line">
          <div class="message-text">
            {node.origin === "activity" && (
              <div class="message-event">A background command finished</div>
            )}
            {agent && (
              <div class="message-event">
                <span>
                  Agent “{agent.title}” finished
                  {agent.outcome !== "completed" && ` (${agent.outcome})`}
                </span>
                {open && (
                  <Button
                    variant="quiet"
                    size="compact"
                    disabled={!online}
                    onClick={() => open(agent.session)}
                  >
                    Open agent
                  </Button>
                )}
              </div>
            )}
            {words}
          </div>
          <span class="message-meta small">
            {node.status && <span>{node.status}</span>}
            <Time value={node.time} />
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
              label={`Actions for ${name}`}
              node={node}
              http={http}
              branch={branch}
            />
          </span>
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
      {/* What the agent handed you: a file, previewed where that is safe,
          or an artifact in the folder. */}
      {node.parts?.map((part, index) =>
        part.kind === "file" ? (
          <SharedFile key={index} file={part} href={`${assets}${part.id}`} />
        ) : (
          part.kind === "artifact" &&
          openFile && (
            <ArtifactCard
              key={index}
              session={session.id}
              part={part}
              open={openFile}
            />
          )
        ),
      )}
      {truncated && (
        <Button disabled={expanding} onClick={() => setWantFull(full === null)}>
          {expanding
            ? "Loading…"
            : full !== null
              ? "Show less"
              : "Show full message"}
        </Button>
      )}
      {(node.error || (resend && unsent(node))) && (
        <p role="alert" class={unsent(node) ? "failure unsent" : undefined}>
          {node.error || node.status}
          {resend && unsent(node) && (
            <Button
              size="compact"
              disabled={!online}
              onClick={() => resend(node)}
            >
              Retry
            </Button>
          )}
        </p>
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

// Prepare only the bounded completed page about to be mounted. The same
// parser cache is read synchronously by Markdown on its first render; a
// conversation's messages cite files, so they render with references.
export async function prepareHistoryNodes(nodes: Node[]) {
  const texts = new Set<string>();
  for (const node of nodes) {
    if (isRow(node) || node.streaming || !node.body?.text) continue;
    for (const part of splitMentionTokens(shownText(node, node.body.text))) {
      if ("text" in part && part.text) texts.add(part.text);
    }
  }
  await Promise.all([...texts].map((text) => prepareMarkdown(text, true)));
}
