import type { ComponentChildren } from "preact";
import type {
  Activity,
  Block,
  Catalogue,
  InspectorTarget,
  Outgoing,
  Session,
  Snapshot,
  View,
  SlashCommand,
} from "../shared/types.ts";
import { queuedGuidance } from "../shared/message-view.ts";
import { useCallback, useMemo } from "preact/hooks";
import { api, routes } from "../state/api.ts";
import { useConversation, useHostContext } from "../state/host-context.ts";
import { Welcome, Deferred, Button, Actions } from "../shared/ui.tsx";
import { keysOf } from "../shared/shortcuts.ts";
import Composer from "../features/composer/composer.tsx";
import Board from "../features/panel/board.tsx";
import { SideLayout } from "../shared/side-pane.tsx";
import { usePhone } from "../shared/layout.ts";
import WaitingList from "../shared/escalations.tsx";
import Chat, { prepareHistoryBlocks } from "../features/chat/chat.tsx";
import { MessageActions } from "../features/chat/message-actions.ts";
import { LiveActivities } from "../state/live-activities.ts";
import { useTranscriptHistory } from "../state/use-transcript-history.ts";
import { prependHistoryPage } from "../state/history-page.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { sideAnswer } from "./dialogs.ts";

// Empty lists, so an idle context value or prop never changes identity.
const NO_ACTIVITIES: Activity[] = [];
const NO_COMMANDS: SlashCommand[] = [];

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
        !view?.blocks?.some(
          (block) =>
            "request_id" in block && block.request_id === item.request_id,
        ),
    ),
  ];
}

// The conversation's agent is using the browser.
export const isBrowsing = (snapshot?: Snapshot) =>
  !!snapshot?.state?.view?.blocks.some(
    (block) =>
      block.kind === "tool_result" &&
      block.name === "browser" &&
      block.status === "running",
  );

