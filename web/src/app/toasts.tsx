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
  return (
    <div class="toasts">
      {(error || notice) && (
        <div role={error ? "alert" : "status"} class="error-banner">
          <span>{error || notice}</span>
          <Button onClick={dismiss}>Dismiss</Button>
        </div>
      )}
      {/* A waiting service worker means this view is stale (notably in an
          installed PWA with no chrome to pull-to-refresh). Surface it
          here, not only in Settings, so reloads actually pick up fixes. */}
      {update && (
        <div role="status" class="update-banner">
          <span>Update available with the latest fixes.</span>
          <Button
            variant="primary"
            disabled={!!updateBlocked}
            title={updateBlocked || "Reloads this view with the latest fixes."}
            onClick={() =>
              import("../shared/pwa.ts").then(({ applyUpdate }) =>
                applyUpdate(update),
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
