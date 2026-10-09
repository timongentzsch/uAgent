// Base styles load with the primitives every surface is built from, so they
// always precede feature styles, however the bundle is split.
import "./style.css";
import {
  Component,
  createContext,
  type ComponentChildren,
  type ComponentType,
  type JSX,
  type Ref,
  type RefObject,
} from "preact";
import { failure } from "./types.ts";
import {
  useContext,
  useEffect,
  useId,
  useLayoutEffect,
  useRef,
  useState,
} from "preact/hooks";
import { ChevronRight, X, Check, Copy } from "lucide-preact";
import { markPath } from "./mark.ts";
import { animateOut, motionMs } from "./motion.ts";
import { useDismiss } from "./dismiss.ts";
import { DataText } from "./placeholder.tsx";
export { Placeholder, DataText, usePlaceholder } from "./placeholder.tsx";

export { Input, Textarea, Select } from "./form-controls.tsx";
import { Input, Select } from "./form-controls.tsx";

import { cleanText } from "./display.ts";
import { useResource } from "./use-resource.ts";
export { cleanText };
import {
  TimePrefsContext,
  formatFullMoment,
  formatMoment,
  useNow,
} from "./time.ts";

// Every timestamp in the app: formatted by the viewer's time preferences,
// with the full moment on hover. `value` is an ISO string or epoch ms.
export function Time({ value }: { value?: string | number }) {
  const prefs = useContext(TimePrefsContext);
  const now = useNow(prefs.style !== "absolute");
  if (value == null || value === "") return null;
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return null;
  return (
    <time dateTime={date.toISOString()} title={formatFullMoment(date, prefs)}>
      <DataText>{formatMoment(date, prefs, now)}</DataText>
    </time>
  );
}
async function copyText(text: string) {
  if (navigator.clipboard) return navigator.clipboard.writeText(text);
  // Clipboard API requires HTTPS; tailnet HTTP still supports user-initiated copy.
  const field = document.createElement("textarea");
  field.value = text;
  field.readOnly = true;
  field.style.cssText = "position:fixed;left:-9999px";
  const focused = document.activeElement;
  (
    (focused instanceof Element ? focused.closest("dialog[open]") : null) ||
    document.body
  ).append(field);
  field.select();
  try {
    if (!document.execCommand("copy"))
      throw new Error("Copy is unavailable in this browser");
  } finally {
    field.remove();
    if (focused instanceof HTMLElement) focused.focus();
  }
}
export function CodeCopy({ text }: { text: string }) {
  const [status, setStatus] = useState("");
  useEffect(() => {
    if (!status) return;
    const timer = setTimeout(() => setStatus(""), 2000);
    return () => clearTimeout(timer);
  }, [status]);
  return (
    <>
      <IconButton
        class="swap-feedback"
        label={status || "Copy code"}
        onClick={async () => {
          try {
            await copyText(text);
            setStatus("Copied!");
          } catch (error) {
            setStatus(failure(error).message);
          }
        }}
      >
        {status === "Copied!" ? <Check /> : <Copy />}
      </IconButton>
      <span class="sr-only" role="status">
        {status}
      </span>
    </>
  );
}
export function Mark({ className = "" }: { className?: string }) {
  return (
    <svg
      class={`mark ${className}`}
      viewBox="0 0 16 16"
      role="img"
      aria-label="uAgent"
    >
      <path fill="currentColor" d={markPath} />
    </svg>
  );
}
export function Field({
  label,
  children,
  help,
}: {
  label: string;
  children: ComponentChildren;
  help?: string;
}) {
  return (
    <label class="field">
      <span class="field-label">{label}</span>
      {children}
      {help && <small class="muted">{help}</small>}
    </label>
  );
}
// A setting that is on or off, as a native switch. Applies on change.
export function Switch({
  label,
  checked,
  onChange,
  disabled,
}: {
  label: string;
  checked: boolean;
  onChange: (checked: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <span class="switch">
      <Input
        type="checkbox"
        role="switch"
        aria-label={label}
        checked={checked}
        disabled={disabled}
        onChange={(event) => onChange(event.currentTarget.checked)}
      />
    </span>
  );
}

// A choice shown as a row's trailing value; still a native select, so a
// phone opens its own picker.
export function ValueSelect(
  props: JSX.SelectHTMLAttributes<HTMLSelectElement>,
) {
  return <Select {...props} class="value-select" />;
}

// The one caption over a group of rows or a section.
export function SectionTitle({ children }: { children: ComponentChildren }) {
  return (
    <h3 class="section-title">
      <DataText>{children}</DataText>
    </h3>
  );
}

// Rows on one inset card, with an optional caption and a help line below.
export function Group({
  title,
  footer,
  children,
}: {
  title?: string;
  footer?: ComponentChildren;
  children: ComponentChildren;
}) {
  return (
    <section class="group" aria-label={title}>
      {title && <SectionTitle>{title}</SectionTitle>}
      <div class="group-rows">{children}</div>
      {footer && <p class="group-footer">{footer}</p>}
    </section>
  );
}

// One setting or destination: a label (and detail line), then its value or
// control. With `onClick` the whole row is the button and shows a chevron.
export function Row({
  label,
  detail,
  children,
  onClick,
  destructive = false,
  current,
  disabled,
  expanded,
}: {
  label: ComponentChildren;
  detail?: ComponentChildren;
  children?: ComponentChildren;
  onClick?: () => void;
  disabled?: boolean;
  destructive?: boolean;
  // The destination shown beside the list (a two-column layout).
  current?: boolean;
  // A disclosure: detail opens in place below the row.
  expanded?: boolean;
}) {
  const body = (
    <>
      <span class="row-text">
        <span class="row-label">
          <DataText>{label}</DataText>
        </span>
        {detail && (
          <small class="row-detail">
            <DataText>{detail}</DataText>
          </small>
        )}
      </span>
      {children && <span class="row-value">{children}</span>}
    </>
  );
  return onClick ? (
    <Button
      variant="quiet"
      class={`row${destructive ? " destructive-row" : ""}`}
      aria-current={current ? "page" : undefined}
      aria-expanded={expanded}
      disabled={disabled}
      onClick={onClick}
    >
      {body}
      <ChevronRight class="row-chevron" />
    </Button>
  ) : (
    <div class="row">{body}</div>
  );
}

// A view with nothing in it yet: what it is for and what to try first. The
// tips are there while it is empty and gone with the first content.
export function Welcome({
  level = 2,
  title,
  tips,
  children,
}: {
  level?: 1 | 2;
  title: string;
  tips: ComponentChildren[];
  children?: ComponentChildren;
}) {
  const Heading = `h${level}` as "h1";
  return (
    <div class="empty">
      <Mark className="cursor-mark" />
      <Heading>{title}</Heading>
      {children}
      <ul class="tips" role="list">
        {tips.map((tip, index) => (
          <li key={index}>{tip}</li>
        ))}
      </ul>
    </div>
  );
}

// Nothing to show yet: one line, and what to do about it.
export function EmptyState({
  children,
  action,
}: {
  children: ComponentChildren;
  action?: ComponentChildren;
}) {
  return (
    <div class="empty-state">
      <p>{children}</p>
      {action}
    </div>
  );
}

// Asks before something that cannot be taken back: what will happen, then
// Cancel and the action, named by what it does.
export function ConfirmModal({
  title,
  children,
  action = title,
  busy,
  error,
  confirm,
  close,
}: {
  title: string;
  children: ComponentChildren;
  action?: string;
  busy?: boolean;
  error?: unknown;
  confirm: () => void;
  close: () => void;
}) {
  return (
    <Modal title={title} close={close}>
      <p>{children}</p>
      {error != null && <LoadError error={error} />}
      <Actions>
        <Button onClick={close}>Cancel</Button>
        <Button variant="destructive" busy={busy} onClick={confirm}>
          {action}
        </Button>
      </Actions>
    </Modal>
  );
}

// A dialog's or form's buttons, trailing and in reading order.
export function Actions({ children }: { children: ComponentChildren }) {
  return <div class="dialog-actions">{children}</div>;
}
// Lines of text whose length is unknown until they load (a tool's full
// output, a thread). Known shapes draw themselves (see <Placeholder>).
export function Skeleton({
  rows = 3,
  label = "Loading…",
  delayMs = 150,
}: {
  rows?: number;
  label?: string;
  delayMs?: number;
}) {
  // A skeleton that lives for a single frame IS the flash: wait out fast
  // resolves.
  const [visible, setVisible] = useState(delayMs <= 0);
  useEffect(() => {
    if (delayMs <= 0) return;
    const timer = setTimeout(() => setVisible(true), delayMs);
    return () => clearTimeout(timer);
  }, [delayMs]);
  if (!visible) return null;
  return (
    <div class="skeleton" role="status" aria-busy="true">
      <small class="loading-label">{label}</small>
      <div aria-hidden="true">
        {Array.from({ length: rows }, (_, index) => (
          <span key={index} />
        ))}
      </div>
    </div>
  );
}

export function Spinner({
  label = "Loading…",
  surface = false,
}: {
  label?: string;
  surface?: boolean;
}) {
  return (
    <div
      class={`loading-indicator${surface ? " loading-surface" : ""}`}
      role="status"
      aria-label={label}
      aria-busy="true"
      aria-live="polite"
    >
      <span class="spinner" aria-hidden="true" />
      <span>{label}</span>
    </div>
  );
}
export function LoadError({
  error,
  retry,
}: {
  error: unknown;
  retry?: () => void;
}) {
  return (
    <div class="load-error">
      <p role="alert">{failure(error).message}</p>
      {retry && <Button onClick={retry}>Retry</Button>}
    </div>
  );
}
// Contains a render failure to its subtree: one malformed event or message
// must not blank the whole interface.
export class ErrorBoundary extends Component<
  { children: ComponentChildren },
  { error?: unknown }
> {
  componentDidCatch(error: unknown) {
    this.setState({ error });
  }
  render() {
    return this.state.error !== undefined ? (
      <LoadError
        error={this.state.error}
        retry={() => this.setState({ error: undefined })}
      />
    ) : (
      this.props.children
    );
  }
}
export function EventRow({
  title,
  time,
  icon,
  messageId,
  children,
}: {
  title: string;
  time?: string;
  icon: ComponentChildren;
  messageId?: string;
  children: ComponentChildren;
}) {
  return (
    <DisclosureRow
      className="event-row"
      label={title}
      time={time}
      icon={icon}
      messageId={messageId}
    >
      <div class="event-body">{children}</div>
    </DisclosureRow>
  );
}

export function DisclosureRow({
  label,
  status,
  time,
  icon,
  marker,
  open = false,
  onToggle,
  className = "",
  messageId,
  children,
}: {
  label: string;
  // Whether it starts open; from then on the person's toggles decide.
  open?: boolean;
  status?: string;
  time?: string;
  icon?: ComponentChildren;
  // Leads the label on its line, e.g. a failure mark and its spoken prefix.
  marker?: ComponentChildren;
  onToggle?: JSX.GenericEventHandler<HTMLDetailsElement>;
  className?: string;
  messageId?: string;
  children: ComponentChildren;
}) {
  const [isOpen, setIsOpen] = useState(open);
  const [mounted, setMounted] = useState(open);
  // Internal state only, so user toggles survive streaming re-renders
  // (never reset to closed) with no post-paint prop sync to lag a frame.
  // The toggle is driven explicitly on click: a focus scroll landing
  // between mousedown and mouseup can move the summary and swallow the
  // native toggle (focus arrives, expansion does not). preventDefault
  // plus explicit state makes every pointer and keyboard toggle land.
  const setOpen = (next: boolean) => {
    setIsOpen(next);
    if (next) setMounted(true);
  };
  return (
    <details
      class={`disclosure-row ${className}`}
      data-status={status}
      data-message-id={messageId}
      open={isOpen}
      onToggle={(event) => {
        // Backstop for non-click toggles (assistive tech driving the
        // element directly): adopt DOM truth so state cannot strand.
        setOpen(event.currentTarget.open);
        onToggle?.(event);
      }}
    >
      <summary
        onClick={(event) => {
          event.preventDefault();
          const next = !isOpen;
          setOpen(next);
          onToggle?.({
            currentTarget: { open: next },
          } as JSX.TargetedEvent<HTMLDetailsElement>);
        }}
      >
        {icon}
        <ChevronRight class="disclosure-chevron" aria-hidden="true" />
        <span class="disclosure-label" title={label}>
          {marker}
          {label}
        </span>
        {status && <small>{status}</small>}
        <Time value={time} />
      </summary>
      {mounted && <div class="disclosure-body">{children}</div>}
    </details>
  );
}

export function Button({
  variant = "secondary",
  size = "default",
  busy = false,
  class: className = "",
  buttonRef,
  children,
  disabled,
  ...props
}: JSX.ButtonHTMLAttributes<HTMLButtonElement> & {
  variant?: "primary" | "secondary" | "quiet" | "destructive";
  size?: "default" | "compact" | "icon";
  busy?: boolean;
  // The element, for a caller that places or focuses against it (a menu).
  buttonRef?: Ref<HTMLButtonElement>;
}) {
  return (
    <button
      type="button"
      {...props}
      ref={buttonRef}
      class={`${variant} ${size === "default" ? "" : `${size}-button`} ${className}`}
      disabled={disabled || busy}
      aria-busy={busy || undefined}
    >
      {busy && <span class="spinner" aria-hidden="true" />}
      {children}
    </button>
  );
}

export function IconButton({
  label,
  children,
  ...props
}: JSX.ButtonHTMLAttributes<HTMLButtonElement> & {
  label: string;
  // Quiet unless it is the surface's main action (Send).
  variant?: "primary" | "quiet";
  buttonRef?: Ref<HTMLButtonElement>;
}) {
  return (
    <Button
      variant="quiet"
      size="icon"
      aria-label={label}
      title={label}
      {...props}
    >
      {children}
    </Button>
  );
}

// What a dialog's header needs, for content that draws its own (a settings
// sheet whose title and back button follow its navigation).
const DialogContext = createContext<{
  title: string;
  titleId: string;
  heading: RefObject<HTMLHeadingElement>;
  close: () => void;
} | null>(null);

// The close of the dialog around the caller: plays its exit, as Close does.
export const useDialogClose = () => useContext(DialogContext)!.close;

// The one dialog header: title, optional leading control (Back) and actions,
// then Close. Modal renders it unless its content renders its own.
export function DialogHeader({
  title,
  leading,
  actions,
}: {
  title: string;
  leading?: ComponentChildren;
  actions?: ComponentChildren;
}) {
  const dialog = useContext(DialogContext)!;
  return (
    <header>
      {leading}
      <h2 id={dialog.titleId} ref={dialog.heading} tabIndex={-1}>
        {title}
      </h2>
      {actions && <span class="dialog-header-actions">{actions}</span>}
      <IconButton
        label={`Close ${dialog.title.toLowerCase()}`}
        onClick={dialog.close}
      >
        <X />
      </IconButton>
    </header>
  );
}

export function Modal({
  title,
  children,
  close,
  className = "",
  size = "compact",
  layout = "content",
  actions,
  header = true,
}: {
  title: string;
  children: ComponentChildren;
  close: () => void;
  className?: string;
  // "narrow": a sheet of a few actions or one control (see SheetButton),
  // which rises from the bottom on a phone instead of filling the screen.
  size?: "narrow" | "compact" | "medium" | "wide" | "browser";
  // "sheet": full height at the right edge (full screen on a phone).
  layout?: "content" | "panel" | "sheet";
  // Header controls beside Close, for actions on the dialog's subject.
  actions?: ComponentChildren;
  // False when the content renders its own DialogHeader and .dialog-body.
  header?: boolean;
}) {
  const ref = useRef<HTMLDialogElement>(null);
  // A tap on the scrim closes every dialog, as it does a native sheet. The
  // scrim targets the dialog element, as does its own padding, so the point
  // decides; a press that began inside (a text selection dragged out) does
  // not count.
  const pressedScrim = useRef(false);
  const onScrim = (event: MouseEvent) => {
    const dialog = ref.current;
    if (!dialog || event.target !== dialog) return false;
    const box = dialog.getBoundingClientRect();
    return (
      event.clientX < box.left ||
      event.clientX > box.right ||
      event.clientY < box.top ||
      event.clientY > box.bottom
    );
  };
  const heading = useRef<HTMLHeadingElement>(null);
  const titleId = useId();
  // Every way the person closes it (Close, Escape, the back gesture) plays
  // the exit first; a parent that unmounts it (a swap) is instant.
  const leaving = useRef(false);
  const mounted = useRef(true);
  const requestClose = () => {
    if (leaving.current) return;
    if (!motionMs("base")) return close();
    leaving.current = true;
    // A parent that replaced this dialog meanwhile owns its state now.
    void animateOut(ref.current).then(() => mounted.current && close());
  };
  useDismiss(true, requestClose);
  useLayoutEffect(() => {
    const prior = document.activeElement;
    const dialog = ref.current;
    dialog?.showModal();
    // Start at the dialog's title instead of highlighting its first action.
    // Tab still reaches Close; native modality and focus restoration remain.
    heading.current?.focus({ preventScroll: true });
    return () => {
      mounted.current = false;
      dialog?.close();
      const target =
        prior instanceof HTMLElement && prior.isConnected
          ? prior
          : document.activeElement;
      const restored =
        target instanceof HTMLDialogElement
          ? target.querySelector<HTMLElement>("header > h2")
          : target;
      if (restored instanceof HTMLElement)
        restored.focus({ preventScroll: true });
    };
  }, []);
  return (
    <dialog
      ref={ref}
      class={className}
      data-size={size}
      data-layout={layout}
      // A content-drawn header's title follows its navigation; the dialog
      // keeps its own name.
      aria-labelledby={header ? titleId : undefined}
      aria-label={header ? undefined : title}
      onCancel={(event) => {
        event.preventDefault();
        requestClose();
      }}
      onPointerDown={(event) => {
        pressedScrim.current = onScrim(event);
      }}
      onClick={(event) => {
        if (pressedScrim.current && onScrim(event)) requestClose();
      }}
    >
      <DialogContext.Provider
        value={{
          title,
          titleId,
          heading,
          close: requestClose,
        }}
      >
        {header ? (
          <>
            <DialogHeader title={title} actions={actions} />
            <div class="dialog-body">{children}</div>
          </>
        ) : (
          children
        )}
      </DialogContext.Provider>
    </dialog>
  );
}

const modules = new WeakMap<object, unknown>();

export async function preloadDeferred<P extends object>(
  load: () => Promise<{ default: ComponentType<P> }>,
) {
  const cached = modules.get(load) as ComponentType<P> | undefined;
  if (cached) return cached;
  const module = await load();
  modules.set(load, module.default);
  return module.default;
}

// The caller owns the surface, so lazy code and data use the same visible shell.
export function Deferred<P extends object>({
  load,
  fallback,
  ownsDialog = false,
  ...props
}: P & {
  load: () => Promise<{ default: ComponentType<P> }>;
  fallback?: ComponentChildren;
  // The whole content of a Modal with header={false}.
  ownsDialog?: boolean;
}) {
  const dialog = useContext(DialogContext);
  const {
    value: Component,
    error,
    retry,
  } = useResource(
    () => preloadDeferred(load),
    [load],
    () => modules.get(load) as ComponentType<P> | undefined,
  );
  return Component ? (
    <Component {...(props as P)} />
  ) : error ? (
    // The content of a headerless dialog: a failed load still shows the
    // dialog's title and Close.
    ownsDialog && dialog ? (
      <>
        <DialogHeader title={dialog.title} />
        <div class="dialog-body">
          <LoadError error={error} retry={retry} />
        </div>
      </>
    ) : (
      <LoadError error={error} retry={retry} />
    )
  ) : fallback === undefined ? (
    <Spinner surface />
  ) : (
    fallback
  );
}
