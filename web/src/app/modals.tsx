import type {
  AppModal,
  Catalogue,
  Report,
  Session,
  Snapshot,
} from "../shared/types.ts";
import { folderOf } from "../shared/folder-label.tsx";
import type { ComponentProps, JSX } from "preact";
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
import type { InspectorTarget } from "../features/chat/inspector.tsx";
import {
  useSnapshots,
  type SnapshotStore,
  type Snapshots,
} from "../state/snapshot-store.ts";
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
  | "theme"
  | "setTheme"
  | "motion"
  | "setMotion"
  | "timePrefs"
  | "setTimePrefs"
  | "zoom"
  | "setZoom"
  | "install"
  | "setInstall"
  | "update"
  | "updateBlocked"
  | "notificationMode"
  | "setNotificationMode"
  | "notifications"
  | "logout"
>;

const NONE: Snapshots = {};

// The app's dialogs over the shell. They read snapshots only while one is
// open, so a closed set costs a streamed frame nothing.
export default function Modals({
  store,
  modal,
  setModal,
  inspector,
  setInspector,
  viewed,
  setViewed,
  annotate,
  selected,
  session,
  catalogue,
  online,
  compact,
  load,
  refresh,
  forget,
  report,
  managementVersion,
  projects,
  showContext,
  folder,
  setFolder,
  space,
  setSpace,
  create,
  busy,
  preferences,
}: {
  store: SnapshotStore;
  modal: AppModal | null;
  setModal: (modal: AppModal | null) => void;
  inspector: InspectorTarget | null;
  setInspector: (target: InspectorTarget | null) => void;
  viewed: ViewedImage | null;
  setViewed: (image: ViewedImage | null) => void;
  annotate?: (file: File, replaces?: string) => Promise<boolean>;
  selected: string;
  session?: Session;
  catalogue: Catalogue;
  online: boolean;
  compact: boolean;
  load: (id: string) => Promise<Snapshot>;
  refresh: () => Promise<void>;
  forget: (id: string) => void;
  report: Report;
  managementVersion: number;
  projects: string[];
  showContext: () => void;
  folder: string;
  setFolder: (folder: string) => void;
  space: string;
  setSpace: (space: string) => void;
  create: (
    event: JSX.TargetedSubmitEvent<HTMLFormElement> | null,
    panel?: boolean,
  ) => void;
  busy: boolean;
  preferences: Preferences;
}) {
  const snapshots = useSnapshots(store, (all) =>
    modal || inspector ? all : NONE,
  );
  const snapshot = snapshots[selected];
  const running = online && !!session?.turn_active;
  const toolsSessionId = modal?.type === "tools" ? modal.session_id : "";
  const toolsSnapshot = snapshots[toolsSessionId];
  const toolsSession =
    toolsSnapshot?.metadata ||
    catalogue.sessions.find((item) => item.id === toolsSessionId);
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
                ? "Rename space"
                : "Rename conversation"
              : modal.type === "delete"
                ? modal.space
                  ? "Delete space?"
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
                // A space takes its conversations with it.
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
          items={snapshot?.state?.activities || []}
          agents={snapshot?.state?.agents || []}
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
          />
        </Modal>
      )}
      {modal?.type === "new" && (
        <Modal title="New conversation" close={close}>
          <form onSubmit={create}>
            {/* One of the two: writing in one empties the other. */}
            <label>
              In a space
              <Input
                autoFocus
                list="new-conversation-spaces"
                value={space}
                onInput={(event) => {
                  setSpace(event.currentTarget.value);
                  setFolder("");
                }}
                placeholder="A name, new or existing"
                required={!folder}
                maxLength={64}
                autoComplete="off"
              />
            </label>
            <datalist id="new-conversation-spaces">
              {(catalogue.spaces || []).map((item) => (
                <option key={item.path} value={item.name} />
              ))}
            </datalist>
            <label>
              Or in a folder on the host
              <Input
                list="new-conversation-folders"
                value={folder}
                onInput={(event) => {
                  setFolder(event.currentTarget.value);
                  setSpace("");
                }}
                placeholder="/path/to/project"
                required={!space}
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
              A space is µAgent&rsquo;s own: a place for work that belongs to no
              project, which you can rename and delete. A folder is a directory
              of yours, in Git or not. Either holds several conversations, and a
              panel that runs them for you and hosts a chat with other voices.
            </p>
            <Actions>
              {/* Not a submit: Enter in the field starts a conversation. */}
              <Button
                disabled={busy || !online}
                onClick={(event) =>
                  event.currentTarget.form?.reportValidity() &&
                  create(null, true)
                }
              >
                Open its panel
              </Button>
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
            latest={
              modal.session ? snapshots[modal.session]?.state?.http : undefined
            }
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
            state={snapshot?.state}
            projects={projects}
            online={online}
            version={managementVersion}
            showRequest={session?.generation ? showContext : undefined}
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
            installed={
              matchMedia("(display-mode: standalone)").matches ||
              ("standalone" in navigator && navigator.standalone === true)
            }
            ios={
              /iPhone|iPad/.test(navigator.userAgent) ||
              (navigator.platform === "MacIntel" &&
                navigator.maxTouchPoints > 1)
            }
            snapshots={snapshots}
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
              busy={!!toolsSession.turn_active || !!toolsSnapshot?.pending}
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
