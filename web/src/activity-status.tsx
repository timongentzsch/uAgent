import { count } from "./quantities.ts";
import type { ComponentChildren } from "preact";
import type {
  Activity,
  Block,
  Collaborator,
  Pending,
  Report,
  SessionRef,
} from "./types.ts";
import { useEffect, useState } from "preact/hooks";
import { ChevronDown } from "lucide-preact";
import { Deferred } from "./ui.tsx";
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
export type LedState = "idle" | "active" | "running";
export function StatusLed({ state }: { state: LedState }) {
  return <span class={`status-led ${state}`} aria-hidden="true" />;
}
export function ActivityStatus({
  phase = "Ready",
  running,
  items = [],
  collaborators = [],
  pending,
  announce = false,
  present = false,
}: {
  phase?: string;
  running?: boolean;
  items?: Activity[];
  collaborators?: Collaborator[];
  pending?: Pending | boolean | null;
  announce?: boolean;
  present?: boolean;
}) {
  const counts = activityLabel([
    ...items,
    ...collaborators.map((item) => ({
      ...item,
      id: undefined,
      agent_id: item.id,
      kind: "agent",
    })),
  ]);
  return (
    <span
      class="activity-status"
      role={announce ? "status" : undefined}
      aria-atomic={announce ? "true" : undefined}
    >
      <StatusLed
        state={running && !pending ? "running" : present ? "active" : "idle"}
      />
      {pending ? "Needs your input" : phase}
      {counts && ` · ${counts}`}
    </span>
  );
}
export default function Activities(
  props: ActivityProps & {
    phase?: string;
    pending?: Pending | null;
    present?: boolean;
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
