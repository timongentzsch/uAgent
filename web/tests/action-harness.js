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
  const { valueStore } = await import("../src/state/value-store.ts");
  const drafts = valueStore({});
  const root = document.body.appendChild(document.createElement("div"));
  let uploads, select, selected;
  const errors = [];
  function Probe() {
    const [id, setId] = useState("first");
    selected = id;
    select = setId;
    uploads = useDraftUploads({
      host: {
        online: true,
        selected: id,
        drafts,
        report: (error) => errors.push(error.message),
        catalogue: { sessions: [], devices: [], capabilities: {} },
      },
      session: { id },
    });
    return null;
  }
  render(h(Probe), root);
  return {
    upload: () => uploads.upload([new File(["text"], "note.txt")]),
    select: (id) => select(id),
    state: () => ({ selected, drafts: drafts.get(), errors }),
    close: () => {
      render(null, root);
      root.remove();
    },
  };
}
