import { observeResize, observeViewport, viewportBounds } from "./layout.ts";
import { settled } from "./motion.ts";
import type { ComponentChildren, JSX } from "preact";
import { useId, useLayoutEffect, useRef, useState } from "preact/hooks";
import { Ellipsis } from "lucide-preact";

const navigation = ["ArrowDown", "ArrowUp", "Home", "End"];

// A row's actions, in a dropdown under its ⋯ button: a native top-layer
// popover placed against the button, which follows it while open. Every
// other overlay is a sheet (see sheet.tsx); a short list of actions belongs
// beside what it acts on.
export function Menu({
  label,
  children,
}: {
  label: string;
  children: ComponentChildren;
}) {
  const [open, setOpen] = useState(false);
  const anchor = useRef<HTMLButtonElement>(null);
  const panel = useRef<HTMLDivElement>(null);
  const id = useId();
  // Hiding plays the panel's exit (CSS, allow-discrete); the panel unmounts
  // once that has finished. Our own close unmounts without waiting for the
  // toggle event: WebKit drops it when a menu item opens a modal dialog,
  // which left the menu unable to reopen. Light dismiss uses the event.
  const unmount = () => void settled(panel.current).then(() => setOpen(false));
  const close = () => {
    panel.current?.hidePopover();
    anchor.current?.focus({ preventScroll: true });
    unmount();
  };
  useLayoutEffect(() => {
    if (!open) return;
    const element = panel.current;
    const button = anchor.current;
    if (!element || !button) return;
    element.showPopover({ source: button });
    const place = () => {
      const { left: x, top: y, width, height } = viewportBounds(true);
      const target = button.getBoundingClientRect();
      const gap =
        parseFloat(getComputedStyle(element).getPropertyValue("row-gap")) || 0;
      const above = Math.max(0, target.top - y - 2 * gap);
      const below = Math.max(0, y + height - target.bottom - 2 * gap);
      const upwards = below < Math.min(element.scrollHeight, above);
      element.style.maxWidth = `${width - 2 * gap}px`;
      element.style.maxHeight = `${Math.max(1, upwards ? above : below)}px`;
      const box = element.getBoundingClientRect();
      const left = target.right - box.width;
      element.style.left = `${Math.max(x + gap, Math.min(left, x + width - box.width - gap))}px`;
      element.style.top = `${Math.max(y + gap, Math.min(upwards ? target.top - box.height - gap : target.bottom + gap, y + height - box.height - gap))}px`;
    };
    place();
    element
      .querySelector<HTMLElement>("button:not(:disabled)")
      ?.focus({ preventScroll: true });
    const stopObserving = observeResize(place, element, button);
    // Follow the anchor wherever layout or scrolling moves it, as
    // floating-ui's autoUpdate does: one rect read per frame while open.
    let anchorAt = "";
    let frame = requestAnimationFrame(function follow() {
      const { x, y } = button.getBoundingClientRect();
      if (anchorAt !== (anchorAt = `${x},${y}`)) place();
      frame = requestAnimationFrame(follow);
    });
    const toggled = (event: ToggleEvent) => {
      if (event.newState === "closed") unmount();
    };
    element.addEventListener("toggle", toggled);
    const stopViewport = observeViewport(place);
    return () => {
      stopObserving();
      cancelAnimationFrame(frame);
      element.removeEventListener("toggle", toggled);
      stopViewport();
    };
  }, [open]);
  return (
    <div class="action-menu">
      <button
        type="button"
        ref={anchor}
        class="quiet icon-button"
        title={label}
        aria-label={label}
        aria-haspopup="menu"
        aria-controls={open ? id : undefined}
        aria-expanded={open}
        onClick={() => (open ? close() : setOpen(true))}
      >
        <Ellipsis />
      </button>
      {open && (
        <div
          ref={panel}
          id={id}
          popover="auto"
          role="menu"
          aria-label={label}
          class="menu-panel"
          onKeyDown={(event) => {
            if (event.key === "Escape") {
              event.preventDefault();
              event.stopPropagation();
              close();
              return;
            }
            if (!navigation.includes(event.key)) return;
            event.preventDefault();
            const items = [
              ...event.currentTarget.querySelectorAll<HTMLButtonElement>(
                "button:not(:disabled)",
              ),
            ];
            const index = items.indexOf(
              document.activeElement as HTMLButtonElement,
            );
            const last = items.length - 1;
            const down = event.key === "ArrowDown";
            items[
              event.key === "Home"
                ? 0
                : event.key === "End"
                  ? last
                  : index < 0
                    ? down
                      ? 0
                      : last
                    : (index + (down ? 1 : last)) % items.length
            ]?.focus();
          }}
          onClick={(event) => {
            if (
              event.target instanceof Element &&
              event.target.closest("button")
            )
              close();
          }}
        >
          {children}
        </div>
      )}
    </div>
  );
}

export function MenuItem({
  class: className = "",
  ...props
}: JSX.ButtonHTMLAttributes<HTMLButtonElement>) {
  return (
    <button
      {...props}
      class={`quiet ${className}`}
      role="menuitem"
      type="button"
    />
  );
}
