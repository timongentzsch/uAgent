import { useExit } from "../shared/motion.ts";
import { focusComposer } from "../shared/navigation.ts";
import { Button } from "../shared/ui.tsx";

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
  updateBlocked: string;
}) {
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
      {/* An installed PWA has no reload button: the update shows here too. */}
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