// The one surface that reads the selected conversation's snapshot, so a
// streamed frame re-renders the transcript and composer, not the shell.
// It stays mounted on the other pages: the transcript history keeps its
// place under a page key and resumes when the conversation returns.
export default function ChatPage({
  active,
  historyKey,
  selected,
  session,
  threads,
  opening,
  catalogue,
  outgoing,
  loadError,
  menu,
  newPlace,
  updateView,
  choose,
  side,
  closeSide,
  actions,
  setInspector,
  files,
}: {
  active: boolean;
  historyKey: string;
  selected: string;
  session?: Session;
  // The open panel's threads.
  threads?: Session[];
  opening: boolean;
  catalogue: Catalogue;
  outgoing: Outgoing[];
  loadError: unknown;
  // A board row's own actions.
  menu: (item: Session) => ComponentChildren;
  // Asks for a workspace's name or a folder's path, to start a conversation.
  newPlace: (place: "workspace" | "folder") => void;
  updateView: (id: string, update: (snapshot: Snapshot) => Snapshot) => void;
  choose: (id: string) => Promise<void>;
  side: { question: string; id?: string; firstRequest: string } | null;
  closeSide: () => void;
  actions: MessageActions;
  setInspector: (target: InspectorTarget | null) => void;
  // The files pane, when it is open beside the chat; it takes the board's
  // place while it is.
  files?: ComponentChildren;
}) {
  const { store, online, report, load, setModal } = useHostContext();
  const { setFollowing, draft, setDraft, submit } = useConversation();
  const phone = usePhone();
  const snapshot = useSnapshots(store, (all) => all[selected]);
  const view = snapshot?.state?.view;
  const allBlocks = useMemo(
    () => transcriptBlocks(view, outgoing, selected),
    [view, outgoing, selected],
  );
  // This device's guidance waiting on the turn: beside the composer, not in
  // the transcript, and unchanged by the frames streamed meanwhile.
  const queued = useMemo(
    () =>
      outgoing.filter(
        (item) => item.session_id === selected && queuedGuidance(item),
      ),
    [outgoing, selected],
  );
  const blocks = useMemo(
    () =>
      queued.length
        ? allBlocks.filter(
            (block) => block.kind !== "user" || !queuedGuidance(block),
          )
        : allBlocks,
    [allBlocks, queued],
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
  const more = view?.more;
  const before = view?.before;
  const older = useCallback(async () => {
    if (!more || !transcript.current) return;
    const id = selected;
    const value = await api<Snapshot>(routes.session(id, `before=${before}`));
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
  }, [more, before, selected, updateView, transcript]);
  // What the composer is given keeps its identity across streamed frames.
  const send = useCallback(
    (event: Event, queue?: boolean) => submit(event, jumpToLatest, queue),
    [submit, jumpToLatest],
  );
  // Optimistic: pin to the end synchronously (<1 frame), refresh the
  // snapshot in the background. jumpToLatest is idempotent and load()
  // dedupes in flight, so rapid presses stay a single pin + a single fetch.
  const jump = useCallback(() => {
    jumpToLatest();
    load(selected).catch(report);
  }, [jumpToLatest, load, selected, report]);
  const showStatistics = useCallback(
    () => setModal({ type: "statistics", session_id: selected }),
    [setModal, selected],
  );
  const openBrowser = useCallback(
    () => setModal({ type: "browser", handoff: true }),
    [setModal],
  );
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
  // A panel's board, escalations and chat read the same threads.
  // A conversation opened by its link before the list names it is drawn the
  // same way: its transcript from samples until it loads, its composer inert
  // until the page is online.
  const shown =
    session ?? (opening ? { id: selected, generation: "placeholder" } : null);
  const composerFor = (item: Session) => (
    <Composer
      key={item.id}
      session={item}
      queued={queued}
      recall={actions.recall}
      insert={actions.insert}
      commands={catalogue.commands || NO_COMMANDS}
      state={snapshot?.state}
      pending={snapshot?.pending}
      draft={draft}
      setDraft={setDraft}
      submit={send}
      unseen={unseen}
      jump={jump}
      openInspector={setInspector}
      showStatistics={showStatistics}
      openBrowser={openBrowser}
      stopped={stopped}
      resume={actions.resume}
    />
  );
  return shown ? (
    <SideLayout
      pane={
        files
          ? { name: "files", label: "Resize the files", content: files }
          : threads && !phone
            ? {
                name: "board",
                label: "Resize the board",
                content: (
                  <Board
                    threads={threads}
                    choose={choose}
                    menu={menu}
                    state={snapshot?.state}
                    open={setInspector}
                  />
                ),
              }
            : undefined
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
            session={shown}
            older={older}
          />
        </MessageActions.Provider>
      </LiveActivities.Provider>
      {side && (
        <Deferred
          load={sideAnswer}
          fallback={null}
          key={side.firstRequest}
          side={side}
          choose={choose}
          close={closeSide}
        />
      )}
      {threads && (
        <WaitingList
          sessions={catalogue.sessions}
          folder={shown.cwd || ""}
          paused={snapshot?.state?.paused}
          choose={choose}
        />
      )}
      {composerFor(shown)}
    </SideLayout>
  ) : (
    <Welcome
      level={1}
      title="What are we working on?"
      tips={[
        [
          "Panel",
          "Runs several conversations in a workspace or folder for you. A place’s name in the list opens its panel.",
        ],
        [
          <kbd>{keysOf("palette").join(" ")}</kbd>,
          "Finds a conversation, a command or a settings section.",
        ],
        [<kbd>?</kbd>, "Lists the keyboard shortcuts."],
      ]}
    >
      <p>
        Start in a workspace or a folder on your host, or open a conversation
        from the list.
      </p>
      <Actions>
        <Button onClick={() => newPlace("workspace")} disabled={!online}>
          New workspace…
        </Button>
        <Button
          variant="primary"
          onClick={() => newPlace("folder")}
          disabled={!online}
        >
          New folder…
        </Button>
      </Actions>
    </Welcome>
  );
}
