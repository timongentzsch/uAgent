import type { ComponentChildren, ComponentType, JSX } from "preact";
import { failure } from "./types.ts";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { ChevronDown, ChevronRight, X, Check, Copy } from "lucide-preact";
import { markPath } from "./mark.ts";

import { cleanText } from "./display.ts";
export { cleanText };
export async function copyText(text: string) {
  if (navigator.clipboard) return navigator.clipboard.writeText(text);
  // Clipboard API requires HTTPS; tailnet HTTP still supports user-initiated copy.
  const field = document.createElement("textarea");
  field.value = text;
  field.readOnly = true;
  field.style.cssText = "position:fixed;left:-9999px";
  const focused = document.activeElement;
  document.body.append(field);
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
        {status === "Copied!" ? (
          <Check aria-hidden="true" />
        ) : (
          <Copy aria-hidden="true" />
        )}
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
export function Select({
  children,
  className = "",
  ...props
}: JSX.SelectHTMLAttributes<HTMLSelectElement>) {
  return (
    <span class={`select ${className}`}>
      <select {...props}>{children}</select>
      <ChevronDown aria-hidden="true" />
    </span>
  );
}
export function Field({
  label,
  value,
  children,
  help,
}: {
  label: string;
  value?: ComponentChildren;
  children: ComponentChildren;
  help?: string;
}) {
  return (
    <label class="field">
      <span class="field-label">
        {label}
        {value !== undefined && <output>{value}</output>}
      </span>
      {children}
      {help && <small class="muted">{help}</small>}
    </label>
  );
}
export function Toggle({
  label,
  help,
  ...props
}: JSX.InputHTMLAttributes<HTMLInputElement> & {
  label: string;
  help?: string;
}) {
  return (
    <label class="toggle-row">
      <input type="checkbox" {...props} />
      <span>
        <strong>{label}</strong>
        {help && <small class="muted">{help}</small>}
      </span>
    </label>
  );
}
export function Skeleton({
  rows = 3,
  className = "",
  label = "Loading…",
  decorative = false,
  delayMs = 150,
}: {
  rows?: number;
  className?: string;
  label?: string;
  decorative?: boolean;
  delayMs?: number;
}) {
  // Announced loaders wait out fast resolves: a skeleton that lives for
  // a single frame IS the flash. Structural mirrors stay immediate
  // (decorative) because they are the geometry reservation.
  const [visible, setVisible] = useState(decorative || delayMs <= 0);
  useEffect(() => {
    if (decorative || delayMs <= 0) return;
    const timer = setTimeout(() => setVisible(true), delayMs);
    return () => clearTimeout(timer);
  }, [decorative, delayMs]);
  if (!visible) return null;
  return (
    <div
      class={`skeleton ${className}`}
      role={decorative ? undefined : "status"}
      aria-busy={decorative ? undefined : "true"}
      aria-hidden={decorative || undefined}
    >
      {!decorative && <small class="loading-label">{label}</small>}
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
      {retry && (
        <button type="button" onClick={retry}>
          Retry
        </button>
      )}
    </div>
  );
}
export function EventRow({
  title,
  time,
  status,
  icon,
  messageId,
  children,
  onToggle,
}: {
  title: string;
  time?: string;
  status?: string;
  icon: ComponentChildren;
  messageId?: string;
  children: ComponentChildren;
  onToggle?: JSX.GenericEventHandler<HTMLDetailsElement>;
}) {
  return (
    <DisclosureRow
      className="event-row"
      label={title}
      status={status}
      time={time}
      icon={icon}
      messageId={messageId}
      onToggle={onToggle}
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
  onToggle,
  className = "",
  messageId,
  children,
}: {
  label: string;
  status?: string;
  time?: string;
  icon?: ComponentChildren;
  onToggle?: JSX.GenericEventHandler<HTMLDetailsElement>;
  className?: string;
  messageId?: string;
  children: ComponentChildren;
}) {
  const [isOpen, setIsOpen] = useState(false);
  const [mounted, setMounted] = useState(false);
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
          {label}
        </span>
        {status && <small>{status}</small>}
        {time && (
          <time dateTime={time}>
            {new Date(time).toLocaleTimeString([], {
              hour: "2-digit",
              minute: "2-digit",
            })}
          </time>
        )}
      </summary>
      {mounted && <div class="disclosure-body">{children}</div>}
    </details>
  );
}

export function IconButton({
  label,
  children,
  ...props
}: JSX.ButtonHTMLAttributes<HTMLButtonElement> & { label: string }) {
  return (
    <button
      type="button"
      class="quiet icon-button"
      aria-label={label}
      title={label}
      {...props}
    >
      {children}
    </button>
  );
}

export function Modal({
  title,
  children,
  close,
  className = "",
}: {
  title: string;
  children: ComponentChildren;
  close: () => void;
  className?: string;
}) {
  const ref = useRef<HTMLDialogElement>(null);
  const [ready, setReady] = useState(false);
  useLayoutEffect(() => {
    const prior = document.activeElement;
    const dialog = ref.current;
    dialog?.showModal();
    // Paint gate: mount work and the first async data land before the
    // dialog becomes visible, so open never flashes an unsettled box.
    // One frame is layout, the second commits paint.
    let second = 0;
    const first = requestAnimationFrame(() => {
      second = requestAnimationFrame(() => setReady(true));
    });
    return () => {
      cancelAnimationFrame(first);
      cancelAnimationFrame(second);
      dialog?.close();
      if (prior instanceof HTMLElement && prior.isConnected)
        prior.focus({ preventScroll: true });
    };
  }, []);
  return (
    <dialog
      ref={ref}
      class={className}
      data-ready={ready || undefined}
      aria-label={title}
      onCancel={(event) => {
        event.preventDefault();
        close();
      }}
    >
      <header>
        <h2>{title}</h2>
        <IconButton label={`Close ${title.toLowerCase()}`} onClick={close}>
          <X />
        </IconButton>
      </header>
      <div class="dialog-body">{children}</div>
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
  ...props
}: P & {
  load: () => Promise<{ default: ComponentType<P> }>;
  fallback?: ComponentChildren;
}) {
  const [Component, setComponent] = useState<ComponentType<P> | null>(
    () => (modules.get(load) as ComponentType<P> | undefined) || null,
  );
  const [error, setError] = useState<unknown>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    let active = true;
    setError(null);
    preloadDeferred(load)
      .then((component) => {
        if (active) setComponent(() => component);
      })
      .catch((failure) => {
        if (active) setError(failure);
      });
    return () => {
      active = false;
    };
  }, [load, attempt]);
  return Component ? (
    <Component {...(props as P)} />
  ) : error ? (
    <LoadError error={error} retry={() => setAttempt(attempt + 1)} />
  ) : (
    fallback || <Skeleton delayMs={0} />
  );
}
