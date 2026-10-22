import type {
  AppModal,
  Catalogue,
  InspectorTarget,
  Session,
} from "../shared/types.ts";
import { folderOf } from "../shared/display.ts";
import { maxSpaceNameChars } from "../shared/limits.ts";
import { createContext, type ComponentProps } from "preact";
import { useContext } from "preact/hooks";
import {
  Modal,
  Deferred,
  Actions,
  Button,
  EmptyState,
  Spinner,
  Input,
} from "../shared/ui.tsx";
import { ImageViewerDialog, type ViewedImage } from "../shared/attachments.tsx";
import { StatisticsLoading } from "../shared/statistics-layout.tsx";
import { SettingsLoading } from "../features/settings/loading.tsx";
import { BrowserLoading } from "../features/browser/frame.tsx";
import type Settings from "../features/settings/settings.tsx";
import { useSnapshots } from "../state/snapshot-store.ts";
import { useConversation, useHostContext } from "../state/host-context.ts";
import { isRunning, sessionById } from "../state/attention.ts";
import type { Places } from "./use-places.tsx";
import {
  browserDialog,
  conversationActions,
  instructionsDialog,
  rawDialog,
  inspectorDialog,
  settingsDialog,
  toolsDialog,
  statisticsDialog,
} from "./dialogs.ts";

// What Settings shows and changes of the shell's own state.
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
// Provided by the shell, read only by Settings.
export const PreferencesContext = createContext<Preferences | null>(null);

// The app's dialogs over the shell. Each selects from the snapshots only
// what it shows, while it is open, so a streamed frame re-renders none of
// them unless that changed.
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
  // The places to start in, and the New dialog's fields.
  places: Pick<
    Places,
    "projects" | "folder" | "setFolder" | "space" | "setSpace" | "create"
  >;
}) {
  const preferences = useContext(PreferencesContext)!;
  const { projects, folder, setFolder, space, setSpace, create } = places;
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
  const agents = useSnapshots(store, (all) =>
    inspector ? all[selected]?.state?.agents : undefined,
  );
  const http = useSnapshots(store, (all) =>
    modal?.type === "raw" && modal.session
      ? all[modal.session]?.state?.http
      : undefined,
  );
  const mcp = useSnapshots(store, (all) =>
    modal?.type === "settings" ? all[selected]?.state?.mcp : undefined,
  );
  const running = !!session && isRunning(session, online);
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
            <Deferred
              load={statisticsDialog}
              fallback={<StatisticsLoading unit={modal.unit} />}
              modal={modal}
              loadSnapshot={load}
            />
          ) : (
            <Deferred
              load={conversationActions}
              fallback={
                <Spinner label="Loading conversation actions…" surface />
              }
              key={`${modal.type}-${modal.session?.id || modal.space?.path}`}
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
      {inspector && session && (
        <Deferred
          load={inspectorDialog}
          fallback={null}
          target={inspector}
          items={activities || []}
          agents={agents || []}
          cwd={session.cwd || ""}
          running={running}
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
          size="browser"
          layout={compact ? "sheet" : "panel"}
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
          title={modal.place === "workspace" ? "New workspace" : "New folder"}
          close={close}
        >
          <form onSubmit={create}>
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
                  {(catalogue.spaces || []).map((item) => (
                    <option key={item.path} value={item.name} />
                  ))}
                </datalist>
                <p class="muted">
                  A workspace is µAgent&rsquo;s own: a place for work that
                  belongs to no project, which you can rename and delete.
                </p>
              </>
            ) : (
              <>
                <label>
                  Folder on the host
                  <Input
                    autoFocus
                    list="new-conversation-folders"
                    value={folder}
                    onInput={(event) => setFolder(event.currentTarget.value)}
                    placeholder="/path/to/project"
                    required
                    autoComplete="off"
                  />
                </label>
                <datalist id="new-conversation-folders">
                  {projects
                    .filter(
                      (path) =>
                        !catalogue.spaces?.some((item) => item.path === path),
                    )
                    .map((path) => (
                      <option key={path} value={path} />
                    ))}
                </datalist>
                <p class="muted">
                  A folder is a directory of yours, in Git or not.
                </p>
              </>
            )}
            <Actions>
              <Button
                type="submit"
                variant="primary"
                disabled={busy || !online}
              >
                Start conversation
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
          size="wide"
          layout="sheet"
          close={close}
        >
          <Deferred
            load={rawDialog}
            instructions={() => setModal({ type: "instructions" })}
            fallback={<Spinner label="Loading full body…" surface />}
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
        <Modal title="Instructions" size="wide" layout="sheet" close={close}>
          <Deferred
            load={instructionsDialog}
            fallback={<Spinner label="Loading instructions…" surface />}
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
          className="settings-view"
          layout="sheet"
          header={false}
          close={close}
        >
          <Deferred
            load={settingsDialog}
            ownsDialog
            fallback={<SettingsLoading />}
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
        <Modal title="Tools" size="medium" layout="sheet" close={close}>
          {toolsSession ? (
            <Deferred
              load={toolsDialog}
              fallback={<Spinner label="Loading tools…" surface />}
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
