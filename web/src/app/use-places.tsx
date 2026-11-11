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
import { hostApi, useApi } from "../state/api.ts";
import { useMultipleHosts } from "../state/sources.ts";
import { folderOf, projectsOf } from "../shared/display.ts";
import { scratchSpace } from "../shared/limits.ts";
import { SpaceMenu } from "./menus.tsx";
import { collapsed, placeKey } from "../features/sidebar/folding.ts";

// The places conversations start in: workspaces by name and folders by
// path, the New dialog that asks for one, and starting a conversation there.
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
  choose: (id: string, host?: string) => Promise<void>;
  load: (id: string) => Promise<Snapshot>;
  report: Report;
  open: (modal: AppModal) => void;
  modal: AppModal | null;
  setModal: (modal: AppModal | null) => void;
  setBusy: (busy: boolean) => void;
}) {
  const { command, host } = useApi();
  const multipleHosts = useMultipleHosts();
  const [newHost, setNewHost] = useState(host);
  const [folder, setFolder] = useState("");
  const [space, setSpace] = useState("");
  // A workspace typed into the dialog, or a folder, starts a conversation.
  async function create(event: TargetedSubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true);
    try {
      // Only the open dialog's field counts.
      const workspace = modal?.type === "new" && modal.place === "workspace";
      await startConversation(
        workspace ? "" : folder,
        workspace ? space.trim() : "",
        "",
        newHost,
      );
      setModal(null);
    } catch (failure) {
      report(failure);
    } finally {
      setBusy(false);
    }
  }
  // One start at a time: a double click must not race two activations.
  // With `text`, its first message is sent.
  const starting = useRef(false);
  const startConversation = useCallback(
    async (folder: string, space = "", text = "", targetHost = host) => {
      if (starting.current) return;
      starting.current = true;
      try {
        // A workspace is asked for by its name; the host keeps its folder.
        const created = await hostApi(targetHost).command(
          "create",
          null,
          space ? { space } : { cwd: folder },
        );
        if (created.pending) return "";
        upsertSession(created.session);
        // The new conversation is not behind a fold.
        collapsed.set(placeKey(targetHost, folderOf(created.session)), false);
        // Live before it is shown, so opening never flashes the saved state;
        // shown even if activating fails, with the failure reported.
        try {
          if (created.session.presence !== "active")
            await command("activate", created.session);
          if (text) await command("submit", created.session, { text });
        } finally {
          await choose(created.session.id, targetHost);
        }
        await load(created.session.id);
        return created.session.id;
      } finally {
        starting.current = false;
      }
    },
    [upsertSession, choose, load, command, host],
  );
  // Asks for the place's name or path, which starts a conversation there.
  const newPlace = useCallback(
    (place: "conversation" | "workspace" | "folder") => {
      setNewHost(place === "workspace" ? "" : host);
      setSpace("");
      setFolder("");
      open({ type: "new", place });
    },
    [open, host],
  );
  const startIn = useCallback(
    (cwd: string) => startConversation(cwd).catch(report),
    [startConversation, report],
  );
  const startScratch = useCallback(
    () =>
      multipleHosts
        ? newPlace("conversation")
        : startConversation("", scratchSpace).catch(report),
    [startConversation, report, multipleHosts, newPlace],
  );
  const projects = useMemo(
    () => projectsOf(catalogue.sessions),
    [catalogue.sessions],
  );
  const spaceMenu = useCallback(
    (space: Space, conversations: number) => (
      <SpaceMenu space={space} conversations={conversations} open={open} />
    ),
    [open],
  );
  return {
    newHost,
    setNewHost,
    folder,
    setFolder,
    space,
    setSpace,
    create,
    startConversation,
    newPlace,
    startIn,
    startScratch,
    projects,
    spaceMenu,
  };
}

// What the New dialog shows and changes.
export type Places = ReturnType<typeof usePlaces>;
