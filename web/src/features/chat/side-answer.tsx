import { ExternalLink, X } from "lucide-preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { Act } from "../../shared/types.ts";
import { Button, IconButton } from "../../shared/ui.tsx";
import MessageInput from "../../shared/message-input.tsx";
import Decision from "../../shared/decision.tsx";
import { command } from "../../state/api.ts";
import { useHostContext } from "../../state/host-context.ts";
import { useSnapshots } from "../../state/snapshot-store.ts";
import { MessageRows } from "./message.tsx";
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
  const [sending, setSending] = useState(false);
  const [error, setError] = useState("");
  const transcript = useRef<HTMLDivElement>(null);
  const following = useRef(true);
  const started = useRef(false);
  // Watch a second conversation through the host's existing event stream.
  useEffect(() => {
    if (!side.id || !online) return;
    const unwatch = store.watch(side.id);
    void load(side.id).catch((failure) => report(failure, "inline"));
    return unwatch;
  }, [side.id, online, load, report, store]);
  useEffect(() => {
    if (following.current && transcript.current)
      transcript.current.scrollTop = transcript.current.scrollHeight;
  }, [snapshot]);
  const session = snapshot?.metadata;
  const pending = snapshot?.pending;
  const running = !!session?.turn_active;
  const act: Act = (kind, fields) => command(kind, session, fields);
  const all = snapshot?.state?.view?.blocks || [];
  const first = all.findIndex(
    (block) => block.kind === "user" && block.request_id === side.firstRequest,
  );
  if (first >= 0) started.current = true;
  // Once older turns leave the live window, every retained row is side work.
  const blocks = first >= 0 ? all.slice(first) : started.current ? all : [];
  async function send() {
    if (!draft.trim() || !session || !online || sending || pending) return;
    setSending(true);
    setError("");
    try {
      await act("submit", { text: draft, queue: running });
      setDraft("");
      await load(session.id);
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure));
    } finally {
      setSending(false);
    }
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
        ref={transcript}
        onScroll={(event) => {
          const node = event.currentTarget;
          following.current =
            node.scrollHeight - node.scrollTop - node.clientHeight < 40;
        }}
      >
        {!blocks.length && <p class="muted">{side.question}</p>}
        {session && (
          <MessageActions.Provider value={{ report }}>
            <MessageRows blocks={blocks} online={online} session={session} />
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
      {error && <p role="alert">{error}</p>}
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
