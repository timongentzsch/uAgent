import { count } from "./quantities.ts";
import type { ComponentChildren } from "preact";
import type { Activity, Pending } from "./types.ts";
import type { ActivityProps } from "./activity.tsx";
import { useEffect, useState } from "preact/hooks";
import { ChevronDown } from "lucide-preact";
import { Deferred } from "./ui.tsx";
const panel = () => import("./activity.tsx");
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
export function ActivityStatus({
  phase = "Ready",
  running,
  items = [],
  pending,
  announce = false,
}: {
  phase?: string;
  running?: boolean;
  items?: Activity[];
  pending?: Pending | boolean | null;
  announce?: boolean;
}) {
  const counts = activityLabel(items);
  return (
    <span
      class="activity-status"
      role={announce ? "status" : undefined}
      aria-atomic={announce ? "true" : undefined}
    >
      <span class={running || counts ? "working-mark" : ""} aria-hidden="true">
        {running || counts ? "◌" : "·"}
      </span>
      {pending ? "Needs your input" : phase}
      {counts && ` · ${counts}`}
    </span>
  );
}
export default function Activities(
  props: ActivityProps & {
    phase?: string;
    pending?: Pending | null;
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
