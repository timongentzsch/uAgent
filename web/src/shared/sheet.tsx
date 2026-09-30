import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import { Modal, useDialogClose } from "./ui.tsx";

type Content = ComponentChildren | ((close: () => void) => ComponentChildren);

// Hands content the close of the sheet it is drawn in.
function SheetContent({ children }: { children: Content }) {
  const close = useDialogClose();
  return <>{typeof children === "function" ? children(close) : children}</>;
}

// A control whose content opens in a sheet: from the right edge, or from
// below on a phone. Panels are sheets, so focus, Escape and the back gesture
// behave as in every dialog; only a row's ⋯ actions drop down (menu.tsx).
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
}) {
  const [open, setOpen] = useState(false);
  return (
    <div class={`sheet-control ${className}`}>
      <button
        type="button"
        class={buttonClass}
        title={title}
        aria-label={label}
        aria-haspopup="dialog"
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
