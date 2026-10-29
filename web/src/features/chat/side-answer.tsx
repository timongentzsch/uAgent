import { ExternalLink, X } from "lucide-preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { Act } from "../../shared/types.ts";
import { Button, IconButton } from "../../shared/ui.tsx";
import { useAction } from "../../shared/use-action.ts";
import { failure } from "../../shared/types.ts";
import MessageInput from "../../shared/message-input.tsx";
import Decision from "../../shared/decision.tsx";
import { command } from "../../state/api.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import { useTranscriptHistory } from "../../state/use-transcript-history.ts";
import { Transcript } from "./node.tsx";
import { MessageActions } from "./message-actions.ts";

// An ordinary conversation runtime, kept beside the parent's composer.
export default function SideAnswer({
  side,
  choose,
  close,
}: {
  side: { question: string; id?: string; firstRequest: string };
  choose: (id: string) => Promise<void>;
  close: () => void;
}) {
  const { store, load, online, report, setModal } = useHostContext();
  const snapshot = useSnapshots(store, (all) =>
    side.id ? all[side.id] : undefined,
  );
  const [draft, setDraft] = useState("");
  const { run, busy: sending, error } = useAction();
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
  // Follows its end like the transcript (same hook): a reader who scrolls
  // up stays put, and scrolling back down follows again.
  const historyKey = `side:${side.id || ""}`;
  const thread = useTranscriptHistory(() => {}, historyKey, nodes);
  async function send() {
    if (!draft.trim() || !session || !online || sending || pending) return;
    await run(async () => {
      await act("submit", { text: draft, queue: running });
      setDraft("");
      await load(session.id);
    });
  }
  return (
    <aside class="side-answer" aria-label="Side conversation">
      <header>
        <strong>btw</strong>
        <span class="muted">
          {!session
            ? "Starting…"
            : pending
              ? "Needs you"
              : running
                ? "Working…"
                : "Ready"}
        </span>
        {side.id && (
          <IconButton
            label="Open side conversation"
            onClick={() => void choose(side.id!)}
          >
            <ExternalLink />
          </IconButton>
        )}
        <IconButton label="Dismiss side conversation" onClick={close}>
          <X />
        </IconButton>
      </header>
      <div
        class="side-transcript dense"
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
      {!!error && <p role="alert">{failure(error).message}</p>}
      <form
        onSubmit={(event) => {
          event.preventDefault();
          void send();
        }}
      >
        <MessageInput
          submit={() => void send()}
          aria-label="Follow up on side conversation"
          placeholder="Follow up…"
          rows={2}
          value={draft}
          onInput={(event) => setDraft(event.currentTarget.value)}
          disabled={!session || !online || sending}
        />
        <Button
          type="submit"
          disabled={
            !session || !online || sending || !draft.trim() || !!pending
          }
        >
          {running ? "Queue follow-up" : "Send follow-up"}
        </Button>
        {running && (
          <Button onClick={() => void act("interrupt").catch(report)}>
            Stop
          </Button>
        )}
      </form>
    </aside>
  );
}
