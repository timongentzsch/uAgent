import type {
  AppModal,
  Catalogue,
  InspectorTarget,
  Session,
} from "../shared/types.ts";
import { ApiContext, hostApi } from "../state/api.ts";
import { useHosts } from "../state/sources.ts";
import HostField from "../shared/host-field.tsx";
import FolderPicker from "../features/files/folder-picker.tsx";
import { folderOf, projectsOf } from "../shared/display.ts";
import { plural } from "../shared/quantities.ts";
import { maxSpaceNameChars } from "../shared/limits.ts";
import { createContext, type ComponentProps } from "preact";
import { useContext } from "preact/hooks";
import {
  Modal,
  Deferred,
  Actions,
  Button,
  EmptyState,
  Input,
  Notice,
} from "../shared/ui.tsx";
import { ImageViewerDialog, type ViewedImage } from "../shared/attachments.tsx";
import { BrowserLoading } from "../features/browser/frame.tsx";
import Settings from "../features/settings/settings.tsx";
import Statistics from "../features/chat/statistics.tsx";
import ConversationActions from "../features/chat/conversation-actions.tsx";
import DeleteConversations from "../features/chat/delete-many.tsx";
import { deleteTitle } from "../shared/delete-many.tsx";
import MoveConversation from "../features/chat/move-conversation.tsx";
import Inspector from "../features/chat/inspector.tsx";
import Raw from "../features/chat/raw.tsx";
import Instructions from "../features/settings/instructions.tsx";
import Tools from "../features/settings/tools.tsx";
import { useSnapshots } from "../state/snapshot-store.ts";
import { useConversation, useHostContext } from "../state/host-context.ts";
import { sessionById } from "../state/attention.ts";
import type { Places } from "./use-places.tsx";

const browserDialog = () => import("../features/browser/browser.tsx");

type Preferences = Pick<
  ComponentProps<typeof Settings>,
  | "install"
  | "setInstall"
  | "update"
  | "updateBlocked"
  | "notificationMode"
  | "setNotificationMode"
  | "notifications"
  | "logout"
>;
export const PreferencesContext = createContext<Preferences | null>(null);

