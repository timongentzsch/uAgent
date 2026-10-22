import type { ComponentChildren } from "preact";
import { ClipboardPaste, Copy, Keyboard } from "lucide-preact";
import { Button, IconButton } from "../../shared/ui.tsx";
import { coarsePointer } from "../../shared/layout.ts";
import "./browser.css";

// The browser's fixed geometry, shared by its loading state and the live
// panel: the remote screen takes all the space, one bar sits below. State
// changes only change what fills these slots, never their size.
export function BrowserFrame({
  screen,
  bar,
}: {
  screen: ComponentChildren;
  bar: ComponentChildren;
}) {
  return (
    <section class="browser-panel">
      {screen}
      <div class="browser-bar">{bar}</div>
    </section>
  );
}

// Present in every state; disabled rather than removed while watching.
export function BrowserTools({
  disabled,
  keyboard,
  copy,
  paste,
}: {
  disabled: boolean;
  keyboard?: () => void;
  copy?: () => void;
  paste?: () => void;
}) {
  const keepFocus = (event: Event) => event.preventDefault();
  return (
    <>
      {/* Touch devices type through the on-screen keyboard; a mouse and
          keyboard reach Chrome directly. */}
      {coarsePointer() && (
        <IconButton
          label="Keyboard"
          disabled={disabled}
          onPointerDown={keepFocus}
          onClick={keyboard}
        >
          <Keyboard />
        </IconButton>
      )}
      <IconButton label="Copy" disabled={disabled} onClick={copy}>
        <Copy />
      </IconButton>
      <IconButton label="Paste" disabled={disabled} onClick={paste}>
        <ClipboardPaste />
      </IconButton>
    </>
  );
}

export function BrowserLoading() {
  return (
    <BrowserFrame
      screen={
        <div class="browser-screen">
          <p class="browser-overlay" role="status">
            Loading browser…
          </p>
        </div>
      }
      bar={
        <>
          <div class="sheet-control browser-status-menu">
            <Button variant="quiet" class="browser-status" disabled>
              <span>Browser</span>
            </Button>
          </div>
          <BrowserTools disabled />
          <Button variant="primary" class="browser-primary" disabled>
            Take over
          </Button>
        </>
      }
    />
  );
}
