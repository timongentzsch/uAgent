import type {
  Act,
  Activity,
  AppModal,
  Block,
  Catalogue,
  Draft,
  Outgoing,
  Report,
  Session,
  Snapshot,
  View,
} from "../shared/types.ts";
import type { ConnectionPhase } from "../shared/connection-status.tsx";
import { useMemo } from "preact/hooks";
import { api } from "../state/api.ts";
import { Welcome, Deferred, Placeholder, Button } from "../shared/ui.tsx";
import { keysOf } from "../shared/shortcuts.ts";
import Composer from "../features/composer/composer.tsx";
import Board, { CoordinatorLayout } from "../features/coordinator/board.tsx";
import WaitingList from "../features/coordinator/escalations.tsx";
import { threadsOf } from "../state/attention.ts";
import Chat, {
  TranscriptPlaceholder,
  prepareHistoryBlocks,
} from "../features/chat/chat.tsx";
import type { InspectorTarget } from "../features/chat/inspector.tsx";
import { MessageActions } from "../features/chat/message-actions.ts";
import { LiveActivities } from "../state/live-activities.ts";
import { useTranscriptHistory } from "../state/use-transcript-history.ts";
import { prependHistoryPage } from "../state/history-page.ts";
import { useSnapshots, type SnapshotStore } from "../state/snapshot-store.ts";
import { sideAnswer } from "./dialogs.ts";

// One empty list, so an idle context value never changes identity.
const NO_ACTIVITIES: Activity[] = [];

// The transcript's rows: the host's view, then this device's messages the
// host has not confirmed yet.
export function transcriptBlocks(
  view: View | undefined,
  outgoing: Outgoing[],
  selected: string,
): Block[] {
  return [
    ...(view?.blocks || []),
    ...outgoing.filter(
      (item) =>
        item.session_id === selected &&
        !view?.blocks?.some((block) => block.request_id === item.request_id),
    ),
  ];
}

// The conversation's agent is using the browser.
export const isBrowsing = (snapshot?: Snapshot) =>
  !!snapshot?.state?.view?.blocks.some(
    (block) => block.name === "browser" && block.status === "running",
  );

