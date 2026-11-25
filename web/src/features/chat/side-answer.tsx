import { CircleAlert, PanelRightClose, Square, X } from "lucide-preact";
import type { ComponentChildren } from "preact";
import { useEffect, useId, useMemo, useRef } from "preact/hooks";
import {
  failure,
  type Act,
  type Node,
  type Session,
} from "../../shared/types.ts";
import { Button, IconButton, Time } from "../../shared/ui.tsx";
import { Menu, MenuItem, MenuSeparator } from "../../shared/menu.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { Tabs } from "../../shared/tabs.tsx";
import { useAction } from "../../shared/use-action.ts";
import { StartField } from "../../shared/start-field.tsx";
import Markdown from "../../shared/markdown-view.tsx";
import Decision from "../../shared/decision.tsx";
import { useApi } from "../../state/api.ts";
import { isAgent, statusOf } from "../../state/attention.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import { useTranscriptHistory } from "../../state/use-transcript-history.ts";
import type { Asking, Side } from "../../app/use-side-conversations.ts";
import { AgentSessions, sessionsById } from "./agent-sessions.tsx";
import { Transcript } from "./node.tsx";
import { MessageActions } from "./message-actions.ts";

// The row of a summing up still under way, one per row the host drew, so
// it keeps its identity while nothing changes.
const summing = new WeakMap<Node, Node>();
const underWay = (node: Node) => {
  let shown = summing.get(node);
  if (!shown) {
    shown = {
      ...node,
      state: "running",
      headline: { ...node.headline!, verb: "Summing up for" },
    };
    summing.set(node, shown);
  }
  return shown;
};

// A side chat's own part of its transcript, from its first question on.
// Where that has left the window, everything held is its own; where it is
// not there yet, nothing is. While it sums itself up, its last summing-up
// row says so.
export function sideNodes(
  nodes: Node[],
  request: string,
  more?: boolean,
  merging?: boolean,
) {
  const first = nodes.findIndex(
    (node) => node.kind === "turn" && node.request_id === request,
  );
  const own =
    !request || first === 0
      ? nodes
      : first > 0
        ? nodes.slice(first)
        : more
          ? nodes
          : [];
  const last = merging
    ? own.findLastIndex((node) => node.origin === "merge" && !!node.headline)
    : -1;
  return last < 0 ? own : own.with(last, underWay(own[last]));
}

// Merge, while it can be; "Summing up…" while the side chat does; when it
// is done and nothing was said since, when. A merge that failed or was
// stopped says why.
export function MergeControl({ session }: { session: Session }) {
  const { command } = useApi();
  const { online, store } = useHostContext();
  const merging = useAction();
  const stop = useSnapshots(store, (all) => {
    const nodes = all[session.id]?.state?.view?.nodes;
    const last = nodes?.findLast((node) => node.kind === "turn");
    return last?.origin === "merge" ? all[session.id]?.state?.stop : undefined;
  });
  const merged = session.side?.merged;
  if (session.merging) return <Button disabled>Summing up…</Button>;
  if (merged && merged.turns === session.turns)
    return (
      <span class="merged-at small muted">
        Merged <Time value={merged.at} />
      </span>
    );
  const why = merging.error
    ? failure(merging.error).message
    : stop && stop.reason !== "completed" && !merging.busy
      ? stop.reason
      : "";
  return (
    <>
      {why && (
        <span class="merge-error small truncate" role="status" title={why}>
          Not merged: {why}
        </span>
      )}
      <Button
        title="Sum it up for the conversation, which answers"
        disabled={
          !online || merging.busy || statusOf(session, online) !== "idle"
        }
        onClick={() =>
          void merging.run(() =>
            command("side", session, { operation: "merge" }),
          )
        }
      >
        Merge
      </Button>
    </>
  );
}

// Removes the side chat for good, once the person has confirmed it.
export function DeleteSideChat({ session }: { session: Session }) {
  const { online, setModal } = useHostContext();
  return (
    <MenuItem
      disabled={!online}
      onClick={() => setModal({ type: "delete", session })}
    >
      Delete side chat…
    </MenuItem>
  );
}

// What can be done with one side chat; `close` closes its tab, where it has
// one.
export function SideChatMenu({
  label,
  session,
  detach,
  close,
}: {
  label: string;
  session: Session;
  detach: (session: Session) => Promise<void>;
  close?: () => void;
}) {
  const { online, report } = useHostContext();
  return (
    <Menu label={label}>
      <MenuItem
        disabled={
          !online || !!session.merging || statusOf(session, online) !== "idle"
        }
        onClick={() => void detach(session).catch(report)}
      >
        Open as its own conversation
      </MenuItem>
      {close && <MenuItem onClick={close}>Close tab</MenuItem>}
      <MenuSeparator />
      <DeleteSideChat session={session} />
    </Menu>
  );
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
  sessions,
  choose,
  asking,
  cancel,
}: {
  side: Side;
  sessions: Session[];
  choose: (id: string) => Promise<void>;
  asking?: Asking;
  cancel: () => void;
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
  const agents = useMemo(
    () =>
      sessions.filter((agent) => agent.parent === side.id && isAgent(agent)),
    [sessions, side.id],
  );
  const running = !!session?.turn_active;
  const act: Act = (kind, fields) => command(kind, session, fields);
  const view = snapshot?.state?.view;
  const nodes = sideNodes(
    view?.nodes || [],
    side.request,
    view?.more,
    session?.merging,
  );
  // Follows its end as the transcript does, through the same hook.
  const historyKey = `side:${side.id || ""}`;
  const agentSessions = useMemo(() => sessionsById(agents), [agents]);
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
            <MessageActions.Provider value={{ report, open: choose }}>
              <AgentSessions.Provider value={agentSessions}>
                <Transcript nodes={nodes} online={online} session={session} />
              </AgentSessions.Provider>
            </MessageActions.Provider>
          )}
          {session &&
            pending &&
            (pending.kind === "browser" ? (
              <Button
                onClick={() =>
                  setModal({
                    type: "browser",
                    handoff: true,
                    session: session.id,
                  })
                }
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
  choose,
  show,
  hide,
  toggle,
  cancel,
  unquote,
  ask,
  detach,
}: {
  sides: Side[];
  shown: string;
  asking?: Asking;
  sessions: Session[];
  choose: (id: string) => Promise<void>;
  show: (key: string) => void;
  hide: (key: string) => void;
  toggle: () => void;
  cancel: () => void;
  // The new side chat is asked without a quote.
  unquote: () => void;
  ask: (question: string, quote: string) => void;
  detach: (session: Session) => Promise<void>;
}) {
  const { online } = useHostContext();
  const side = sides.find((item) => item.key === shown);
  const session = sessions.find((one) => one.id === side?.id);
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
        {session && <MergeControl session={session} />}
        {session && (
          <SideChatMenu
            label="This side chat"
            session={session}
            detach={detach}
            close={() => hide(shown)}
          />
        )}
        <IconButton label="Hide side chats" onClick={toggle}>
          <PanelRightClose />
        </IconButton>
      </header>
      {side ? (
        <SideAnswer
          key={side.key}
          side={side}
          sessions={sessions}
          choose={choose}
          asking={asking?.to === shown ? asking : undefined}
          cancel={cancel}
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
