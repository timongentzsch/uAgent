import { useExit } from "../shared/motion.ts";
import { focusComposer } from "../shared/navigation.ts";
import { Button, Notice } from "../shared/ui.tsx";

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
        <div ref={messageRef} class="banner enters">
          <Notice
            tone={error ? "error" : "info"}
            action={
              <Button
                onClick={() => {
                  dismiss();
                  focusComposer();
                }}
              >
                Dismiss
              </Button>
            }
          >
            {message}
          </Notice>
        </div>
      )}
      {/* An installed PWA has no reload button: the update shows here too. */}
      {waiting && (
        <div ref={updateRef} class="banner enters">
          <Notice
            tone="info"
            action={
              <Button
                variant="primary"
                disabled={!!updateBlocked}
                title={
                  updateBlocked || "Reloads this view with the latest fixes."
                }
                onClick={() =>
                  import("../shared/pwa.ts").then(({ applyUpdate }) =>
                    applyUpdate(waiting),
                  )
                }
              >
                Refresh now
              </Button>
            }
          >
            Update available with the latest fixes.
          </Notice>
        </div>
      )}
    </div>
  );
}
