import { useState } from "preact/hooks";
import { plural } from "../../../shared/quantities.ts";
import { Group, LoadError, Row } from "../../../shared/ui.tsx";
import { useAction } from "../../../shared/use-action.ts";
import { manage } from "../../../state/api.ts";

// A saved change that only a restart applies offers that restart: the
// running conversations, or the web host for its own settings.
export function RestartNotice({
  keys,
  host,
  running,
}: {
  keys: string[];
  host: boolean;
  running: number;
}) {
  const action = useAction();
  const [done, setDone] = useState("");
  const restart = () =>
    action.run(async () => {
      if (host) {
        await manage("restart_host");
        return setDone("Restarting the web host…");
      }
      const { restarting, deferred } = await manage("restart_conversations");
      setDone(
        [
          restarting && `Restarted ${plural(restarting, "conversation")}`,
          deferred && `${plural(deferred, "busy conversation")} after its turn`,
        ]
          .filter(Boolean)
          .join(" · ") || "No conversation was running",
      );
    });
  const applies = `Applies ${keys.join(", ")}.`;
  return (
    <Group
      title="Restart to apply"
      footer={
        host
          ? "Conversations keep running; this page reconnects by itself."
          : "New conversations use it already. Running ones keep their history."
      }
    >
      {done ? (
        <Row label={done} detail={applies} />
      ) : host || running > 0 ? (
        <Row
          label={
            host
              ? "Restart web host"
              : `Restart ${plural(running, "running conversation")}`
          }
          detail={applies}
          disabled={action.busy}
          onClick={() => void restart()}
        />
      ) : (
        <Row label="No conversation is running" detail={applies} />
      )}
      {action.error != null && <LoadError error={action.error} />}
    </Group>
  );
}
