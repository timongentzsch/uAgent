import type { JSX, RefObject } from "preact";
import { useLayoutEffect, useRef } from "preact/hooks";
import { observeResize } from "../../shared/layout.ts";

function resize(element: HTMLTextAreaElement) {
  element.style.height = "0px";
  element.style.height = `${element.scrollHeight}px`;
}

// Main and child composers share growth, IME handling and submit semantics.
export default function MessageInput({
  inputRef,
  submit,
  onKeyDown,
  resizeKey,
  ...props
}: JSX.TextareaHTMLAttributes<HTMLTextAreaElement> & {
  inputRef?: RefObject<HTMLTextAreaElement>;
  resizeKey?: number;
  submit: (event: Event) => void;
}) {
  const local = useRef<HTMLTextAreaElement>(null);
  const ref = inputRef || local;
  useLayoutEffect(() => {
    const element = ref.current;
    if (!element) return;
    resize(element);
  }, [props.value, resizeKey]);
  useLayoutEffect(() => {
    const element = ref.current;
    if (!element) return;
    let width = element.clientWidth;
    return observeResize(() => {
      if (element.clientWidth === width) return;
      width = element.clientWidth;
      resize(element);
    }, element.parentElement!);
  }, []);
  return (
    <textarea
      {...props}
      ref={ref}
      onKeyDown={(event) => {
        onKeyDown?.(event);
        if (
          event.defaultPrevented ||
          event.key !== "Enter" ||
          event.shiftKey ||
          event.isComposing ||
          event.keyCode === 229
        )
          return;
        event.preventDefault();
        if (!event.repeat) submit(event);
      }}
    />
  );
}
