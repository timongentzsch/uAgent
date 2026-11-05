import { useExit } from "../shared/motion.ts";
import { focusComposer } from "../shared/navigation.ts";
import { Button } from "../shared/ui.tsx";

// Notices float over the app, never push it down: one line of an error or a
// notice, and an update that waits to be applied.
export default function Toasts({
  error,
  notice,
  dismiss,
  update,
  updateBlocked,
}: {
  error: string;
  notice: string;
  dismiss: () => void;
  update: ServiceWorker | null;
  // Why the update must wait, if it must.
  updateBlocked: string;
}) {
  // Each leaves the way it came; dismissing one returns focus to the input.
  const [message, messageRef] = useExit<string, HTMLDivElement>(
    error || notice,
  );
  const [waiting, updateRef] = useExit<ServiceWorker | null, HTMLDivElement>(
    update,
  );
  return (
    <div class="toasts">
      {message && (
        <div
          ref={messageRef}
          role={error ? "alert" : "status"}
          class="error-banner enters"
        >
          <span>{message}</span>
          <Button
            onClick={() => {
              dismiss();
              focusComposer();
            }}
          >
            Dismiss
          </Button>
        </div>
      )}
      {/* A waiting service worker means this view is stale (notably in an
          installed PWA with no chrome to pull-to-refresh). Surface it
          here, not only in Settings, so reloads actually pick up fixes. */}
      {waiting && (
        <div ref={updateRef} role="status" class="update-banner enters">
          <span>Update available with the latest fixes.</span>
          <Button
            variant="primary"
            disabled={!!updateBlocked}
            title={updateBlocked || "Reloads this view with the latest fixes."}
            onClick={() =>
              import("../shared/pwa.ts").then(({ applyUpdate }) =>
                applyUpdate(waiting),
              )
            }
          >
            Refresh now
          </Button>
        </div>
      )}
    </div>
  );
}
