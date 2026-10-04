import { observeResize, observeViewport, viewportBounds } from "./layout.ts";
import { settled } from "./motion.ts";
import { createContext, type ComponentChildren, type JSX } from "preact";
import {
  useContext,
  useId,
  useLayoutEffect,
  useRef,
  useState,
} from "preact/hooks";
import { ChevronLeft, ChevronRight, Ellipsis } from "lucide-preact";
import { nextIndex, typeAhead } from "./listbox-nav.ts";
import { Button, IconButton } from "./ui.tsx";

const navigation = ["ArrowDown", "ArrowUp", "Home", "End"];
// Which nested view the panel shows (a MenuSub's id, "" for the items
// themselves), and how to change it. A view's own items see "".
const View = createContext({ view: "", show: (_view: string) => {} });
const enabledItems = (panel: Element) => [
  ...panel.querySelectorAll<HTMLButtonElement>("button:not(:disabled)"),
];

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
  const [view, show] = useState("");
  // Which item takes focus on opening: ArrowUp on the button opens at the
  // last, everything else at the first.
  const start = useRef<"first" | "last">("first");
  const anchor = useRef<HTMLButtonElement>(null);
  const panel = useRef<HTMLDivElement>(null);
  const id = useId();
  // Hiding plays the panel's exit (CSS, allow-discrete); the panel unmounts
  // once that has finished. Our own close unmounts without waiting for the
  // toggle event: WebKit drops it when a menu item opens a modal dialog,
  // which left the menu unable to reopen. Light dismiss uses the event.
  const unmount = () =>
    void settled(panel.current).then(() => {
      setOpen(false);
      show("");
    });
  const close = () => {
    panel.current?.hidePopover();
    anchor.current?.focus({ preventScroll: true });
    unmount();
  };
  const openAt = (at: "first" | "last") => {
    start.current = at;
    setOpen(true);
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
    const items = enabledItems(element);
    (start.current === "last" ? items.at(-1) : items[0])?.focus({
      preventScroll: true,
    });
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
      <IconButton
        buttonRef={anchor}
        label={label}
        aria-haspopup="menu"
        aria-controls={open ? id : undefined}
        aria-expanded={open}
        onClick={() => (open ? close() : openAt("first"))}
        onKeyDown={(event) => {
          if (event.key !== "ArrowDown" && event.key !== "ArrowUp") return;
          event.preventDefault();
          if (!open) openAt(event.key === "ArrowUp" ? "last" : "first");
        }}
      >
        <Ellipsis />
      </IconButton>
      {open && (
        <div
          ref={panel}
          id={id}
          popover="auto"
          role="menu"
          aria-label={label}
          class="menu-panel"
          onKeyDown={(event) => {
            const row = (document.activeElement as HTMLElement | null)?.dataset
              .menuView;
            // Escape and ArrowLeft leave a view before they leave the menu;
            // ArrowRight enters the view its row opens.
            if (view && (event.key === "Escape" || event.key === "ArrowLeft")) {
              event.preventDefault();
              event.stopPropagation();
              show("");
              return;
            }
            if (event.key === "ArrowRight" && row) {
              event.preventDefault();
              show(row);
              return;
            }
            if (event.key === "Escape") {
              event.preventDefault();
              event.stopPropagation();
              close();
              return;
            }
            // Tab leaves the menu from its button, onward or back, as
            // though the menu had not been open.
            if (event.key === "Tab") {
              close();
              return;
            }
            const items = enabledItems(event.currentTarget);
            const index = items.indexOf(
              document.activeElement as HTMLButtonElement,
            );
            if (navigation.includes(event.key)) {
              event.preventDefault();
              items[nextIndex(index, event.key, items.length)]?.focus();
              return;
            }
            if (event.ctrlKey || event.metaKey || event.altKey) return;
            const match = typeAhead(
              items.map((item) => item.textContent || ""),
              index,
              event.key,
            );
            if (match < 0) return;
            event.preventDefault();
            items[match].focus();
          }}
          onClick={(event) => {
            // A row that opens or leaves a view keeps the menu open.
            const pressed =
              event.target instanceof Element && event.target.closest("button");
            if (pressed && !pressed.hasAttribute("data-menu-stay")) close();
          }}
        >
          <View.Provider value={{ view, show }}>{children}</View.Provider>
        </div>
      )}
    </div>
  );
}

// A row that opens a nested view in place: its items replace the menu's,
// under a row that leads back. In place rather than beside the menu, so it
// works where there is no room for a second panel.
export function MenuSub({
  label,
  value,
  children,
}: {
  label: string;
  // What is chosen inside, shown on the row.
  value?: string;
  children: ComponentChildren;
}) {
  const { view, show } = useContext(View);
  const id = useId();
  const open = view === id;
  // Focus follows into the view (past the row that leads back), and back
  // to this row when the view closes.
  const opened = useRef(false);
  useLayoutEffect(() => {
    const row = document.querySelector<HTMLElement>(`[data-menu-view="${id}"]`);
    const first = row?.parentElement?.querySelector<HTMLElement>(
      "button:not([data-menu-stay]):not(:disabled)",
    );
    if (open) first?.focus({ preventScroll: true });
    else if (opened.current) row?.focus({ preventScroll: true });
    opened.current = open;
  }, [open]);
  if (!open)
    return (
      <MenuItem
        data-menu-stay
        data-menu-view={id}
        aria-haspopup="menu"
        onClick={() => show(id)}
      >
        {label}
        <span class="menu-value">{value}</span>
        <ChevronRight />
      </MenuItem>
    );
  return (
    <View.Provider value={{ view: "", show }}>
      <MenuItem data-menu-stay data-menu-view={id} onClick={() => show("")}>
        <ChevronLeft />
        {label}
      </MenuItem>
      {children}
    </View.Provider>
  );
}

export function MenuItem(props: JSX.ButtonHTMLAttributes<HTMLButtonElement>) {
  // While a view is open the menu's own items make way for it.
  if (useContext(View).view) return null;
  return (
    <Button
      // A choice among several says so with its own role.
      role="menuitem"
      {...props}
      variant="quiet"
      // Arrows and letters move focus between items; Tab leaves the menu.
      tabIndex={-1}
    />
  );
}
