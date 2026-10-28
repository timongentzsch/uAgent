import { observeResize, observeViewport, placePopover } from "./layout.ts";
import { settled } from "./motion.ts";
import {
  createContext,
  type ComponentChildren,
  type ButtonHTMLAttributes,
  type Ref,
  type TargetedKeyboardEvent,
  type TargetedMouseEvent,
} from "preact";
import {
  useCallback,
  useContext,
  useId,
  useLayoutEffect,
  useRef,
  useState,
} from "preact/hooks";
import { ChevronLeft, ChevronRight, Ellipsis } from "lucide-preact";
import { nextIndex, typeAhead } from "./listbox-nav.ts";
import { Button, IconButton } from "./ui.tsx";

// The dropdown menu for a row's or header's actions: items, nested views and
// the keyboard and focus behaviour shared by every menu.

const navigation = ["ArrowDown", "ArrowUp", "Home", "End"];
// Which nested view the panel shows (a MenuSub's id, "" for the items
// themselves), and how to change it. A view's own items see "".
const View = createContext({
  view: "",
  show: (_view: string) => {},
  // The menu's name, for the row that leads back to it.
  menu: "",
});
const enabledItems = (panel: Element) => [
  ...panel.querySelectorAll<HTMLButtonElement>("button:not(:disabled)"),
];

// A row's actions, in a dropdown under its ⋯ button: a native top-layer
// popover placed against the button, which follows it while open. Every
// other overlay is a sheet (see sheet.tsx); a short list of actions belongs
// beside what it acts on.
export function Menu({
  label,
  trigger,
  children,
}: {
  label: string;
  // A button that says what it is, in place of the ⋯.
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
  // Hiding plays the panel's exit (CSS, allow-discrete); the panel unmounts
  // once that has finished. Our own close unmounts without waiting for the
  // toggle event: WebKit drops it when a menu item opens a modal dialog,
  // which left the menu unable to reopen. Light dismiss uses the event.
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
          <View.Provider value={{ view, show, menu: label }}>
            {children}
          </View.Provider>
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
  const { view, show, menu } = useContext(View);
  const id = useId();
  const open = view === id;
  // Focus follows into the view, to its first choice (the row that leads
  // back when none can be taken), and back to this row when it closes.
  const row = useRef<HTMLButtonElement>(null);
  const opened = useRef(false);
  useLayoutEffect(() => {
    const first = row.current?.parentElement?.querySelector<HTMLElement>(
      "[role=group] button:not(:disabled)",
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
    <View.Provider value={{ view: "", show, menu }}>
      <MenuItem
        buttonRef={row}
        data-menu-stay
        aria-label={`Back to ${menu}`}
        onClick={() => show("")}
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
