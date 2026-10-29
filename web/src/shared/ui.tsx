// Base styles load with the primitives every surface is built from, so they
// always precede feature styles, however the bundle is split.
import "./style.css";
import "./focus.ts";
import { copiedFeedbackMs } from "./limits.ts";
import {
  Component,
  createContext,
  type ComponentChildren,
  type ComponentType,
  type FunctionComponent,
  type ButtonHTMLAttributes,
  type Ref,
  type RefObject,
  type SelectHTMLAttributes,
} from "preact";
import { failure, type State } from "./types.ts";
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
import { formatFullMoment, formatMoment, useNow } from "./time.ts";
import { devicePrefs } from "./device-prefs.ts";
import { useStored } from "./storage.ts";

// Whether any prop changed identity: the update check of a view whose
// parent renders more often than its props change.
const propsChanged = (prior: object, next: object) =>
  Object.entries(next).some(
    ([key, value]) => (prior as Record<string, unknown>)[key] !== value,
  );

// `View`, rendered only when a prop changes identity; of a session's
// `state`, only when one of the `fields` it reads does, or with none named
// any field but the transcript's view, which every streamed node replaces.
export function updateChecked<P extends object>(
  View: FunctionComponent<P>,
  fields?: (keyof State)[],
) {
  return class extends Component<P> {
    shouldComponentUpdate(next: P) {
      const { state: before, ...prior } = this.props as { state?: State };
      const { state: after, ...rest } = next as { state?: State };
      if (propsChanged(prior, rest) || !before !== !after) return true;
      const keys = fields || [
        ...(Object.keys({ ...before, ...after }) as (keyof State)[]),
      ];
      return keys.some(
        (key) => key !== "view" && before?.[key] !== after?.[key],
      );
    }
    render(props: P) {
      return <View {...props} />;
    }
  };
}

// Every timestamp in the app: formatted by the viewer's time preferences,
// with the full moment on hover. `value` is an ISO string or epoch ms.
export function Time({ value }: { value?: string | number }) {
  const [prefs] = useStored(devicePrefs.time);
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
    const timer = setTimeout(() => setStatus(""), copiedFeedbackMs);
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
export function ValueSelect(props: SelectHTMLAttributes<HTMLSelectElement>) {
  return <Select {...props} class="value-select" />;
}

// The one caption over a group of rows or a section.
export function SectionTitle({ children }: { children: ComponentChildren }) {
  return (
    <h3 class="section-title caption">
      <DataText>{children}</DataText>
    </h3>
  );
}

// Rows on one inset card, with an optional caption and a help line below.
// `action` acts on the whole group (Add) and ends its caption.
export function Group({
  title,
  action,
  footer,
  children,
}: {
  title?: string;
  action?: ComponentChildren;
  footer?: ComponentChildren;
  children: ComponentChildren;
}) {
  return (
    <section class="group" aria-label={title}>
      {action ? (
        <div class="group-caption">
          <SectionTitle>{title}</SectionTitle>
          {action}
        </div>
      ) : (
        title && <SectionTitle>{title}</SectionTitle>
      )}
      <div class="group-rows divided">{children}</div>
      {footer && <p class="group-footer small">{footer}</p>}
    </section>
  );
}

// One setting or destination: an icon, a label (and detail line), then its
// value or control. With `onClick` the whole row is the button and shows a
// chevron.
export function Row({
  icon,
  label,
  detail,
  children,
  onClick,
  destructive = false,
  current,
  disabled,
  expanded,
}: {
  icon?: ComponentChildren;
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
      {icon && <span class="row-icon">{icon}</span>}
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
      class={`row entry${destructive ? " destructive-row" : ""}`}
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
  // Each a short lead (a key, a word) and what it does.
  tips: [ComponentChildren, ComponentChildren][];
  children?: ComponentChildren;
}) {
  const Heading = `h${level}` as "h1";
  return (
    <div class="empty">
      <Mark className="cursor-mark" />
      <Heading>{title}</Heading>
      {children}
      <dl class="tips">
        {tips.map(([lead, text], index) => (
          <div key={index}>
            <dt>{lead}</dt>
            <dd>{text}</dd>
          </div>
        ))}
      </dl>
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
    <div class="empty-state" role="status">
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
// A loading indicator that lives for a single frame IS the flash: it shows
// once `delayMs` has passed, so fast resolves draw nothing.
function useDelayed(delayMs: number) {
  const [visible, setVisible] = useState(delayMs <= 0);
  useEffect(() => {
    if (delayMs <= 0) return;
    const timer = setTimeout(() => setVisible(true), delayMs);
    return () => clearTimeout(timer);
  }, [delayMs]);
  return visible;
}

// Loading draws by one rule: a view whose shape is known draws itself from
// sample data (see <Placeholder>); text of unknown length draws these lines;
// a <Spinner> is left for an action's progress, or a surface whose shape
// cannot be known, such as one whose code is still on its way.
export function Skeleton({
  rows = 3,
  label = "Loading…",
  delayMs = 150,
}: {
  rows?: number;
  label?: string;
  delayMs?: number;
}) {
  if (!useDelayed(delayMs)) return null;
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
  delayMs = 150,
}: {
  label?: string;
  surface?: boolean;
  delayMs?: number;
}) {
  if (!useDelayed(delayMs)) return null;
  return (
    <div
      class={`loading-indicator small${surface ? " loading-surface" : ""}`}
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
export function Button({
  variant = "secondary",
  size = "default",
  busy = false,
  class: className = "",
  buttonRef,
  children,
  disabled,
  ...props
}: ButtonHTMLAttributes<HTMLButtonElement> & {
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
}: ButtonHTMLAttributes<HTMLButtonElement> & {
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
  heading: RefObject<HTMLHeadingElement | null>;
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
  size = "m",
  layout = "content",
  actions,
  header = true,
}: {
  title: string;
  children: ComponentChildren;
  close: () => void;
  className?: string;
  // Its width, one of the --dialog-* sizes. "s": a sheet of a few actions or
  // one control (see SheetButton), which rises from the bottom on a phone
  // instead of filling the screen.
  size?: "s" | "m" | "l" | "xl";
  // "sheet": full height at the right edge (full screen on a phone).
  // "spotlight": centered high in the window, as tall as its content.
  layout?: "content" | "panel" | "sheet" | "spotlight";
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

async function preloadDeferred<P extends object>(
  load: () => Promise<{ default: ComponentType<P> }>,
) {
  const cached = modules.get(load) as ComponentType<P> | undefined;
  if (cached) return cached;
  const module = await load();
  modules.set(load, module.default);
  return module.default;
}

// Code loaded on first use (diagrams, annotation, the browser viewer); the
// caller owns the surface and its placeholder.
export function Deferred<P extends object>({
  load,
  fallback,
  ownsDialog = false,
  ...props
}: P & {
  load: () => Promise<{ default: ComponentType<P> }>;
  fallback: ComponentChildren;
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
  ) : (
    fallback
  );
}
