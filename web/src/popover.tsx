import { observeResize, observeViewport, viewportBounds } from "./layout.ts";
import type { ComponentChildren, JSX } from "preact";
import { useId, useLayoutEffect, useRef, useState } from "preact/hooks";
import { Ellipsis } from "lucide-preact";

export function Popover({
  label,
  title = label,
  trigger,
  children,
  className = "",
  buttonClass = "quiet icon-button",
  panelClass = "model-panel",
  side = "bottom",
  align = "end",
  disabled,
  menu = false,
}: {
  label: string;
  title?: string;
  trigger: ComponentChildren;
  children: ComponentChildren | ((close: () => void) => ComponentChildren);
  className?: string;
  buttonClass?: string;
  panelClass?: string;
  side?: "top" | "bottom";
  align?: "start" | "end";
  disabled?: boolean;
  menu?: boolean;
}) {
  const [open, setOpen] = useState(false);
  const anchor = useRef<HTMLButtonElement>(null);
  const panel = useRef<HTMLDivElement>(null);
  const id = useId();
  const close = () => {
    setOpen(false);
    anchor.current?.focus({ preventScroll: true });
  };
  useLayoutEffect(() => {
    if (!open) return;
    const element = panel.current;
    const button = anchor.current;
    if (!element || !button) return;
    element.showPopover({ source: button });
    const place = () => {
      const { left: x, top: y, width, height } = viewportBounds();
      const target = button.getBoundingClientRect();
      const above = Math.max(0, target.top - y - 16);
      const below = Math.max(0, y + height - target.bottom - 16);
      const upwards =
        side === "top"
          ? above >= Math.min(element.scrollHeight, below)
          : below < Math.min(element.scrollHeight, above);
      element.style.maxWidth = `${width - 16}px`;
      element.style.maxHeight = `${Math.max(1, upwards ? above : below)}px`;
      const box = element.getBoundingClientRect();
      const left = align === "start" ? target.left : target.right - box.width;
      element.style.left = `${Math.max(x + 8, Math.min(left, x + width - box.width - 8))}px`;
      element.style.top = `${Math.max(y + 8, Math.min(upwards ? target.top - box.height - 8 : target.bottom + 8, y + height - box.height - 8))}px`;
    };
    place();
    element
      .querySelector<HTMLElement>(
        "select:not(:disabled), button:not(:disabled), input:not(:disabled)",
      )
      ?.focus({ preventScroll: true });
    const stopObserving = observeResize(place, element, button);
    const scroll = (event: Event) => {
      if (!(event.target instanceof Node) || !element.contains(event.target))
        place();
    };
    const toggled = (event: ToggleEvent) => {
      if (event.newState === "closed") setOpen(false);
    };
    element.addEventListener("toggle", toggled);
    const stopViewport = observeViewport(place);
    document.addEventListener("scroll", scroll, true);
    return () => {
      stopObserving();
      element.removeEventListener("toggle", toggled);
      stopViewport();
      document.removeEventListener("scroll", scroll, true);
    };
  }, [open]);
  return (
    <div class={`popover-control ${className}`}>
      <button
        type="button"
        ref={anchor}
        class={buttonClass}
        title={title}
        aria-label={label}
        aria-haspopup={menu ? "menu" : "dialog"}
        aria-controls={open ? id : undefined}
        aria-expanded={open}
        disabled={disabled}
        onClick={() => setOpen(!open)}
      >
        {trigger}
      </button>
      {open && (
        <div
          ref={panel}
          id={id}
          popover="auto"
          role={menu ? "menu" : "dialog"}
          aria-label={label}
          class={`popover-panel ${menu ? "menu-panel" : panelClass}`}
          onKeyDown={(event) => {
            if (event.key === "Escape") {
              event.preventDefault();
              event.stopPropagation();
              close();
            }
            if (
              !menu ||
              !["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)
            )
              return;
            event.preventDefault();
            const buttons = [
              ...panel.current!.querySelectorAll<HTMLButtonElement>(
                "button:not(:disabled)",
              ),
            ];
            const index = buttons.findIndex(
              (button) => button === document.activeElement,
            );
            buttons[
              event.key === "Home"
                ? 0
                : event.key === "End"
                  ? buttons.length - 1
                  : (index +
                      (event.key === "ArrowDown" ? 1 : -1) +
                      buttons.length) %
                    buttons.length
            ]?.focus();
          }}
          onClick={(event) => {
            if (
              menu &&
              event.target instanceof Element &&
              event.target.closest("button")
            )
              close();
          }}
        >
          {typeof children === "function" ? children(close) : children}
        </div>
      )}
    </div>
  );
}
export function Menu({
  label,
  children,
}: {
  label: string;
  children: ComponentChildren;
}) {
  return (
    <Popover label={label} trigger={<Ellipsis />} className="action-menu" menu>
      {children}
    </Popover>
  );
}

export function MenuItem(props: JSX.ButtonHTMLAttributes<HTMLButtonElement>) {
  return <button {...props} role="menuitem" type="button" />;
}
