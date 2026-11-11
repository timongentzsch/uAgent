import {
  coarsePointer,
  observeResize,
  observeViewport,
  placePopover,
} from "./layout.ts";
import { longPressMs, longPressSlopPx } from "./limits.ts";
import { settled } from "./motion.ts";
import {
  createContext,
  type ComponentChildren,
  type ButtonHTMLAttributes,
  type Ref,
  type RefObject,
  type TargetedKeyboardEvent,
  type TargetedMouseEvent,
} from "preact";
import {
  useCallback,
  useContext,
  useEffect,
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
// themselves), and how to change it. A view inside a view has its parent's
// id and a "/" before its own. A view's own items see "".
const View = createContext({
  view: "",
  show: (_view: string) => {},
  // The menu's or view's name, for the row that leads back to it.
  menu: "",
  // What the ids of the views opened from here start with.
  path: "",
});
const outOf = (view: string) =>
  view.slice(0, Math.max(0, view.lastIndexOf("/")));
const enabledItems = (panel: Element) => [
  ...panel.querySelectorAll<HTMLButtonElement>("button:not(:disabled)"),
];

// Touch has no hover: a row opens its menu on a long press, when the touch
// lifts, since a popover opened under a finger would be light-dismissed by
// its release. No click follows the lift, so the row is not also activated.
export function useLongPress(
  list: RefObject<HTMLElement | null>,
  rows: string,
) {
  useEffect(() => {
    const root = list.current;
    if (!root) return;
    let timer = 0;
    let from = { x: 0, y: 0 };
    // The ⋯ of the row held long enough.
    let held: HTMLButtonElement | undefined;
    let row: Element | null = null;
    const rowOf = (event: Event) =>
      event.target instanceof Element ? event.target.closest(rows) : null;
    const cancel = () => {
      clearTimeout(timer);
      held = undefined;
      row?.removeAttribute("data-held");
    };
    const start = (event: TouchEvent) => {
      cancel();
      // A place's last menu is its ⋯, after the one that starts something.
      row = rowOf(event);
      const button = [...(row?.querySelectorAll("button") ?? [])]
        .filter((item) => item.closest(".action-menu"))
        .at(-1);
      if (!button || button.disabled) return;
      from = { x: event.touches[0].clientX, y: event.touches[0].clientY };
      timer = window.setTimeout(() => {
        held = button;
        row?.setAttribute("data-held", "");
      }, longPressMs);
    };
    const move = (event: TouchEvent) => {
      const { clientX, clientY } = event.touches[0];
      if (Math.hypot(clientX - from.x, clientY - from.y) > longPressSlopPx)
        cancel();
    };
    const end = (event: TouchEvent) => {
      const button = held;
      cancel();
      if (!button) return;
      event.preventDefault();
      button.click();
    };
    const contextmenu = (event: Event) => {
      if (coarsePointer() && rowOf(event)) event.preventDefault();
    };
    root.addEventListener("touchstart", start, { passive: true });
    root.addEventListener("touchmove", move, { passive: true });
    root.addEventListener("touchend", end);
    root.addEventListener("touchcancel", cancel);
    root.addEventListener("contextmenu", contextmenu);
    return () => {
      cancel();
      root.removeEventListener("touchstart", start);
      root.removeEventListener("touchmove", move);
      root.removeEventListener("touchend", end);
      root.removeEventListener("touchcancel", cancel);
      root.removeEventListener("contextmenu", contextmenu);
    };
  }, [list, rows]);
}

// A native top-layer popover placed against its button, which it follows
// while open. Every other overlay is a sheet (see sheet.tsx).
export function Menu({
  label,
  trigger,
  children,
}: {
  label: string;
  trigger?: ComponentChildren;
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
  // The panel unmounts once its exit has played. Our own close does not wait
  // for the toggle event: WebKit drops it when a menu item opens a modal
  // dialog, which leaves the menu unable to reopen.
  const unmount = useCallback(
    () =>
      void settled(panel.current).then(() => {
        setOpen(false);
        show("");
      }),
    [],
  );
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
    const place = () => placePopover(element, button, "end");
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
  }, [open, unmount]);
  const button = {
    buttonRef: anchor,
    "aria-haspopup": "menu" as const,
    "aria-controls": open ? id : undefined,
    "aria-expanded": open,
    // Associate the invoker even where showPopover({ source }) is ignored:
    // pressing it must toggle, rather than light-dismiss before our click.
    popovertarget: id,
    onClick: (event: TargetedMouseEvent<HTMLButtonElement>) => {
      // Mounting and exit animations are owned here, so skip native toggle.
      event.preventDefault();
      if (open) close();
      else openAt("first");
    },
    onKeyDown: (event: TargetedKeyboardEvent<HTMLButtonElement>) => {
      if (event.key !== "ArrowDown" && event.key !== "ArrowUp") return;
      event.preventDefault();
      if (!open) openAt(event.key === "ArrowUp" ? "last" : "first");
    },
  };
  return (
    <div class="action-menu">
      {trigger ? (
        <Button variant="quiet" {...button}>
          {trigger}
        </Button>
      ) : (
        <IconButton label={label} {...button}>
          <Ellipsis />
        </IconButton>
      )}
      {open && (
        <div
          ref={panel}
          id={id}
          popover="auto"
          role="menu"
          aria-label={label}
          class="menu-panel popover-list"
          onKeyDown={(event) => {
            const row = (document.activeElement as HTMLElement | null)?.dataset
              .menuView;
            // Escape and ArrowLeft leave a view before they leave the menu;
            // ArrowRight enters the view its row opens.
            if (view && (event.key === "Escape" || event.key === "ArrowLeft")) {
              event.preventDefault();
              event.stopPropagation();
              show(outOf(view));
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
          // Focus is back on the menu's button before an item acts, so a
          // dialog it opens returns focus there, not to an item about to go.
          onClickCapture={(event) => {
            const pressed =
              event.target instanceof Element && event.target.closest("button");
            if (pressed && !pressed.hasAttribute("data-menu-stay"))
              anchor.current?.focus({ preventScroll: true });
          }}
          onClick={(event) => {
            // A row that opens or leaves a view keeps the menu open.
            const pressed =
              event.target instanceof Element && event.target.closest("button");
            if (pressed && !pressed.hasAttribute("data-menu-stay")) close();
          }}
        >
          <View.Provider value={{ view, show, menu: label, path: "" }}>
            {children}
          </View.Provider>
        </div>
      )}
    </div>
  );
}

// A nested view opens in place rather than beside the menu, so it works
// where there is no room for a second panel.
export function MenuSub({
  label,
  value,
  children,
}: {
  label: string;
  value?: string;
  children: ComponentChildren;
}) {
  const { view, show, menu, path } = useContext(View);
  const id = path + useId();
  const here = view === id;
  const open = here || view.startsWith(`${id}/`);
  // Focus follows into the view, to its first choice (the row that leads
  // back when none can be taken), and back to this row when it closes.
  const row = useRef<HTMLButtonElement>(null);
  const opened = useRef(false);
  useLayoutEffect(() => {
    const first = row.current?.parentElement?.querySelector<HTMLElement>(
      ":scope > [role=group] button:not(:disabled)",
    );
    if (open) (first ?? row.current)?.focus({ preventScroll: true });
    else if (opened.current) row.current?.focus({ preventScroll: true });
    opened.current = open;
  }, [open]);
  if (!open)
    return (
      <MenuItem
        buttonRef={row}
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
    <View.Provider
      value={{ view: here ? "" : view, show, menu: label, path: `${id}/` }}
    >
      <MenuItem
        buttonRef={row}
        data-menu-stay
        aria-label={`Back to ${menu}`}
        onClick={() => show(outOf(id))}
      >
        <ChevronLeft />
        {label}
      </MenuItem>
      <div role="group" aria-label={label}>
        {children}
      </div>
    </View.Provider>
  );
}

export function MenuItem(
  props: ButtonHTMLAttributes<HTMLButtonElement> & {
    buttonRef?: Ref<HTMLButtonElement>;
  },
) {
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
