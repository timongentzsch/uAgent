import type { Dispatch, StateUpdater } from "preact/hooks";
import type { Draft, Session } from "../shared/types.ts";
import { emptyDraft } from "../state/store.ts";
import { requestId, uploadAttachment } from "../state/api.ts";
import { dedupeName } from "../features/composer/mention.ts";
import { maxDraftFiles, maxUploadBytes } from "../shared/limits.ts";

// Files on their way into the open conversation's draft. No state of its
// own: the drafts are the host layer's, and each render binds these to the
// conversation then selected.
export function useDraftUploads({
  session,
  online,
  selected,
  draft,
  uploading,
  setDrafts,
  report,
}: {
  session: Session | undefined;
  online: boolean;
  selected: string;
  draft: Draft;
  uploading: boolean;
  setDrafts: Dispatch<StateUpdater<Record<string, Draft>>>;
  report: (error: unknown) => void;
}) {
  // One conversation's draft changes; the others keep theirs.
  function updateDraft(id: string, change: (draft: Draft) => Draft) {
    setDrafts((current) => ({
      ...current,
      [id]: change(current[id] || emptyDraft()),
    }));
  }
  // An annotated copy joins the draft and replaces the draft file it was
  // drawn on; a copy of a sent image is simply attached.
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
    if (!session || !online || uploading || !files.length) return false;
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
    // Display names dedupe against the live draft, so two pastes never
    // share a label. The deduped name is the upload name: server record,
    // model payload and UI agree with no migration.
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
      const pendingIds = new Set(pendingFiles.map((item) => item.id));
      updateDraft(id, (draft) => ({
        ...draft,
        files: draft.files.filter((item) => !pendingIds.has(item.id)),
      }));
    }
  }
  return { updateDraft, attachAnnotated, upload };
}
