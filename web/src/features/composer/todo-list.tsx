import { Circle, CircleCheck, CircleDot } from "lucide-preact";
import type { TodoItem } from "../../shared/types.ts";
import { cleanText } from "../../shared/ui.tsx";

// The conversation's to-do list while it has items: the first item not done
// is the current step and stands out, done ones are struck through.
export default function TodoList({ items }: { items?: TodoItem[] }) {
  if (!items?.length) return null;
  const done = items.filter((item) => item.status === "done").length;
  const current = items.findIndex((item) => item.status !== "done");
  return (
    <section class="todo-list" aria-label="To-dos">
      <div class="todo-heading">
        To-dos · {done}/{items.length} done
      </div>
      <ol class="todo-items">
        {items.map((item, index) => {
          const Icon =
            item.status === "done"
              ? CircleCheck
              : index === current
                ? CircleDot
                : Circle;
          return (
            <li
              key={index}
              data-status={item.status}
              aria-current={index === current ? "step" : undefined}
            >
              <Icon aria-hidden="true" />
              {item.status === "done" && <span class="sr-only">Done: </span>}
              <span class={index === current ? "working" : undefined}>
                {cleanText(item.text)}
              </span>
            </li>
          );
        })}
      </ol>
    </section>
  );
}
