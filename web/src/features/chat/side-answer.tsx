import { ArrowUpRight, CircleAlert, Square, X } from "lucide-preact";
import type { ComponentChildren } from "preact";
import { useEffect, useId, useRef } from "preact/hooks";
import type { Act, Node, Session } from "../../shared/types.ts";
import { Button, IconButton, LoadError } from "../../shared/ui.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { Tabs } from "../../shared/tabs.tsx";
import { useAction } from "../../shared/use-action.ts";
import { StartField } from "../../shared/start-field.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import Decision from "../../shared/decision.tsx";
import { useApi } from "../../state/api.ts";
import { statusOf } from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import { useTranscriptHistory } from "../../state/use-transcript-history.ts";
import type { Asking, Side } from "../../app/use-side-conversations.ts";
import { Transcript } from "./node.tsx";
import { MessageActions } from "./message-actions.ts";

// A side chat's own part of its transcript, from its first question on.
// Where that has left the window, everything held is its own; where it is
// not there yet, nothing is.
export function sideNodes(nodes: Node[], request: string, more?: boolean) {
  if (!request) return nodes;
  const first = nodes.findIndex(
    (node) => node.kind === "turn" && node.request_id === request,
  );
  return first >= 0 ? nodes.slice(first) : more ? nodes : [];
}

// The question, under what it is about, in the box every message is
// written in. Escape leaves what is being asked.
function SideForm({
  quote,
  drop,
  cancel,
  label,
  placeholder,
  action,
  disabled,
  send,
  children,
}: {
  quote?: string;
  drop: () => void;
  cancel: () => void;
  label: string;
  placeholder: string;
  action: string;
  disabled: boolean;
  send: (text: string) => Promise<unknown> | undefined;
  children?: ComponentChildren;
}) {
  const id = useId();
  // A new quote is asked about at once.
  useEffect(() => {
    if (quote !== undefined)
      document.getElementById(id)?.focus({ preventScroll: true });
  }, [quote, id]);
  return (
    <>
      {quote && (
        <div class="side-quote">
          <Markdown text={quote} refs />
          <IconButton label="Remove the quote" onClick={drop}>
            <X />
          </IconButton>
        </div>
      )}
      <StartField
        id={id}
        label={label}
        placeholder={placeholder}
        action={action}
        disabled={disabled}
        onKeyDown={(event) => {
          if (event.key !== "Escape" || quote === undefined) return;
          event.stopPropagation();
          cancel();
        }}
        start={(text) => (text ? send(text) : undefined)}
      >
        {() => children}
      </StartField>
    </>
  );
}

function SideAnswer({
  side,
  asking,
  cancel,
  choose,
}: {
  side: Side;
  asking?: Asking;
  cancel: () => void;
  choose: (id: string) => Promise<void>;
}) {
  const { command } = useApi();
  const { store, load, online, report, setModal } = useHostContext();
  const snapshot = useSnapshots(store, (all) =>
    side.id ? all[side.id] : undefined,
  );
  // Watch a second conversation through the host's existing event stream.
  useEffect(() => {
    if (!side.id || !online) return;
    const unwatch = store.watch(side.id);
    void load(side.id).catch((failure) => report(failure, "inline"));
    return unwatch;
  }, [side.id, online, load, report, store]);
  const session = snapshot?.metadata;
  const pending = snapshot?.pending;
  const running = !!session?.turn_active;
  const act: Act = (kind, fields) => command(kind, session, fields);
  const view = snapshot?.state?.view;
  const nodes = sideNodes(view?.nodes || [], side.request, view?.more);
  // Follows its end as the transcript does, through the same hook.
  const historyKey = `side:${side.id || ""}`;
  const thread = useTranscriptHistory(() => {}, historyKey, nodes);
  return (
    <>
      <div
        class="side-transcript dense"
        data-side={side.key}
        data-history-key={historyKey}
        ref={thread.attachScroller}
      >
        <div ref={thread.attachContent}>
          {!nodes.length && <p class="muted">{side.question}</p>}
          {session && (
            <MessageActions.Provider value={{ report }}>
              <Transcript nodes={nodes} online={online} session={session} />
            </MessageActions.Provider>
          )}
          {session &&
            pending &&
            (pending.kind === "browser" ? (
              <Button
                onClick={() => {
                  void choose(session.id).then(() =>
                    setModal({ type: "browser" }),
                  );
                }}
              >
                Open browser
              </Button>
            ) : (
              <Decision
                key={pending.id}
                pending={pending}
                session={session.id}
                cwd={session.cwd}
                act={act}
                online={online}
                report={report}
              />
            ))}
        </div>
      </div>
      <SideForm
        quote={asking?.quote}
        drop={cancel}
        cancel={cancel}
        label="Follow up on side chat"
        placeholder="Follow up…"
        action={running ? "Queue follow-up" : "Send follow-up"}
        disabled={!session || !online || !!pending}
        send={async (text) => {
          await act("submit", {
            text: asking?.quote ? `${asking.quote}\n\n${text}` : text,
            queue: running,
          });
          cancel();
          await load(session!.id);
        }}
      >
        {running && (
          <IconButton
            label="Stop"
            onClick={() => void act("interrupt").catch(report)}
          >
            <Square />
          </IconButton>
        )}
      </SideForm>
    </>
  );
}

