import { h, render } from "preact";
import { useAction } from "../src/shared/use-action.ts";

// Runs the real hook through Preact; each mount has an independent owner.
export function mountAction(options) {
  const root = document.body.appendChild(document.createElement("div"));
  let action;
  function Probe() {
    action = useAction(options);
    return null;
  }
  render(h(Probe), root);
  return {
    run: (work) => action.run(work),
    state: () => ({ busy: action.busy, error: action.error?.message ?? null }),
    close: () => {
      render(null, root);
      root.remove();
    },
  };
}

export async function mountUploads() {
  const { useDraftUploads } = await import("../src/app/use-draft-uploads.ts");
  const { useState } = await import("preact/hooks");
  const { emptyDraft } = await import("../src/state/store.ts");
  const root = document.body.appendChild(document.createElement("div"));
  let uploads, drafts, select, selected;
  const errors = [];
  function Probe() {
    const [id, setId] = useState("first");
    const [value, setDrafts] = useState({});
    selected = id;
    select = setId;
    drafts = value;
    const draft = value[id] || emptyDraft();
    uploads = useDraftUploads({
      session: { id },
      online: true,
      selected: id,
      draft,
      uploading: draft.files.some((file) => file.pending),
      setDrafts,
      report: (error) => errors.push(error.message),
    });
    return null;
  }
  render(h(Probe), root);
  return {
    upload: () => uploads.upload([new File(["text"], "note.txt")]),
    select: (id) => select(id),
    state: () => ({ selected, drafts, errors }),
    close: () => {
      render(null, root);
      root.remove();
    },
  };
}
