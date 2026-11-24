import type {
  Node,
  Session,
  SessionRef,
  Snapshot,
} from "../../shared/types.ts";
import { useAction } from "../../shared/use-action.ts";
import { useResource } from "../../shared/use-resource.ts";
import { useCallback, useContext } from "preact/hooks";
import { observeResize } from "../../shared/layout.ts";
import {
  LoadError,
  Notice,
  Placeholder,
  Welcome,
  updateChecked,
} from "../../shared/ui.tsx";
import { useHostContext } from "../../state/host-context.ts";
import { isRunning } from "../../state/attention.ts";
import HistoryStart from "./history-start.tsx";
import { prepareHistoryNodes, unprepared } from "./message.tsx";
import { Transcript } from "./node.tsx";
import { MessageActions } from "./message-actions.ts";
import "./chat.css";

export { prepareHistoryNodes } from "./message.tsx";

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
  const { openFile } = useContext(MessageActions);
  const olderPage = useAction({ concurrent: true });
  // Prepared once per snapshot, not on every streamed frame.
  const { value: preparedKey } = useResource(
    () =>
      snapshot
        ? prepareHistoryNodes(snapshot.state?.view?.nodes || [])
            .catch(() => {}) // The message renderer shows its own retryable error.
            .then(() => selected)
        : undefined,
    [selected, !!snapshot],
    // A page shown before draws in its first frame, without a placeholder.
    () =>
      snapshot && !unprepared(snapshot.state?.view?.nodes || []).length
        ? selected
        : undefined,
  );
  const prepared = !!snapshot && preparedKey === selected;
  const view = snapshot?.state?.view;
  const retry = () => loadSnapshot(selected).catch(() => {});
  const mountScroller = useCallback(
    (box: HTMLDivElement | null) => {
      attachScroller(box);
      if (!box) return;
      const pane = box.parentElement!;
      // A classic scrollbar occupies one edge of this borderless scrollport.
      // Keep the controls outside it on the same rails, including in RTL.
      const measure = () => {
        const left = box.clientLeft;
        const right = box.offsetWidth - box.clientWidth - left;
        pane.style.setProperty("--gutter-left", `${left}px`);
        pane.style.setProperty("--gutter-right", `${right}px`);
      };
      measure();
      const stop = observeResize(measure, box);
      return () => {
        stop();
        attachScroller(null);
      };
    },
    [attachScroller],
  );
  return (
    <div
      class="transcript"
      data-session={selected}
      // A reader's place is restored over the conversation, never over its
      // load error: the retry that shows it comes after.
      data-history-key={snapshot ? `chat:${selected}` : undefined}
      aria-busy={
        (!snapshot && !loadError) || (snapshot && !prepared) || undefined
      }
      ref={mountScroller}
      onClick={(event) => {
        const cited = (event.target as Element).closest<HTMLElement>(
          ".file-ref",
        );
        if (cited && openFile) {
          // Safari leaves focus behind on a pointer click. Give the pane its
          // actual opener so closing it returns to this finding.
          cited.focus({ preventScroll: true });
          openFile({ path: cited.dataset.file!, lines: cited.dataset.lines });
        }
      }}
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
          <Notice tone="info">
            The live preview exceeded its buffer. Retained history refreshes
            when this turn saves.
          </Notice>
        )}
        {!!loadError && <LoadError error={loadError} retry={retry} />}
        {/* A failed first load shows only its error. */}
        {(snapshot ? !prepared : !loadError) && (
          <TranscriptPlaceholder session={session} />
        )}
        {snapshot && prepared && nodes.length === 0 && (
          <Welcome
            title="What are we working on?"
            tips={[
              [<kbd>/</kbd>, "Lists the commands."],
              [<kbd>@</kbd>, "Refers to a file you dropped or pasted in."],
              [
                <kbd>Esc</kbd>,
                "Stops a turn. The file count under a finished turn opens its changes, where Undo puts them back.",
              ],
            ]}
          >
            <p>Describe a task.</p>
          </Welcome>
        )}
        {snapshot && prepared && (
          <Transcript nodes={nodes} online={online} session={session} />
        )}
        {session.error && <Notice tone="error">{session.error}</Notice>}
        {snapshot?.state?.error && (
          <Notice tone="error">{snapshot.state.error}</Notice>
        )}
      </div>
    </div>
  );
}

// The page renders with every key pressed in the composer; the transcript
// only when one of its props changes, which the page keeps stable.
export default updateChecked(Chat);
