import type { ComponentChildren } from "preact";
import type {
  Activity,
  Block,
  Catalogue,
  Draft,
  InspectorTarget,
  Outgoing,
  Session,
  Snapshot,
  View,
  UserBlock,
} from "../shared/types.ts";
import { queuedGuidance } from "../shared/message-view.ts";
import { useMemo } from "preact/hooks";
import { api, routes } from "../state/api.ts";
import { useConversation, useHostContext } from "../state/host-context.ts";
import {
  Welcome,
  Deferred,
  Placeholder,
  Button,
  Actions,
} from "../shared/ui.tsx";
import { keysOf } from "../shared/shortcuts.ts";
import Composer from "../features/composer/composer.tsx";
import Board from "../features/panel/board.tsx";
import { SideLayout } from "../shared/side-pane.tsx";
import { usePhone } from "../shared/layout.ts";
import WaitingList from "../shared/escalations.tsx";
import Chat, {
  TranscriptPlaceholder,
  prepareHistoryBlocks,
} from "../features/chat/chat.tsx";
import { MessageActions } from "../features/chat/message-actions.ts";
import { LiveActivities } from "../state/live-activities.ts";
import { useTranscriptHistory } from "../state/use-transcript-history.ts";
import { prependHistoryPage } from "../state/history-page.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
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
  draft,
  setDraft,
  menu,
  newPlace,
  addMember,
  submit,
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
  draft: Draft;
  setDraft: (value: Draft) => void;
  // A panel's roster: a row's own actions, and asking for another member.
  menu: (item: Session) => ComponentChildren;
  // Asks for a workspace's name or a folder's path, to start a conversation.
  newPlace: (place: "workspace" | "folder") => void;
  addMember: () => void;
  submit: (event: Event, jump: () => void, queue?: boolean) => Promise<void>;
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
  const { setFollowing } = useConversation();
  const phone = usePhone();
  const snapshot = useSnapshots(store, (all) => all[selected]);
  const view = snapshot?.state?.view;
  const allBlocks = useMemo(
    () => transcriptBlocks(view, outgoing, selected),
    [view, outgoing, selected],
  );
  const queued = allBlocks.filter(
    (block): block is UserBlock =>
      block.kind === "user" && queuedGuidance(block),
  );
  const blocks = useMemo(
    () =>
      allBlocks.filter(
        (block) => block.kind !== "user" || !queuedGuidance(block),
      ),
    [allBlocks],
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
  // A panel's board, escalations and chat read the same threads.
  // The conversation's composer; before the session is known, the same
  // composer drawn from a sample (see <Placeholder>).
  const composerFor = (item: Session) => (
    <Composer
      key={item.id}
      session={item}
      queued={queued}
      recall={actions.recall}
      insert={actions.insert}
      commands={catalogue.commands || []}
      snapshot={snapshot}
      draft={draft}
      setDraft={setDraft}
      submit={(event, queue) => submit(event, jumpToLatest, queue)}
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
      openBrowser={() => setModal({ type: "browser", handoff: true })}
      stopped={stopped}
      resume={actions.resume}
      members={threads?.filter((thread) => thread.member)}
    />
  );
  return session ? (
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
                    add={addMember}
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
            session={session}
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
          folder={session.cwd || ""}
          paused={snapshot?.state?.paused}
          choose={choose}
        />
      )}
      {composerFor(session)}
    </SideLayout>
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
        [
          "Panel",
          "Runs several conversations in a workspace or folder for you and hosts a chat with other voices. A place’s name in the list opens its panel.",
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
