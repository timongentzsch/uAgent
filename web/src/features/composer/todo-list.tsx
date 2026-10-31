import { Circle, CircleCheck, CircleDot } from "lucide-preact";
import type { TodoItem } from "../../shared/types.ts";
import { cleanText } from "../../shared/ui.tsx";

const ICONS = { pending: Circle, in_progress: CircleDot, done: CircleCheck };

// The conversation's to-do list while it has items: the item in progress
// stands out, done ones are struck through.
export default function TodoList({ items }: { items?: TodoItem[] }) {
  if (!items?.length) return null;
  const done = items.filter((item) => item.status === "done").length;
  return (
    <section class="todo-list" aria-label="To-dos">
      <div class="todo-heading">
        To-dos · {done}/{items.length} done
      </div>
      <ol class="todo-items">
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
              <span
                class={
                  item.status === "in_progress" ? "shimmer-text" : undefined
                }
              >
                {cleanText(item.text)}
              </span>
            </li>
          );
        })}
      </ol>
    </section>
  );
}
