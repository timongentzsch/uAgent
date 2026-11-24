import type { ComponentChildren } from "preact";
import type {
  Activity,
  Catalogue,
  InspectorTarget,
  Node,
  Outgoing,
  Session,
  Snapshot,
  Space,
  View,
  SlashCommand,
} from "../shared/types.ts";
import { queuedGuidance } from "../shared/message-view.ts";
import { useCallback, useMemo, useState } from "preact/hooks";
import { useApi } from "../state/api.ts";
import { useConversation, useHostContext } from "../state/host-context.ts";
import { Button, Mark } from "../shared/ui.tsx";
import {
  HomeLists,
  type Redirect,
  Suggestion,
} from "../features/filing/filing.tsx";
import { ChevronDown, Gauge, LayoutGrid } from "lucide-preact";
import "../shared/model-control.css";
import { StartField } from "../shared/start-field.tsx";
import { SheetButton } from "../shared/sheet.tsx";
import ModelPicker from "../shared/model-picker.tsx";
import { FreeStart } from "./free-start.tsx";
import type { ModelChoice } from "./use-places.tsx";
import { keysOf } from "../shared/shortcuts.ts";
import Composer from "../features/composer/composer.tsx";
import { SideLayout } from "../shared/side-pane.tsx";
import Chat, { prepareHistoryNodes } from "../features/chat/chat.tsx";
import { FindBar, type FindRequest } from "../features/chat/find-bar.tsx";
import {
  AgentParticipants,
  useParticipants,
} from "../features/chat/agent-sender.tsx";
import { MessageActions } from "../features/chat/message-actions.ts";
import { LiveActivities } from "../state/live-activities.ts";
import { useTranscriptHistory } from "../state/use-transcript-history.ts";
import { useSnapshots } from "../state/snapshot-store.ts";
import { keepFirst, mergeNodes } from "../shared/tree.ts";
import { maxHistoryNodes } from "../shared/limits.ts";
import { useLatest } from "../shared/use-latest.ts";
import SideChats from "../features/chat/side-answer.tsx";
import { SelectionActions } from "../features/chat/selection-actions.tsx";
import type { useSideConversations } from "./use-side-conversations.ts";

