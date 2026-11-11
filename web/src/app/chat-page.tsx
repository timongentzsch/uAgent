import type { ComponentChildren } from "preact";
import type {
  Activity,
  Catalogue,
  InspectorTarget,
  Node,
  Outgoing,
  Session,
  Snapshot,
  View,
  SlashCommand,
} from "../shared/types.ts";
import { queuedGuidance } from "../shared/message-view.ts";
import { useCallback, useMemo } from "preact/hooks";
import { useApi } from "../state/api.ts";
import { useConversation, useHostContext } from "../state/host-context.ts";
import { Welcome, Button, Actions } from "../shared/ui.tsx";
import { keysOf } from "../shared/shortcuts.ts";
import Composer from "../features/composer/composer.tsx";
import { SideLayout } from "../shared/side-pane.tsx";
import Chat, { prepareHistoryNodes } from "../features/chat/chat.tsx";
import { FindBar, type FindRequest } from "../features/chat/find-bar.tsx";
import { MessageActions } from "../features/chat/message-actions.ts";
import { LiveActivities } from "../state/live-activities.ts";
import { useTranscriptHistory } from "../state/use-transcript-history.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { keepFirst, mergeNodes } from "../shared/tree.ts";
import { maxHistoryNodes } from "../shared/limits.ts";
import { isRunning } from "../state/attention.ts";
import SideAnswer from "../features/chat/side-answer.tsx";

// Empty lists, so an idle context value or prop never changes identity.
const NO_ACTIVITIES: Activity[] = [];
const NO_COMMANDS: SlashCommand[] = [];

// The host's view, then this device's messages it has not confirmed yet.
export function transcriptNodes(
  view: View | undefined,
  outgoing: Outgoing[],
  selected: string,
): Node[] {
  return [
    ...(view?.nodes || []),
    ...outgoing.filter(
      (item) =>
        item.session_id === selected &&
        !view?.nodes?.some((node) => node.request_id === item.request_id),
    ),
  ];
}

export const isBrowsing = (snapshot?: Snapshot) =>
  !!snapshot?.state?.view?.nodes.some(
    (node) =>
      node.state === "running" &&
      !!node.input?.some(
        (part) => part.kind === "link" && part.to === "browser",
      ),
  );

// The one surface that reads the selected conversation's snapshot. It stays
// mounted on the other pages, so the transcript history keeps its place.
export default function ChatPage({
  active,
  historyKey,
  selected,
  session,
  opening,
  catalogue,
  outgoing,
  loadError,
  newPlace,
  updateView,
  choose,
  side,
  closeSide,
  actions,
  setInspector,
  files,
  find,
  closeFind,
}: {
  active: boolean;
  historyKey: string;
  selected: string;
  session?: Session;
  opening: boolean;
  catalogue: Catalogue;
  outgoing: Outgoing[];
  loadError: unknown;
  newPlace: (place: "conversation" | "workspace" | "folder") => void;
  updateView: (id: string, update: (snapshot: Snapshot) => Snapshot) => void;
  choose: (id: string) => Promise<void>;
  side: { question: string; id?: string; firstRequest: string } | null;
  closeSide: () => void;
  actions: MessageActions;
  setInspector: (target: InspectorTarget | null) => void;
  files?: ComponentChildren;
  find: FindRequest | null;
  closeFind: () => void;
}) {
  const { api, routes } = useApi();
  const { store, online, report, load, setModal } = useHostContext();
  const { setFollowing, draft, setDraft, submit } = useConversation();
  const snapshot = useSnapshots(store, (all) => all[selected]);
  const view = snapshot?.state?.view;
  const allNodes = useMemo(
    () => transcriptNodes(view, outgoing, selected),
    [view, outgoing, selected],
  );
  // Queued guidance shows beside the composer, not in the transcript.
  const queued = useMemo(
    () =>
      outgoing.filter(
        (item) => item.session_id === selected && queuedGuidance(item),
      ),
    [outgoing, selected],
  );
  const nodes = useMemo(
    () =>
      queued.length
        ? allNodes.filter((node) => !queuedGuidance(node))
        : allNodes,
    [allNodes, queued],
  );
  // The hook owns scroll restore per session: no pin-on-select here.
  const {
    scroller: transcript,
    attachScroller,
    attachContent,
    jumpToLatest,
    preserveWhile,
    reveal,
    unseen,
  } = useTranscriptHistory(setFollowing, historyKey, nodes);
  // The transcript hook keeps the visible node in place; this only loads data.
  const more = view?.more;
  const before = view?.before;
  const older = useCallback(async () => {
    if (!more || !transcript.current) return;
    const id = selected;
    const value = await api<Snapshot>(routes.session(id, `before=${before}`));
    await prepareHistoryNodes(value.state?.view?.nodes || []);
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
          view: {
            ...value.state.view,
            nodes: keepFirst(
              mergeNodes(value.state.view.nodes, current.state.view.nodes),
              maxHistoryNodes,
            ),
          },
        },
      };
    });
  }, [more, before, selected, updateView, transcript, routes.session, api]);
  const send = useCallback(
    (event: Event, queue?: boolean) => submit(event, jumpToLatest, queue),
    [submit, jumpToLatest],
  );
  // jumpToLatest is idempotent and load() dedupes in flight, so rapid presses
  // cost one pin and one fetch.
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
  const sessions = catalogue.sessions;
  const agents = useMemo(
    () =>
      sessions.filter(
        (item) => item.parent === selected && isRunning(item, online),
      ),
    [sessions, selected, online],
  );
  const open = useCallback((id: string) => void choose(id), [choose]);
  if (!active) return null;

  const stop = snapshot?.state?.stop;
  const stopped =
    stop &&
    stop.reason !== "completed" &&
    !session?.turn_active &&
    !snapshot?.pending &&
    !nodes.at(-1)?.id.startsWith("outgoing-")
      ? stop.reason
      : undefined;
  // A linked conversation the list does not name yet is drawn as a placeholder.
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
      agents={agents}
      choose={open}
    />
  );
  return shown ? (
    <SideLayout
      pane={
        files
          ? { name: "files", label: "Resize the files", content: files }
          : undefined
      }
    >
      {find?.session === selected && (
        <FindBar
          key={`${selected}:${find.at}`}
          request={find}
          view={view}
          nodes={nodes}
          root={transcript}
          older={() => preserveWhile(older)}
          reveal={reveal}
          close={closeFind}
        />
      )}
      {/* Remounted per session: a stale surface's queued scrolls can never
        rewrite the live one. */}
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
            nodes={nodes}
            session={shown}
            older={older}
          />
        </MessageActions.Provider>
      </LiveActivities.Provider>
      {side && (
        <SideAnswer
          key={side.firstRequest}
          side={side}
          choose={choose}
          close={closeSide}
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
          "Folders",
          "A folder’s name in the list opens its overview: what works, what needs you, spend and changes.",
        ],
        [
          <kbd>{keysOf("search").join(" ")}</kbd>,
          "Searches every conversation, command and setting.",
        ],
        [<kbd>?</kbd>, "Lists the keyboard shortcuts."],
      ]}
    >
      <p>
        Start a conversation and choose where it runs, or open one from the
        list.
      </p>
      <Actions>
        <Button
          variant="primary"
          onClick={() => newPlace("conversation")}
          disabled={!online}
        >
          New conversation
        </Button>
        <Button onClick={() => newPlace("workspace")} disabled={!online}>
          New workspace…
        </Button>
        <Button onClick={() => newPlace("folder")} disabled={!online}>
          New folder…
        </Button>
      </Actions>
    </Welcome>
  );
}
