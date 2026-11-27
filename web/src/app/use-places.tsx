import type { TargetedSubmitEvent } from "preact";
import { useCallback, useMemo, useRef, useState } from "preact/hooks";
import type { AppModal, Space } from "../shared/types.ts";
import { hostApi, useApi } from "../state/api.ts";
import type { HostState } from "../state/use-host.ts";
import { deleteConversation } from "../features/chat/conversation-actions.tsx";
import type { Redirect } from "../features/filing/filing.tsx";
import { folderOf, projectsOf } from "../shared/display.ts";
import { scratchSpace } from "../shared/limits.ts";
import { SpaceMenu } from "./menus.tsx";
import { collapsed, placeKey } from "../features/sidebar/folding.ts";
import { emptyDraft } from "../state/store.ts";
import { useLatest } from "../shared/use-latest.ts";

// A model picked before a conversation exists, and how it reads.
export interface ModelChoice {
  label: string;
  model: string;
  effort: string;
}

export function usePlaces({
  host: { catalogue, upsertSession, load, report, drafts },
  upload,
  choose,
  open,
  modal,
  setModal,
  setBusy,
}: {
  host: HostState;
  // Into a conversation's draft, as the composer attaches.
  upload: (files: File[], id: string) => Promise<boolean>;
  choose: (id: string, host?: string) => Promise<void>;
  open: (modal: AppModal) => void;
  modal: AppModal | null;
  setModal: (modal: AppModal | null) => void;
  setBusy: (busy: boolean) => void;
}) {
  const { command, host } = useApi();
  const [newHost, setNewHost] = useState(host);
  const [folder, setFolder] = useState("");
  const [space, setSpace] = useState("");
  async function create(event: TargetedSubmitEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true);
    try {
      // Only the open dialog's field counts.
      const workspace = modal?.type === "new" && modal.place === "workspace";
      await startConversation(
        workspace ? "" : folder,
        workspace ? space.trim() : "",
        carried.current,
        newHost,
      );
      setModal(null);
    } catch (failure) {
      report(failure);
    } finally {
      setBusy(false);
    }
  }
  // What was typed on the home screen before its place was asked for.
  const carried = useRef("");
  // One start at a time: a double click must not race two activations.
  const starting = useRef(false);
  const uploading = useLatest(upload);
  const startConversation = useCallback(
    async (
      folder: string,
      space = "",
      text = "",
      targetHost = host,
      model?: ModelChoice,
      files: File[] = [],
    ) => {
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
        collapsed.set(placeKey(targetHost, folderOf(created.session)), false);
        // The first message waits in the draft until it is sent: a failure on
        // the way (activating, an upload, the send) leaves it there.
        const id = created.session.id;
        if (text || files.length)
          drafts.set((all) => ({ ...all, [id]: { ...emptyDraft(), text } }));
        // Live before it is shown, so opening never flashes the saved state;
        // shown even if activating fails, with the failure reported.
        try {
          if (created.session.presence !== "active")
            await command("activate", created.session);
          // The model picked for it is its own before its first message.
          if (model)
            await command("model", created.session, {
              operation: "select",
              model: model.model,
              effort: model.effort,
            });
          if (
            (text || files.length) &&
            (!files.length || (await uploading.current(files, id)))
          ) {
            const sent = drafts.get()[id];
            await command("submit", created.session, {
              text: sent.text,
              attachment_ids: sent.files.map(({ id, name }) => ({ id, name })),
            });
            drafts.set((all) => ({ ...all, [id]: emptyDraft() }));
          }
        } catch (failure) {
          // It exists: whatever asked for it is done, and the person lands in
          // it with the refusal said and the message in its draft.
          report(failure);
        } finally {
          await choose(created.session.id, targetHost);
        }
        await load(created.session.id);
        return created.session.id;
      } finally {
        starting.current = false;
      }
    },
    [upsertSession, choose, load, command, host, drafts, uploading, report],
  );
  const newPlace = useCallback(
    (place: "conversation" | "workspace" | "folder", text = "") => {
      carried.current = text;
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
  // Scratch is a workspace, so Local's.
  const startUnfiled = useCallback(
    (text: string, model?: ModelChoice, files?: File[]) =>
      startConversation("", scratchSpace, text, "", model, files),
    [startConversation],
  );
  const redirect: Redirect = useCallback(
    async (from, text, to) => {
      await command("interrupt", from).catch(() => {});
      await deleteConversation(from, command);
      if (!to.session) return startConversation(to.cwd, "", text, "");
      await command("submit", to.session, { text });
      await choose(to.session.id);
    },
    [command, startConversation, choose],
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
    startUnfiled,
    redirect,
    projects,
    spaceMenu,
  };
}

export type Places = ReturnType<typeof usePlaces>;