// The one surface that reads the selected conversation's snapshot, so a
// streamed frame re-renders the transcript and composer, not the shell.
// It stays mounted on the other pages: the transcript history keeps its
// place under a page key and resumes when the conversation returns.
export default function ChatPage({
  store,
  active,
  historyKey,
  selected,
  session,
  opening,
  catalogue,
  online,
  connection,
  outgoing,
  loadError,
  draft,
  setDraft,
  upload,
  uploading,
  busy,
  submit,
  act,
  report,
  following,
  setFollowing,
  load,
  updateView,
  choose,
  zoom,
  side,
  closeSide,
  actions,
  setModal,
  setInspector,
  showContext,
}: {
  store: SnapshotStore;
  active: boolean;
  historyKey: string;
  selected: string;
  session?: Session;
  opening: boolean;
  catalogue: Catalogue;
  online: boolean;
  connection: ConnectionPhase;
  outgoing: Outgoing[];
  loadError: unknown;
  draft: Draft;
  setDraft: (value: Draft) => void;
  upload: (files: File[]) => Promise<boolean>;
  uploading: boolean;
  busy: boolean;
  submit: (event: Event, jump: () => void, queue?: boolean) => Promise<void>;
  act: Act;
  report: Report;
  following: boolean;
  setFollowing: (following: boolean) => void;
  load: (id: string) => Promise<Snapshot>;
  updateView: (id: string, update: (snapshot: Snapshot) => Snapshot) => void;
  choose: (id: string) => Promise<void>;
  zoom: number;
  side: { question: string; answer?: string } | null;
  closeSide: () => void;
  actions: MessageActions;
  setModal: (modal: AppModal | null) => void;
  setInspector: (target: InspectorTarget | null) => void;
  showContext: () => void;
}) {
  const snapshot = useSnapshots(store, (all) => all[selected]);
  const view = snapshot?.state?.view;
  const blocks = useMemo(
    () => transcriptBlocks(view, outgoing, selected),
    [view, outgoing, selected],
  );
  // Session-scoped surfaces: switching conversations resumes the saved
  // scroll position (or pins a fresh one); the hook owns this, so there
  // is no pin-on-select here to clobber the restore.
  const {
    scroller: transcript,
    attachScroller,
    attachContent,
    jumpToLatest,
    preserveWhile,
    unseen,
  } = useTranscriptHistory(setFollowing, historyKey, blocks);
  // The shared transcript controller retains the visible block through
  // this bounded page update; this loader only owns data and cursors.
  async function older() {
    if (!view?.more || !transcript.current) return;
    const before = view.before;
    const id = selected;
    const value = await api<Snapshot>(`/api/sessions/${id}?before=${before}`);
    await prepareHistoryBlocks(value.state?.view?.blocks || []);
    updateView(id, (current) => {
      if (
        !value.state?.view ||
        !current.state?.view ||
        current.epoch !== value.epoch ||
        current.metadata.generation !== value.metadata.generation ||
        current.state?.view?.before !== before
      )
        return current;
      return {
        ...current,
        state: {
          ...current.state,
          view: prependHistoryPage(value.state.view, current.state.view),
        },
      };
    });
  }
  if (!active) return null;

  // The last turn stopped short, and nothing new has been sent since: the
  // status line above the composer says so and offers Continue.
  const stop = snapshot?.state?.stop;
  const stopped =
    stop &&
    stop.reason !== "completed" &&
    !session?.turn_active &&
    !snapshot?.pending &&
    !blocks.at(-1)?.id.startsWith("outgoing-")
      ? stop.reason
      : undefined;
  // A coordinator's board, escalations and chat read the same threads.
  const threads =
    session?.kind === "coordinator"
      ? threadsOf(catalogue.sessions, session.cwd || "")
      : undefined;
  // The conversation's composer; before the session is known, the same
  // composer drawn from a sample (see <Placeholder>).
  const composerFor = (item: Session) => (
    <Composer
      session={item}
      commands={catalogue.commands || []}
      snapshot={snapshot}
      online={online}
      connection={connection}
      draft={draft}
      setDraft={setDraft}
      upload={upload}
      uploading={uploading}
      busy={busy}
      submit={(event, queue) => submit(event, jumpToLatest, queue)}
      act={act}
      report={report}
      following={following}
      unseen={unseen}
      // Optimistic: pin to the end synchronously (<1 frame),
      // refresh the snapshot in the background. jumpToLatest
      // is idempotent and load() dedupes in flight, so rapid
      // presses stay a single pin + a single fetch.
      jump={() => {
        jumpToLatest();
        load(selected).catch(report);
      }}
      openInspector={setInspector}
      showStatistics={() =>
        setModal({ type: "statistics", session_id: selected })
      }
      showContext={showContext}
      zoom={zoom}
      openBrowser={() => setModal({ type: "browser", handoff: true })}
      stopped={stopped}
      resume={actions.resume}
      members={threads?.filter((thread) => thread.member)}
    />
  );
  return session ? (
    <CoordinatorLayout
      board={
        threads && <Board threads={threads} online={online} choose={choose} />
      }
    >
      {/* Remount the transcript per session: a stale surface's
        node detaches, so queued scrolls from it can never
        rewrite the live one, and each surface keeps its own
        DOM state (expansion, disclosure, scroll). */}
      <LiveActivities.Provider
        value={snapshot?.state?.activities || NO_ACTIVITIES}
      >
        <MessageActions.Provider value={actions}>
          <Chat
            key={selected}
            attachScroller={attachScroller}
            attachContent={attachContent}
            preserveWhile={preserveWhile}
            selected={selected}
            snapshot={snapshot}
            loadError={loadError}
            blocks={blocks}
            session={session}
            online={online}
            loadSnapshot={load}
            older={older}
            report={report}
          />
        </MessageActions.Provider>
      </LiveActivities.Provider>
      {side && (
        <Deferred
          load={sideAnswer}
          fallback={null}
          question={side.question}
          answer={side.answer}
          close={closeSide}
        />
      )}
      {threads && (
        <WaitingList
          sessions={catalogue.sessions}
          folder={session.cwd || ""}
          paused={snapshot?.state?.paused}
          online={online}
          choose={choose}
          report={report}
        />
      )}
      {composerFor(session)}
    </CoordinatorLayout>
  ) : opening ? (
    // A reload on a conversation: its surfaces, before the
    // catalogue names the session.
    <>
      <div class="transcript">
        <div class="transcript-content">
          <TranscriptPlaceholder session={{ id: selected }} />
        </div>
      </div>
      <Placeholder label="Loading composer…">
        {composerFor({ id: selected, generation: "placeholder" })}
      </Placeholder>
    </>
  ) : (
    <Welcome
      level={1}
      title="What are we working on?"
      tips={[
        "A folder's coordinator runs several conversations for you and hosts a chat with other voices. New conversation opens one too.",
        <>
          {keysOf("palette").map((key) => (
            <kbd key={key}>{key}</kbd>
          ))}{" "}
          finds a conversation, a command or a settings section.
        </>,
        <>
          <kbd>?</kbd> lists the keyboard shortcuts.
        </>,
      ]}
    >
      <p>Pick a folder on your host, or open a conversation from the list.</p>
      <Button
        variant="primary"
        onClick={() => setModal({ type: "new" })}
        disabled={!online}
      >
        New conversation
      </Button>
    </Welcome>
  );
}
