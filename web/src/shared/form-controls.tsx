import { createElement, type JSX, type RefObject } from "preact";
import { useLayoutEffect, useRef } from "preact/hooks";
import { ChevronDown } from "lucide-preact";
import { observeResize, zoomChanged } from "./layout.ts";
import { focusFontFloor } from "./limits.ts";

type ControlElement =
  HTMLInputElement | HTMLTextAreaElement | HTMLSelectElement;
type ControlProps =
  | JSX.InputHTMLAttributes<HTMLInputElement>
  | JSX.TextareaHTMLAttributes<HTMLTextAreaElement>
  | JSX.SelectHTMLAttributes<HTMLSelectElement>;
type Sizing = {
  inputRef?: RefObject<ControlElement>;
  grow?: boolean;
  resizeKey?: number;
};

// Keep native editing/pickers and a Safari-safe layout font, while painting
// every field at its inherited UI size. One owner for zoom and box geometry.
function Control({
  tag,
  inputRef,
  grow,
  resizeKey,
  class: className,
  className: alias,
  ...props
}: ControlProps & Sizing & { tag: "input" | "textarea" | "select" }) {
  const local = useRef<ControlElement>(null);
  const ref = inputRef || local;
  const fit = () => {
    const element = ref.current;
    if (!element) return;
    const wrapper = element.parentElement!;
    const font = parseFloat(getComputedStyle(wrapper).fontSize);
    const layoutFont = matchMedia("(pointer: coarse)").matches
      ? Math.max(focusFontFloor, font)
      : font;
    wrapper.style.setProperty("--input-scale", String(font / layoutFont));
    // Explicit pixels avoid division rounding 16px below Safari's threshold.
    element.style.fontSize = `${layoutFont}px`;
    // Closed details still inherit zoom, but have no box to measure yet.
    if (!element.getClientRects().length) return;
    if (grow) {
      element.style.height = "0px";
      element.style.height = `${element.scrollHeight}px`;
    }
    wrapper.style.height = `${element.getBoundingClientRect().height}px`;
  };
  useLayoutEffect(fit, [props.value, resizeKey]);
  useLayoutEffect(() => {
    const element = ref.current;
    if (!element) return;
    let width = element.clientWidth;
    const stop = observeResize(
      () => {
        if (element.clientWidth !== width) {
          width = element.clientWidth;
          fit();
        } else {
          // Native textarea resizing and keyboard height changes keep their size.
          if (element.getClientRects().length)
            element.parentElement!.style.height = `${element.getBoundingClientRect().height}px`;
        }
      },
      element.parentElement!,
      element,
    );
    addEventListener(zoomChanged, fit);
    return () => {
      stop();
      removeEventListener(zoomChanged, fit);
    };
  }, []);
  return (
    <span
      class={`text-control ${tag === "select" ? "select" : ""} ${className || alias || ""}`}
    >
      {createElement(tag, { ...props, ref })}
      {tag === "select" && <ChevronDown aria-hidden="true" />}
    </span>
  );
}

export function Input(props: JSX.InputHTMLAttributes<HTMLInputElement>) {
  // These controls have no editable text and already scale with the UI tokens.
  if (
    [
      "checkbox",
      "radio",
      "range",
      "file",
      "hidden",
      "color",
      "button",
      "submit",
      "reset",
      "image",
    ].includes(String(props.type))
  )
    return <input {...props} />;
  return <Control tag="input" {...props} />;
}

export function Textarea(
  props: JSX.TextareaHTMLAttributes<HTMLTextAreaElement> & Sizing,
) {
  return <Control tag="textarea" {...props} />;
}

export function Select(props: JSX.SelectHTMLAttributes<HTMLSelectElement>) {
  return <Control tag="select" {...props} />;
}
