import type { ButtonHTMLAttributes, ComponentChildren } from "preact";
import { Button } from "./ui.tsx";
import "./list-row.css";

// The current entry is marked with aria-current, which is also what styles it.
export function ListRow({
  title,
  meta,
  unread,
  class: className = "",
  children,
  ...props
}: Omit<ButtonHTMLAttributes<HTMLButtonElement>, "title"> & {
  title: ComponentChildren;
  meta?: ComponentChildren;
  unread?: string | false;
}) {
  return (
    <Button
      title={typeof title === "string" ? title : undefined}
      {...props}
      class={`list-row ${className}`}
    >
      <span class="row-text">
        <span class="row-label truncate">
          {title}
          {unread && (
            <span class="status-led unread" role="img" aria-label={unread} />
          )}
        </span>
        {meta && <small class="row-detail truncate">{meta}</small>}
      </span>
      {children}
    </Button>
  );
}
