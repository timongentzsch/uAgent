import type { TargetedSubmitEvent } from "preact";
import { useCallback, useMemo, useRef, useState } from "preact/hooks";
import type {
  AppModal,
  Catalogue,
  Report,
  Session,
  Snapshot,
  Space,
} from "../shared/types.ts";
import { command } from "../state/api.ts";
import { folderName } from "../shared/folder-label.tsx";
import { folderOf } from "../shared/display.ts";
import { SpaceMenu } from "../features/sidebar/sidebar.tsx";
import { collapsed } from "../features/sidebar/folding.ts";

// The places conversations start in: workspaces by name and folders by
// path, the New dialog that asks for one, and starting a conversation or a
// panel there.
export function usePlaces({
  catalogue,
  upsertSession,
  choose,
  load,
  report,
  open,
  modal,
  setModal,
  setBusy,
}: {
  catalogue: Catalogue;
  upsertSession: (session: Session) => void;
  choose: (id: string) => Promise<void>;
  load: (id: string) => Promise<Snapshot>;
  report: Report;
  open: (modal: AppModal) => void;
  modal: AppModal | null;
  setModal: (modal: AppModal | null) => void;
  setBusy: (busy: boolean) => void;
}) {
  const [folder, setFolder] = useState("");
  const [space, setSpace] = useState("");
  // A conversation in the workspace or folder typed into the dialog.
  async function create(event: TargetedSubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true);
    try {
      // Only the open dialog's field counts.
      const workspace = modal?.type === "new" && modal.place === "workspace";
      await startConversation(
        workspace ? "" : folder,
        false,
        workspace ? space.trim() : "",
      );
      setModal(null);
    } catch (failure) {
      report(failure);
    } finally {
      setBusy(false);
    }
  }
  // The host returns a folder's existing panel rather than a second.
  // One start at a time: a double click must not race two activations.
  const starting = useRef(false);
  const startConversation = useCallback(
    async (folder: string, panel = false, space = "") => {
      if (starting.current) return;
      starting.current = true;
      try {
        // A workspace is asked for by its name; the host keeps its folder.
        const created = await command("create", null, {
          ...(space ? { space } : { cwd: folder }),
          panel,
        });
        if (created.pending) return "";
        upsertSession(created.session);
        // The new conversation is not behind a fold.
        collapsed.set(folderOf(created.session), false);
        // Live before it is shown, so opening never flashes the saved state;
        // shown even if activating fails, with the failure reported.
        try {
          if (created.session.presence !== "active")
            await command("activate", created.session);
        } finally {
          await choose(created.session.id);
        }
        await load(created.session.id);
        return created.session.id;
      } finally {
        starting.current = false;
      }
    },
    [upsertSession, choose, load],
  );
  // Asks for the place's name or path, which starts a conversation there.
  const newPlace = useCallback(
    (place: "workspace" | "folder") => {
      setSpace("");
      setFolder("");
      open({ type: "new", place });
    },
    [open],
  );
  // A workspace by the name it goes by, a folder by the end of its path.
  const placeName = (path = "") =>
    catalogue.spaces?.find((item) => item.path === path)?.name ||
    folderName(path);
  const startIn = useCallback(
    (cwd: string) => startConversation(cwd).catch(report),
    [startConversation, report],
  );
  const openPanel = useCallback(
    (cwd: string) => startConversation(cwd, true).catch(report),
    [startConversation, report],
  );
  const projects = useMemo(
    () => [
      ...new Set(
        catalogue.sessions
          .map((entry) => entry.cwd)
          .filter((path): path is string => !!path),
      ),
    ],
    [catalogue.sessions],
  );
  const spaceMenu = useCallback(
    (space: Space, conversations: number) => (
      <SpaceMenu space={space} conversations={conversations} open={open} />
    ),
    [open],
  );
  return {
    folder,
    setFolder,
    space,
    setSpace,
    create,
    startConversation,
    newPlace,
    placeName,
    startIn,
    openPanel,
    projects,
    spaceMenu,
  };
}

// What the New dialog shows and changes.
export type Places = ReturnType<typeof usePlaces>;
