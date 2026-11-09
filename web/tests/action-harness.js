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
