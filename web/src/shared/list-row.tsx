import type { ComponentChildren, JSX } from "preact";
import { Button } from "./ui.tsx";

// A list entry that opens something: its title, a muted line under it and,
// when it has news, an unread dot named for screen readers. The current
// entry is marked with aria-current, which is also what styles it.
export function ListRow({
  title,
  meta,
  unread,
  class: className = "",
  children,
  ...props
}: Omit<JSX.ButtonHTMLAttributes<HTMLButtonElement>, "title"> & {
  title: ComponentChildren;
  meta?: ComponentChildren;
  unread?: string | false;
}) {
  return (
    <Button {...props} class={`list-row ${className}`}>
      <span>
        {title}
        {unread && <span class="unread-dot" role="img" aria-label={unread} />}
      </span>
      {meta && <small>{meta}</small>}
      {children}
    </Button>
  );
}
