import { ArrowRight, CircleAlert, X } from "lucide-preact";
import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { Act, Session } from "../../shared/types.ts";
import { Button, IconButton, LoadError } from "../../shared/ui.tsx";
import { StatusLed } from "../../shared/connection-status.tsx";
import { Tabs } from "../../shared/tabs.tsx";
import { useAction } from "../../shared/use-action.ts";
import MessageInput from "../../shared/message-input.tsx";
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

// The question, under what it is about. Escape leaves what is being asked.
function SideForm({
  quote,
  drop,
  cancel,
  label,
  placeholder,
  action,
  disabled,
  blocked,
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
  // The question can be written, not yet sent.
  blocked?: boolean;
  send: (text: string) => unknown;
  children?: ComponentChildren;
}) {
  const [draft, setDraft] = useState("");
  const { run, busy, error } = useAction();
  const input = useRef<HTMLTextAreaElement>(null);
  // A new quote is asked about at once.
  useEffect(() => {
    if (quote !== undefined) input.current?.focus({ preventScroll: true });
  }, [quote]);
  const submit = () => {
    if (!draft.trim() || disabled || busy || blocked) return;
    void run(async () => {
      await send(draft);
      setDraft("");
    });
  };
  return (
    <>
      {!!error && <LoadError error={error} />}
      <form
        onSubmit={(event) => {
          event.preventDefault();
          submit();
        }}
        onKeyDown={(event) => {
          if (event.key !== "Escape" || quote === undefined) return;
          event.stopPropagation();
          cancel();
        }}
      >
        {quote && (
          <div class="side-quote">
            <Markdown text={quote} refs />
            <IconButton label="Remove the quote" onClick={drop}>
              <X />
            </IconButton>
          </div>
        )}
        <MessageInput
          inputRef={input}
          submit={submit}
          aria-label={label}
          placeholder={placeholder}
          rows={2}
          value={draft}
          onInput={(event) => setDraft(event.currentTarget.value)}
          disabled={disabled || busy}
        />
        <Button
          type="submit"
          disabled={disabled || busy || !draft.trim() || blocked}
        >
          {action}
        </Button>
        {children}
      </form>
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
  const started = useRef(false);
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
  const all = snapshot?.state?.view?.nodes || [];
  const first = all.findIndex(
    (node) => node.kind === "turn" && node.request_id === side.firstRequest,
  );
  if (first >= 0) started.current = true;
  // Once older turns leave the live window, every retained node is side work.
  const nodes = first >= 0 ? all.slice(first) : started.current ? all : [];
  // Follows its end as the transcript does, through the same hook.
  const historyKey = `side:${side.id || ""}`;
  const thread = useTranscriptHistory(() => {}, historyKey, nodes);
  return (
    <>
      <div
        class="side-transcript dense"
        data-side={side.firstRequest}
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
        disabled={!session || !online}
        blocked={!!pending}
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
          <Button onClick={() => void act("interrupt").catch(report)}>
            Stop
          </Button>
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
  close,
  cancel,
  unquote,
  ask,
  choose,
}: {
  sides: Side[];
  shown: string;
  asking?: Asking;
  sessions: Session[];
  show: (key: string) => void;
  close: (key: string) => void;
  cancel: () => void;
  // The new side chat is asked without a quote.
  unquote: () => void;
  ask: (question: string, quote: string) => void;
  choose: (id: string) => Promise<void>;
}) {
  const { online } = useHostContext();
  const side = sides.find((item) => item.firstRequest === shown);
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
              key: item.firstRequest,
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
        {side?.id && (
          <IconButton
            label="Open side chat"
            onClick={() => void choose(side.id!)}
          >
            <ArrowRight />
          </IconButton>
        )}
        <IconButton label="Close side chat" onClick={() => close(shown)}>
          <X />
        </IconButton>
      </header>
      {side ? (
        <SideAnswer
          key={side.firstRequest}
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
            send={(text) => ask(text, asking?.quote || "")}
          />
        </>
      )}
    </aside>
  );
}
