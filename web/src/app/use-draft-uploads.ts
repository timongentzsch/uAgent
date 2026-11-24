import { useCallback, useRef } from "preact/hooks";
import type { Draft, Session } from "../shared/types.ts";
import { emptyDraft } from "../state/store.ts";
import { requestId, useApi } from "../state/api.ts";
import type { HostState } from "../state/use-host.ts";
import { dedupeName } from "../features/composer/mention.ts";
import { maxDraftFiles, maxUploadBytes } from "../shared/limits.ts";

// No state of its own beyond admission: the drafts are the host layer's.
export function useDraftUploads({
  host: { online, selected, drafts, report },
  session,
}: {
  host: HostState;
  session: Session | undefined;
}) {
  const { uploadAttachment } = useApi();
  const active = useRef(new Set<string>());
  const updateDraft = useCallback(
    (id: string, change: (draft: Draft) => Draft) =>
      drafts.set((current) => ({
        ...current,
        [id]: change(current[id] || emptyDraft()),
      })),
    [drafts],
  );
  // `replaces` is the draft file the annotation was drawn on.
  async function attachAnnotated(file: File, replaces?: string) {
    const id = selected;
    if (!(await upload([file]))) return false;
    if (replaces)
      updateDraft(id, (draft) => ({
        ...draft,
        files: draft.files.filter((item) => item.id !== replaces),
      }));
    return true;
  }
  // Resolves true once every file is attached to the draft.
  async function upload(files: File[]) {
    if (!session || !files.length) return false;
    // Said, not dropped: the files are gone from the page once let go.
    if (!online) {
      report(new Error("Not connected: nothing was attached. Add it again."));
      return false;
    }
    const draft = drafts.get()[selected] ?? emptyDraft();
    if (
      draft.files.some((file) => file.pending) ||
      active.current.has(selected)
    )
      return false;
    const id = selected;
    if (
      files.length + draft.files.length > maxDraftFiles ||
      files.some((file) => file.size > maxUploadBytes)
    ) {
      report(
        new Error(
          `Attach up to ${maxDraftFiles} files, at most ${maxUploadBytes / 1024 / 1024} MiB each.`,
        ),
      );
      return false;
    }
    // Names dedupe against the live draft; the deduped name is the upload name,
    // so server record, model payload and UI agree.
    const taken = new Set(draft.files.map((item) => item.name));
    const pendingFiles = files.map((file) => {
      const name = dedupeName(file.name || "attachment", taken);
      taken.add(name);
      return {
        id: `upload-${requestId()}`,
        name,
        bytes: file.size,
        pending: true,
      };
    });
    // Admission is synchronous; another conversation can still upload.
    active.current.add(id);
    updateDraft(id, (draft) => ({
      ...draft,
      files: [...draft.files, ...pendingFiles],
    }));
    try {
      for (const [index, file] of files.entries()) {
        const asset = await uploadAttachment(
          id,
          file,
          pendingFiles[index].name,
        );
        updateDraft(id, (draft) => ({
          ...draft,
          files: draft.files.map((item) =>
            item.id === pendingFiles[index].id
              ? { ...asset, name: pendingFiles[index].name }
              : item,
          ),
        }));
      }
      return true;
    } catch (failure) {
      report(failure);
      return false;
    } finally {
      active.current.delete(id);
      const pendingIds = new Set(pendingFiles.map((item) => item.id));
      updateDraft(id, (draft) => ({
        ...draft,
        files: draft.files.filter((item) => !pendingIds.has(item.id)),
      }));
    }
  }
  return { updateDraft, attachAnnotated, upload };
}