// Each dialog selects from the snapshots only what it shows, while open.
export default function Modals({
  modal,
  inspector,
  setInspector,
  viewed,
  setViewed,
  annotate,
  selected,
  session,
  catalogue,
  compact,
  managementVersion,
  places,
}: {
  modal: AppModal | null;
  inspector: InspectorTarget | null;
  setInspector: (target: InspectorTarget | null) => void;
  viewed: ViewedImage | null;
  setViewed: (image: ViewedImage | null) => void;
  annotate?: (file: File, replaces?: string) => Promise<boolean>;
  selected: string;
  session?: Session;
  catalogue: Catalogue;
  compact: boolean;
  managementVersion: number;
  places: Pick<
    Places,
    | "projects"
    | "folder"
    | "setFolder"
    | "space"
    | "setSpace"
    | "create"
    | "newHost"
    | "setNewHost"
  >;
}) {
  const preferences = useContext(PreferencesContext)!;
  const {
    projects,
    folder,
    setFolder,
    space,
    setSpace,
    create,
    newHost,
    setNewHost,
  } = places;
  const hosts = useHosts();
  const newSource =
    hosts.sources.find((source) => source.id === newHost) || hosts.sources[0];
  const executionOnline = !newHost || newSource.phase === "connected";
  const {
    store,
    online,
    load,
    refresh,
    forget,
    report,
    setModal,
    showContext,
  } = useHostContext();
  const { busy } = useConversation();
  const activities = useSnapshots(store, (all) =>
    inspector ? all[selected]?.state?.activities : undefined,
  );
  // What the page holds, older pages included: fetching again would drop them.
  const held = useSnapshots(store, (all) =>
    modal?.type === "statistics" ? all[modal.session_id] : undefined,
  );
  const http = useSnapshots(store, (all) =>
    modal?.type === "raw" && modal.session
      ? all[modal.session]?.state?.http
      : undefined,
  );
  const mcp = useSnapshots(store, (all) =>
    modal?.type === "settings" ? all[selected]?.state?.mcp : undefined,
  );
  const toolsSessionId = modal?.type === "tools" ? modal.session_id : "";
  const toolsMetadata = useSnapshots(
    store,
    (all) => all[toolsSessionId]?.metadata,
  );
  const toolsPending = useSnapshots(
    store,
    (all) => !!all[toolsSessionId]?.pending,
  );
  const toolsSession =
    toolsMetadata || sessionById(catalogue.sessions, toolsSessionId);
  const close = () => setModal(null);
  return (
    <>
      {(modal?.type === "statistics" ||
        modal?.type === "rename" ||
        modal?.type === "delete") && (
        <Modal
          title={
            modal.type === "rename"
              ? modal.space
                ? "Rename workspace"
                : modal.terminal
                  ? "Rename terminal"
                  : "Rename conversation"
              : modal.type === "delete"
                ? modal.space
                  ? "Delete workspace?"
                  : "Delete conversation?"
                : "unit" in modal && modal.unit
                  ? `${modal.unit} statistics`
                  : "Conversation statistics"
          }
          layout={modal.type === "statistics" ? "sheet" : "content"}
          close={close}
        >
          {modal.type === "statistics" ? (
            <Statistics
              modal={modal}
              held={held}
              loadSnapshot={load}
              online={online}
              showContext={
                modal.session_id === selected ? () => showContext() : undefined
              }
            />
          ) : (
            <ConversationActions
              key={`${modal.type}-${modal.session?.id || modal.space?.path || modal.terminal?.id}`}
              modal={modal}
              close={close}
              online={online}
              changed={async (kind: string, id: string) => {
                // A workspace takes its conversations with it.
                if (kind === "delete")
                  for (const gone of modal.space
                    ? catalogue.sessions.filter(
                        (item) => folderOf(item) === modal.space?.path,
                      )
                    : [{ id }])
                    forget(gone.id);
                await refresh();
              }}
            />
          )}
        </Modal>
      )}
      {modal?.type === "move" && (
        <Modal
          title={
            modal.sessions && modal.sessions.length > 1
              ? `Move ${plural(modal.sessions.length, "conversation")}`
              : `Move “${modal.session.title}”`
          }
          close={close}
        >
          <MoveConversation
            moving={(modal.sessions ?? [modal.session]).map(
              (item) => sessionById(catalogue.sessions, item.id) || item,
            )}
            sessions={catalogue.sessions}
            spaces={catalogue.spaces || []}
            online={online}
            close={close}
          />
        </Modal>
      )}
      {modal?.type === "delete-many" && (
        <Modal
          title={deleteTitle([
            [modal.spaces.length, "workspace"],
            [modal.sessions.length, "conversation"],
          ])}
          close={close}
        >
          <DeleteConversations
            sessions={modal.sessions}
            spaces={modal.spaces}
            online={online}
            close={close}
            done={async (gone) => {
              // A workspace takes its conversations with it.
              for (const item of catalogue.sessions)
                if (gone.includes(item.id) || gone.includes(folderOf(item)))
                  forget(item.id);
              await refresh();
            }}
          />
        </Modal>
      )}
      {inspector && session && (
        <Inspector
          target={inspector}
          items={activities || []}
          cwd={session.cwd || ""}
          session={session}
          online={online}
          report={report}
          close={() => setInspector(null)}
        />
      )}
      {modal?.type === "browser" && (
        <Modal
          title="Browser"
          className="browser-view"
          size="xl"
          layout={compact ? "sheet" : "panel"}
          swipeable={false}
          close={close}
        >
          <Deferred
            load={browserDialog}
            fallback={<BrowserLoading />}
            sessions={catalogue.sessions}
            report={report}
            close={close}
            handoff={modal.handoff}
            sessionId={session?.id}
          />
        </Modal>
      )}
      {modal?.type === "new" && (
        <Modal
          title={
            modal.place === "workspace"
              ? "New workspace"
              : modal.place === "conversation"
                ? "New conversation"
                : "New folder"
          }
          close={close}
        >
          <form onSubmit={create}>
            <HostField
              value={newHost}
              change={(id) => {
                setNewHost(id);
                setFolder("");
              }}
              localOnly={modal.place === "workspace"}
            />
            {!executionOnline && (
              <Button
                type="button"
                disabled={!online}
                busy={
                  newSource.phase === "connecting" ||
                  newSource.phase === "reconnecting"
                }
                onClick={() =>
                  void hosts.action("connect", newHost).catch(report)
                }
              >
                Connect to {newSource.name}
              </Button>
            )}
            {newSource.error && <Notice tone="error">{newSource.error}</Notice>}
            {modal.place === "workspace" ? (
              <>
                <label>
                  Name
                  <Input
                    autoFocus
                    list="new-conversation-spaces"
                    value={space}
                    onInput={(event) => setSpace(event.currentTarget.value)}
                    placeholder="A name, new or existing"
                    required
                    maxLength={maxSpaceNameChars}
                    autoComplete="off"
                  />
                </label>
                <datalist id="new-conversation-spaces">
                  {(newSource.state.catalogue.spaces || []).map((item) => (
                    <option key={item.path} value={item.name} />
                  ))}
                </datalist>
                <p class="muted">
                  A workspace is µAgent&rsquo;s own: a place for work that
                  belongs to no folder, which you can rename and delete.
                </p>
              </>
            ) : (
              <ApiContext.Provider value={hostApi(newHost)}>
                <FolderPicker
                  key={newHost}
                  value={folder}
                  change={setFolder}
                  online={online && executionOnline}
                  projects={projectsOf(newSource.state.catalogue.sessions)}
                />
              </ApiContext.Provider>
            )}
            <Actions>
              <Button
                type="submit"
                variant="primary"
                disabled={
                  busy ||
                  !online ||
                  !executionOnline ||
                  (modal.place !== "workspace" && !folder.trim())
                }
              >
                {modal.place === "workspace"
                  ? "Open workspace"
                  : "Start conversation"}
              </Button>
            </Actions>
          </form>
        </Modal>
      )}
      {modal?.type === "raw" && (
        <Modal
          title={
            modal.context
              ? "Raw context"
              : modal.exchanges !== undefined
                ? "HTTP request/response"
                : modal.id?.startsWith("t-")
                  ? "Tool input/output"
                  : "Full content"
          }
          className="raw-view"
          size="l"
          layout="sheet"
          close={close}
        >
          <Raw
            instructions={() => setModal({ type: "instructions" })}
            id={modal.id}
            session={modal.session}
            value={modal.value}
            exchanges={modal.exchanges}
            latest={http}
            context={modal.context}
            prepare={modal.prepare}
            part={modal.part}
          />
        </Modal>
      )}
      {modal?.type === "instructions" && (
        <Modal title="Instructions" size="l" layout="sheet" close={close}>
          <Instructions
            session={session}
            projects={projects}
            online={online}
            version={managementVersion}
            showRequest={session?.generation ? () => showContext() : undefined}
          />
        </Modal>
      )}
      {modal?.type === "settings" && (
        <Modal
          title="Settings"
          size="l"
          layout="sheet"
          header={false}
          close={close}
        >
          <Settings
            initialSection={modal.section}
            version={managementVersion}
            {...preferences}
            mcp={mcp}
            catalogue={catalogue}
            online={online}
            refresh={refresh}
            selected={selected}
            session={session}
            instructions={() => setModal({ type: "instructions" })}
          />
        </Modal>
      )}
      {modal?.type === "tools" && (
        <Modal title="Tools" size="l" layout="sheet" close={close}>
          {toolsSession ? (
            <Tools
              session={toolsSession}
              online={online}
              busy={!!toolsSession.turn_active || toolsPending}
              changed={() => load(toolsSessionId)}
            />
          ) : (
            <EmptyState>Open a conversation to choose its tools.</EmptyState>
          )}
        </Modal>
      )}
      {viewed && (
        <ImageViewerDialog
          image={viewed}
          close={() => setViewed(null)}
          annotate={annotate}
        />
      )}
    </>
  );
}
