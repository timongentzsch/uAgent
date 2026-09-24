import {
  ConnectionStatus,
  StatusLed,
  type ConnectionPhase,
} from "../../shared/connection-status.tsx";
import { count } from "../../shared/quantities.ts";
import type { ComponentChildren } from "preact";
import type {
  Activity,
  Block,
  Collaborator,
  Pending,
  Report,
  SessionRef,
} from "../../shared/types.ts";
import { useEffect, useState } from "preact/hooks";
import { ChevronDown } from "lucide-preact";
import { Deferred } from "../../shared/ui.tsx";
const panel = () => import("./activity.tsx");
export interface ActivityProps {
  items?: Activity[];
  collaborators?: Collaborator[];
  target?: Block | null;
  clearTarget: () => void;
  cwd: string;
  running?: boolean;
  session: SessionRef;
  online: boolean;
  report: Report;
}
export const active = (item: Activity) =>
  ["running", "starting", "stopping", "finishing"].includes(item.status || "");
// Supervised activities plus collaborators not already listed as one.
export function withCollaborators(
  items: Activity[],
  collaborators: Collaborator[],
): Activity[] {
  return [
    ...items,
    ...collaborators
      .filter((child) => !items.some((item) => item.agent_id === child.id))
      .map((child) => ({
        ...child,
        id: undefined,
        agent_id: child.id,
        kind: "agent",
        status: child.status || "idle",
      })),
  ];
}

export function activityLabel(items: Activity[] = []) {
  const running = items.filter(active);
  const agents = running.filter((item) => item.kind === "agent").length;
  const commands = running.length - agents;
  return [
    agents && `${count(agents)} agent${agents === 1 ? "" : "s"}`,
    commands && `${count(commands)} command${commands === 1 ? "" : "s"}`,
  ]
    .filter(Boolean)
    .join(" · ");
}
export function ActivityStatus({
  phase = "Ready",
  running,
  items = [],
  collaborators = [],
  pending,
  announce = false,
  present = false,
  connection,
}: {
  phase?: string;
  running?: boolean;
  items?: Activity[];
  collaborators?: Collaborator[];
  pending?: Pending | boolean | null;
  announce?: boolean;
  present?: boolean;
  connection?: ConnectionPhase;
}) {
  if (connection && connection !== "connected")
    return <ConnectionStatus phase={connection} />;
  const counts = activityLabel(withCollaborators(items, collaborators));
  return (
    <span
      class="activity-status"
      role={announce ? "status" : undefined}
      aria-atomic={announce ? "true" : undefined}
    >
      <StatusLed
        state={running && !pending ? "running" : present ? "active" : "idle"}
      />
      <span
        class="activity-caption"
        title={pending ? "Needs your input" : phase}
      >
        {pending ? "Needs your input" : phase}
      </span>
      {counts && <span class="activity-counts"> · {counts}</span>}
    </span>
  );
}
export default function Activities(
  props: ActivityProps & {
    phase?: string;
    pending?: Pending | null;
    present?: boolean;
    connection?: ConnectionPhase;
    children?: ComponentChildren;
  },
) {
  const [open, setOpen] = useState(false);
  useEffect(() => {
    if (props.target) setOpen(true);
  }, [props.target]);
  return (
    <div class="activities">
      <div class="status-line">
        <button
          type="button"
          class="quiet activity-toggle"
          onClick={() => setOpen(!open)}
          aria-expanded={open}
          aria-label="Activity"
        >
          <ActivityStatus {...props} announce />
          {((props.items?.length || 0) > 0 ||
            (props.collaborators?.length || 0) > 0) && <ChevronDown />}
        </button>
        {props.children}
      </div>
      {open && <Deferred load={panel} {...props} />}
    </div>
  );
}
