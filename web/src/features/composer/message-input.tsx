import type { JSX, RefObject } from "preact";
import { useLayoutEffect, useRef } from "preact/hooks";
import { observeResize } from "../../shared/layout.ts";
import { focusFontFloor } from "../../shared/limits.ts";
import { zoomChanged } from "../../shared/size-controls.tsx";

// Safari focuses editable controls without page zoom at a 16px layout font.
// Scale the painted control and compensate its box, so density still applies.
function fitWrapper(element: HTMLTextAreaElement) {
  element.parentElement!.style.height = `${element.getBoundingClientRect().height}px`;
}

function resize(element: HTMLTextAreaElement) {
  const wrapper = element.parentElement!;
  const font = parseFloat(getComputedStyle(wrapper).fontSize);
  const layoutFont = matchMedia("(pointer: coarse)").matches
    ? Math.max(focusFontFloor, font)
    : font;
  const scale = font / layoutFont;
  wrapper.style.setProperty("--input-scale", String(scale));
  // Resolve the floor explicitly: CSS division can round 16px just below
  // Safari's threshold (15.999999px), even with a visually identical result.
  element.style.fontSize = `${layoutFont}px`;
  element.style.height = "0px";
  element.style.height = `${element.scrollHeight}px`;
  fitWrapper(element);
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
    const changed = () => resize(element);
    addEventListener(zoomChanged, changed);
    const stop = observeResize(
      () => {
        if (element.clientWidth === width) {
          fitWrapper(element); // Viewport/keyboard changes can lower max-height.
          return;
        }
        width = element.clientWidth;
        resize(element);
      },
      element.parentElement!,
      element,
    );
    return () => {
      stop();
      removeEventListener(zoomChanged, changed);
    };
  }, []);
  return (
    <span class="message-input">
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
    </span>
  );
}
