import type {
  Node,
  Session,
  SessionRef,
  Snapshot,
} from "../../shared/types.ts";
import { useAction } from "../../shared/use-action.ts";
import { useResource } from "../../shared/use-resource.ts";
import {
  LoadError,
  Placeholder,
  Welcome,
  updateChecked,
} from "../../shared/ui.tsx";
import { useHostContext } from "../../state/host-context.ts";
import { isRunning } from "../../state/attention.ts";
import HistoryStart from "./history-start.tsx";
import { prepareHistoryNodes } from "./message.tsx";
import { Transcript } from "./node.tsx";
import "./chat.css";

// The transcript: the conversation's rows, older history and the placeholder
// shown before the snapshot. Used by the chat page.

export { prepareHistoryNodes } from "./message.tsx";

// A request and its reply, the shape a conversation opens with.
const SAMPLE: Node[] = [
  {
    id: "placeholder-request",
    parent: null,
    kind: "turn",
    body: {
      kind: "text",
      text: "A request about this long, asking for one change.",
    },
  },
  {
    id: "placeholder-reply",
    parent: "placeholder-request",
    kind: "message",
    role: "assistant",
    body: {
      kind: "markdown",
      text:
        "A reply opens with what it found and what it changed, in a sentence or two of this length.\n\n" +
        "A second paragraph explains the change and what to check next.",
    },
  },
];

// The transcript before its snapshot: its own rows, drawn from sample text.
function TranscriptPlaceholder({ session }: { session: SessionRef }) {
  return (
    <Placeholder label="Loading conversation…">
      <Transcript nodes={SAMPLE} online={false} session={session} />
    </Placeholder>
  );
}

type ChatProps = {
  attachScroller: (node: HTMLDivElement | null) => void;
  attachContent: (node: HTMLDivElement | null) => void;
  selected: string;
  snapshot?: Snapshot;
  loadError?: unknown;
  nodes: Node[];
  session: Session;
  older: () => Promise<void>;
  preserveWhile: (load: () => Promise<void>) => Promise<void>;
};

function Chat({
  attachScroller,
  attachContent,
  selected,
  snapshot,
  loadError,
  nodes,
  session,
  older,
  preserveWhile,
}: ChatProps) {
  const { online, load: loadSnapshot, report } = useHostContext();
  const olderPage = useAction();
  // Prepares the history once its snapshot arrives, not on every streamed
  // frame.
  const { value: preparedKey } = useResource(
    () =>
      snapshot
        ? prepareHistoryNodes(snapshot.state?.view?.nodes || [])
            .catch(() => {}) // The message renderer shows its own retryable error.
            .then(() => selected)
        : undefined,
    [selected, !!snapshot],
  );
  const prepared = !!snapshot && preparedKey === selected;
  const view = snapshot?.state?.view;
  const retry = () => loadSnapshot(selected).catch(() => {});
  return (
    <div
      class="transcript"
      data-session={selected}
      data-history-key={`chat:${selected}`}
      aria-busy={
        (!snapshot && !loadError) || (snapshot && !prepared) || undefined
      }
      ref={attachScroller}
    >
      {/* A log a listener can browse, silent as it streams: the status
          line above the composer announces a turn's start and end. */}
      <div
        class="transcript-content dense"
        ref={attachContent}
        role="log"
        aria-label="Conversation"
        aria-live="off"
        aria-busy={isRunning(session, online) || undefined}
      >
        <HistoryStart
          view={view}
          online={online}
          loading={olderPage.busy}
          load={() => olderPage.run(() => preserveWhile(older).catch(report))}
        />
        {snapshot?.live_truncated && (
          <p class="retention">
            The live preview exceeded its buffer. Retained history refreshes
            when this turn saves.
          </p>
        )}
        {!!loadError && <LoadError error={loadError} retry={retry} />}
        {/* A failed first load shows only its error. */}
        {(snapshot ? !prepared : !loadError) && (
          <TranscriptPlaceholder session={session} />
        )}
        {snapshot && prepared && nodes.length === 0 && (
          <Welcome
            title="What are we working on?"
            tips={
              session.kind === "panel"
                ? [
                    [
                      "Plan",
                      "The steps, and what each subagent and background agent is doing.",
                    ],
                  ]
                : [
                    [<kbd>/</kbd>, "Lists the commands."],
                    [
                      <kbd>@</kbd>,
                      "Refers to a file you dropped or pasted in.",
                    ],
                    [
                      <kbd>Esc</kbd>,
                      "Stops a turn. Undo under a finished turn puts back the files it changed.",
                    ],
                  ]
            }
          >
            <p>
              {session.kind === "panel"
                ? "The lead agent plans, asks subagents, and hands code changes to background agents. It never edits files itself."
                : "Describe a task."}
            </p>
          </Welcome>
        )}
        {snapshot && prepared && (
          <Transcript nodes={nodes} online={online} session={session} />
        )}
        {session.error && (
          <p class="failure" role="alert">
            {session.error}
          </p>
        )}
        {snapshot?.state?.error && (
          <p class="failure" role="alert">
            {snapshot.state.error}
          </p>
        )}
      </div>
    </div>
  );
}

// The page renders with every key pressed in the composer; the transcript
// only when one of its props changes, which the page keeps stable.
export default updateChecked(Chat);
