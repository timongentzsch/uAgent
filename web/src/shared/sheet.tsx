import type { ComponentChildren, JSX } from "preact";
import { useState } from "preact/hooks";
import { Ellipsis } from "lucide-preact";
import { Modal, useDialogClose } from "./ui.tsx";

type Content = ComponentChildren | ((close: () => void) => ComponentChildren);

// Hands content the close of the sheet it is drawn in.
function SheetContent({ children }: { children: Content }) {
  const close = useDialogClose();
  return <>{typeof children === "function" ? children(close) : children}</>;
}

// A control whose content opens in a sheet: from the right edge, or from
// below on a phone. Every overlay is a Modal, so nothing floats beside its
// anchor and focus, Escape and the back gesture behave the same everywhere.
// `children` may take the sheet's close, which plays its exit.
export function SheetButton({
  label,
  title = label,
  heading = label,
  trigger,
  children,
  className = "",
  buttonClass = "quiet icon-button",
  sheetClass = "",
  size = "narrow",
  disabled,
  menu = false,
}: {
  label: string;
  title?: string;
  // The sheet's title, when the button's label says more (a status).
  heading?: string;
  trigger: ComponentChildren;
  children: Content;
  className?: string;
  buttonClass?: string;
  sheetClass?: string;
  size?: "narrow" | "compact";
  disabled?: boolean;
  menu?: boolean;
}) {
  const [open, setOpen] = useState(false);
  return (
    <div class={`sheet-control ${className}`}>
      <button
        type="button"
        class={buttonClass}
        title={title}
        aria-label={label}
        aria-haspopup={menu ? "menu" : "dialog"}
        aria-expanded={open}
        disabled={disabled}
        onClick={() => setOpen(true)}
      >
        {trigger}
      </button>
      {open && (
        <Modal
          title={heading}
          layout="sheet"
          size={size}
          className={sheetClass}
          close={() => setOpen(false)}
          lightDismiss
        >
          <SheetContent>{children}</SheetContent>
        </Modal>
      )}
    </div>
  );
}

const navigation = ["ArrowDown", "ArrowUp", "Home", "End"];

// A row's actions, behind its ⋯ button. Choosing one closes the sheet; one
// that opens a dialog replaces it.
export function Menu({
  label,
  children,
}: {
  label: string;
  children: ComponentChildren;
}) {
  return (
    <SheetButton
      label={label}
      trigger={<Ellipsis />}
      className="action-menu"
      menu
    >
      {(close) => (
        <div
          role="menu"
          aria-label={label}
          class="menu-list"
          onKeyDown={(event) => {
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
    </SheetButton>
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
