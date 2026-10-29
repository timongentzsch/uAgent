import { Circle, CircleCheck, CircleDot } from "lucide-preact";
import type { PlanItem } from "../../shared/types.ts";
import { cleanText } from "../../shared/ui.tsx";

const ICONS = { pending: Circle, in_progress: CircleDot, done: CircleCheck };

// The conversation's plan while it has items: the item in progress stands
// out, done ones are struck through.
export default function PlanList({ items }: { items?: PlanItem[] }) {
  if (!items?.length) return null;
  const done = items.filter((item) => item.status === "done").length;
  return (
    <section class="plan-list" aria-label="Plan">
      <div class="plan-heading">
        Plan · {done}/{items.length} done
      </div>
      <ol class="plan-items">
        {items.map((item, index) => {
          const Icon = ICONS[item.status];
          return (
            <li
              key={index}
              data-status={item.status}
              aria-current={item.status === "in_progress" ? "step" : undefined}
            >
              <Icon aria-hidden="true" />
              {item.status === "done" && <span class="sr-only">Done: </span>}
              <span>{cleanText(item.text)}</span>
            </li>
          );
        })}
      </ol>
    </section>
  );
}