// A conversation's side chats, one shown at a time: each branches from the
// conversation and answers beside it.
export default function SideChats({
  sides,
  shown,
  asking,
  sessions,
  show,
  hide,
  cancel,
  unquote,
  ask,
  detach,
  choose,
}: {
  sides: Side[];
  shown: string;
  asking?: Asking;
  sessions: Session[];
  show: (key: string) => void;
  hide: (key: string) => void;
  cancel: () => void;
  // The new side chat is asked without a quote.
  unquote: () => void;
  ask: (question: string, quote: string) => void;
  detach: (session: Session) => Promise<void>;
  choose: (id: string) => Promise<void>;
}) {
  const { command } = useApi();
  const { online } = useHostContext();
  const merging = useAction();
  const detaching = useAction();
  const side = sides.find((item) => item.key === shown);
  const session = sessions.find((one) => one.id === side?.id);
  const idle = !!session && statusOf(session, online) === "idle";
  const fresh = asking && !asking.to;
  const mark = (item: Side) => {
    const session = sessions.find((one) => one.id === item.id);
    // Until it has a conversation it is starting.
    const state = !item.id ? "working" : session && statusOf(session, online);
    return state === "waiting" ? (
      <CircleAlert role="img" aria-label="Needs your input" />
    ) : (
      state === "working" && <StatusLed state="running" />
    );
  };
  // The shown tab is in view, however many there are.
  const head = useRef<HTMLElement>(null);
  useEffect(() => {
    head.current
      ?.querySelector('[aria-selected="true"]')
      ?.scrollIntoView({ block: "nearest", inline: "nearest" });
  }, [shown, sides.length]);
  return (
    <aside class="side-answer" aria-label="Side chats">
      <header ref={head}>
        <Tabs
          label="Side chats"
          value={shown}
          to={show}
          tabs={[
            ...sides.map((item) => ({
              key: item.key,
              title: item.question,
              label: (
                <>
                  {mark(item)}
                  <span class="truncate">{item.question}</span>
                </>
              ),
            })),
            ...(fresh
              ? [
                  {
                    key: "",
                    label: <span class="truncate">New side chat</span>,
                  },
                ]
              : []),
          ]}
        />
        {session && (
          <Button
            title="Sum it up for the conversation, which answers"
            disabled={!online || merging.busy || !idle}
            onClick={() =>
              void merging.run(async () => {
                await command("side", session, { operation: "merge" });
                hide(shown);
              })
            }
          >
            Merge
          </Button>
        )}
        {session && (
          <IconButton
            label="Open as a conversation of its own"
            disabled={!online || detaching.busy || !idle}
            onClick={() => void detaching.run(() => detach(session))}
          >
            <ArrowUpRight />
          </IconButton>
        )}
        <IconButton label="Hide side chat" onClick={() => hide(shown)}>
          <X />
        </IconButton>
      </header>
      {!!(merging.error || detaching.error) && (
        <LoadError error={merging.error || detaching.error} />
      )}
      {side ? (
        <SideAnswer
          key={side.key}
          side={side}
          asking={asking?.to === shown ? asking : undefined}
          cancel={cancel}
          choose={choose}
        />
      ) : (
        <>
          <p class="side-transcript muted">
            Branches from this conversation: it has read everything said so far.
            The conversation keeps running.
          </p>
          <SideForm
            quote={asking?.quote || ""}
            drop={unquote}
            cancel={cancel}
            label="Ask in a side chat"
            placeholder="Ask about it…"
            action="Ask"
            disabled={!online}
            send={async (text) => ask(text, asking?.quote || "")}
          />
        </>
      )}
    </aside>
  );
}