// Empty lists, so an idle context value or prop never changes identity.
const NO_ACTIVITIES: Activity[] = [];
const NO_COMMANDS: SlashCommand[] = [];
const NO_SPACES: Space[] = [];
export const startFieldId = "start-field";

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
  start,
  settings,
  openList,
  redirect,
  updateView,
  choose,
  side,
  sideColumn,
  quote,
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
  newPlace: (
    place: "conversation" | "workspace" | "folder",
    text?: string,
  ) => void;
  start: (text: string, model?: ModelChoice) => Promise<unknown>;
  settings: () => void;
  // On a narrow screen: opens the list of every conversation.
  openList?: () => void;
  redirect: Redirect;
  updateView: (id: string, update: (snapshot: Snapshot) => Snapshot) => void;
  choose: (id: string) => Promise<void>;
  side: ReturnType<typeof useSideConversations>;
  // The side chats have a column beside the chat; else they sit under it.
  sideColumn: boolean;
  // Adds a quote to the conversation's draft.
  quote?: (text: string) => void;
  actions: MessageActions;
  setInspector: (target: InspectorTarget | null) => void;
  files?: ComponentChildren;
  find: FindRequest | null;
  closeFind: () => void;
}) {
  const { api, routes } = useApi();
  const { store, online, report, load, setModal } = useHostContext();
  // The model picked on the home screen for the conversation it starts.
  const [model, setModel] = useState<ModelChoice>();
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
    () => sessions.filter((item) => item.parent === selected),
    [sessions, selected],
  );
  const participants = useParticipants(nodes, agents);
  // Its side chats that have a conversation, for the Activity list: a row
  // shows the tab where they have a column, and the page where not.
  const sides = side.sides;
  const latest = useLatest({ side, sideColumn, quote });
  const sideRows = useMemo(() => {
    const asked = (id: string) =>
      latest.current.side.sides.find((item) => item.id === id);
    return {
      list: sides.flatMap(
        (item) => sessions.find((session) => session.id === item.id) ?? [],
      ),
      open: (id: string) => {
        const { side, sideColumn } = latest.current;
        const item = asked(id);
        if (sideColumn && item) side.show(item.firstRequest);
        else void choose(id);
      },
      remove: (id: string) => {
        const item = asked(id);
        if (item) latest.current.side.close(item.firstRequest);
      },
    };
  }, [sides, sessions, choose, latest]);
  // The composer keeps its place above the input for a selection's actions,
  // so Jump to latest stays clear of them.
  const selecting = !!quote;
  const sideAsked = !!side.asking;
  const selection = useMemo(
    () =>
      selecting && (
        <SelectionActions
          asking={sideAsked}
          ask={(quote, to) => latest.current.side.quote(quote, to)}
          add={(text) => latest.current.quote?.(text)}
        />
      ),
    [selecting, sideAsked, latest],
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
  // No room for the side chats' column: the input asks, and a side chat is
  // a page of its own.
  const asking =
    !sideColumn && side.asking && !side.asking.to ? side.asking : undefined;
  const askSide = (event: Event) => {
    event.preventDefault();
    const question = draft.text.trim();
    if (!question || !session || !asking) return;
    setDraft({ ...draft, text: "" });
    side.cancel();
    void side.ask(session, question, asking.quote).then((id) => {
      if (id) return choose(id);
    });
  };
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
      sides={sideRows}
      above={selection}
      submit={asking ? askSide : send}
      asking={asking && { quote: asking.quote, cancel: side.cancel }}
      unseen={unseen}
      jump={jump}
      openInspector={setInspector}
      showStatistics={showStatistics}
      openBrowser={openBrowser}
      stopped={stopped}
      resume={actions.resume}
      agents={agents}
      agentNames={participants.names}
      choose={open}
    />
  );
  // Its only message, while its first turn has changed no file yet.
  const asked = nodes.filter((node) => node.kind === "turn");
  const first =
    session?.turn_active &&
    asked.length === 1 &&
    !nodes.some((node) => node.kind === "work" && node.result?.edited)
      ? asked[0].body?.text || ""
      : "";
  const sideChats = !!session && (side.sides.length > 0 || side.asking) && (
    <SideChats
      sides={side.sides}
      shown={side.shown}
      asking={side.asking}
      sessions={catalogue.sessions}
      show={side.show}
      close={side.close}
      cancel={side.cancel}
      unquote={() => side.quote("")}
      ask={(question, quote) => {
        void side.ask(session, question, quote);
        side.cancel();
      }}
      choose={choose}
    />
  );
  return shown ? (
    <SideLayout
      panes={[
        sideColumn && sideChats
          ? {
              name: "side",
              label: "Resize the side chats",
              content: sideChats,
            }
          : undefined,
        files
          ? { name: "files", label: "Resize the files", content: files }
          : undefined,
      ]}
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
          <AgentParticipants.Provider value={participants}>
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
          </AgentParticipants.Provider>
        </MessageActions.Provider>
      </LiveActivities.Provider>
      {session && (
        <Suggestion
          key={session.id}
          session={session}
          sessions={catalogue.sessions}
          spaces={catalogue.spaces || NO_SPACES}
          first={first}
          redirect={redirect}
        />
      )}
      {composerFor(shown)}
    </SideLayout>
  ) : (
    <div class="home">
      {/* raw: the home screen's heading, a block and not a bar. */}
      <header class="home-head">
        <Mark className="cursor-mark" />
        <h1>What are we working on?</h1>
      </header>
      <FreeStart online={online} settings={settings} />
      <div class="home-start">
        <StartField
          id={startFieldId}
          label="Start a conversation"
          placeholder="Ask, or describe the work…"
          disabled={!online}
          // A phone would open its keyboard over the lists.
          autoFocus={!openList}
          start={(text) => start(text, model)}
        >
          {(text) => (
            <>
              <Button
                class="start-place"
                title="It starts in Scratch, and µAgent suggests where it belongs once it knows. Choose another place here."
                disabled={!online}
                onClick={() => newPlace("conversation", text)}
              >
                <LayoutGrid />
                <span class="truncate">Scratch</span>
                <ChevronDown />
              </Button>
              <SheetButton
                label="Model for this conversation"
                title="Model and effort"
                className="model-control"
                buttonClass="model-selector"
                disabled={!online}
                trigger={
                  <>
                    <Gauge />
                    <span class="truncate">
                      {model?.label || "Default model"}
                    </span>
                    <ChevronDown />
                  </>
                }
              >
                {(close) => (
                  <ModelPicker
                    session={{ id: "new-conversation", generation: "" }}
                    selection={model?.label}
                    save={(label, picked, effort) =>
                      setModel({ label, model: picked, effort })
                    }
                    close={close}
                    online={online}
                    running={false}
                  />
                )}
              </SheetButton>
            </>
          )}
        </StartField>
      </div>
      <HomeLists
        sessions={catalogue.sessions}
        spaces={catalogue.spaces || NO_SPACES}
        choose={choose}
        all={openList}
      />
      <dl class="tips">
        {(
          [
            [
              "Folders",
              "A folder’s name in the list opens its overview: what works, what needs you, spend and changes.",
            ],
            [
              <kbd>{keysOf("search").join(" ")}</kbd>,
              "Searches every conversation, command and setting.",
            ],
            [<kbd>?</kbd>, "Lists the keyboard shortcuts."],
          ] as const
        ).map(([lead, text], index) => (
          <div key={index}>
            <dt>{lead}</dt>
            <dd>{text}</dd>
          </div>
        ))}
      </dl>
    </div>
  );
}
