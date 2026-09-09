import type { ComponentChildren, ComponentType, JSX } from "preact";
import { failure } from "./types.ts";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { ChevronDown, X } from "lucide-preact";
import { markPath } from "./mark.ts";

export const cleanText = (text = "") =>
  text.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, "");
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
export function Skeleton({
  rows = 3,
  className = "",
  label = "Loading…",
  decorative = false,
}: {
  rows?: number;
  className?: string;
  label?: string;
  decorative?: boolean;
}) {
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
  useLayoutEffect(() => {
    const prior = document.activeElement;
    const dialog = ref.current;
    dialog?.showModal();
    return () => {
      dialog?.close();
      if (prior instanceof HTMLElement && prior.isConnected)
        prior.focus({ preventScroll: true });
    };
  }, []);
  return (
    <dialog
      ref={ref}
      class={className}
      aria-label={title}
      onCancel={(event) => {
        event.preventDefault();
        close();
      }}
    >
      <header>
        <h2>{title}</h2>
        <button
          type="button"
          class="quiet icon-button"
          onClick={close}
          aria-label={`Close ${title.toLowerCase()}`}
          title="Close"
        >
          <X />
        </button>
      </header>
      <div class="dialog-body">{children}</div>
    </dialog>
  );
}

const modules = new WeakMap<object, unknown>();

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
    load()
      .then((module) => {
        modules.set(load, module.default);
        if (active) setComponent(() => module.default);
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
    fallback || <Skeleton />
  );
}
