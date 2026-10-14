import type { ComponentChildren } from "preact";
import { useEffect, useState } from "preact/hooks";
import { Button, Modal, useDialogClose } from "./ui.tsx";

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
  buttonClass = "",
  size = "default",
  sheetClass = "",
  disabled,
  openWhen = false,
}: {
  label: string;
  title?: string;
  // The sheet's title, when the button's label says more (a status).
  heading?: string;
  trigger: ComponentChildren;
  children: Content;
  className?: string;
  buttonClass?: string;
  // "icon" when the trigger is an icon alone.
  size?: "default" | "icon";
  sheetClass?: string;
  disabled?: boolean;
  // Opens the sheet when it becomes true: a control that needs an answer
  // before anything else can happen.
  openWhen?: boolean;
}) {
  const [open, setOpen] = useState(false);
  useEffect(() => {
    if (openWhen && !disabled) setOpen(true);
  }, [openWhen, disabled]);
  return (
    <div class={`sheet-control ${className}`}>
      <Button
        variant="quiet"
        size={size}
        class={buttonClass}
        title={title}
        aria-label={label}
        aria-haspopup="dialog"
        aria-expanded={open}
        disabled={disabled}
        // Safari does not focus a clicked button, and the sheet returns
        // focus to whatever had it.
        onClick={(event) => {
          event.currentTarget.focus({ preventScroll: true });
          setOpen(true);
        }}
      >
        {trigger}
      </Button>
      {open && (
        <Modal
          title={heading}
          layout="sheet"
          size="narrow"
          className={sheetClass}
          close={() => setOpen(false)}
        >
          <SheetContent>{children}</SheetContent>
        </Modal>
      )}
    </div>
  );
}
