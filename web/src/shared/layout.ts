import { useEffect, useState } from "preact/hooks";
import { defaultZoom } from "./limits.ts";
// The interface zoom in steps, as a browser offers it: choosing one rescales
// once, where a slider rescaled the page under the finger dragging it.
export const zoomSteps = [50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200];
const conversationMeasure = 1040;
export const zoomChanged = "uagent:zoom";

// A touch screen: the on-screen keyboard, no hover.
export const coarsePointer = () => matchMedia("(pointer: coarse)").matches;

// A stored value snaps to the nearest step, so the picker always shows it.
export function normalizeZoom(value: number) {
  const wanted = value || defaultZoom;
  return zoomSteps.reduce((best, step) =>
    Math.abs(step - wanted) < Math.abs(best - wanted) ? step : best,
  );
}

export function applyZoom(value: number) {
  const zoom = normalizeZoom(value);
  document.documentElement.style.setProperty("--zoom", String(zoom / 100));
  document.documentElement.style.setProperty(
    "--conversation-measure",
    `${(conversationMeasure * 100) / zoom}px`,
  );
  dispatchEvent(new Event(zoomChanged));
}

// Defer layout writes out of ResizeObserver delivery. Coalesce a resize burst
// into one frame and cancel pending work when its surface disappears.
export function observeResize(update: () => void, ...elements: Element[]) {
  let frame = 0;
  const observer = new ResizeObserver(() => {
    cancelAnimationFrame(frame);
    frame = requestAnimationFrame(update);
  });
  elements.forEach((element) => observer.observe(element));
  return () => {
    observer.disconnect();
    cancelAnimationFrame(frame);
  };
}

const VIEWPORT_ROUNDING_PX = 1;
let currentViewport: ReturnType<typeof readViewportBounds> | undefined;

function readViewportBounds() {
  const viewport = globalThis.visualViewport;
  const root = document.documentElement;
  const width = Math.min(viewport?.width || innerWidth, root.clientWidth);
  const height = Math.min(viewport?.height || innerHeight, root.clientHeight);
  return {
    left: Math.max(
      0,
      Math.min(viewport?.offsetLeft || 0, root.clientWidth - width),
    ),
    top: Math.max(
      0,
      Math.min(viewport?.offsetTop || 0, root.clientHeight - height),
    ),
    width,
    height,
  };
}

export function viewportBounds(safeArea = false) {
  const bounds = { ...(currentViewport || readViewportBounds()) };
  if (safeArea) {
    // The shell resolves env()/calc() insets into actual padding lengths.
    const style = getComputedStyle(
      document.getElementById("app") || document.body,
    );
    const left = parseFloat(style.paddingLeft) || 0;
    const right = parseFloat(style.paddingRight) || 0;
    const top = parseFloat(style.paddingTop) || 0;
    const bottom = parseFloat(style.paddingBottom) || 0;
    bounds.left += left;
    bounds.top += top;
    bounds.width = Math.max(0, bounds.width - left - right);
    bounds.height = Math.max(0, bounds.height - top - bottom);
  }
  return bounds;
}

// A popover against its anchor: under it, or over it where there is more
// room above and too little below; as tall as that room, inside the safe
// viewport, and lined up with the anchor's start or end edge.
export function placePopover(
  element: HTMLElement,
  anchor: HTMLElement,
  align: "start" | "end",
) {
  const { left: x, top: y, width, height } = viewportBounds(true);
  const target = anchor.getBoundingClientRect();
  const gap =
    parseFloat(getComputedStyle(element).getPropertyValue("row-gap")) || 0;
  const above = Math.max(0, target.top - y - 2 * gap);
  const below = Math.max(0, y + height - target.bottom - 2 * gap);
  const upwards = below < Math.min(element.scrollHeight, above);
  element.style.maxWidth = `${width - 2 * gap}px`;
  element.style.maxHeight = `${Math.max(1, upwards ? above : below)}px`;
  const box = element.getBoundingClientRect();
  const left = align === "end" ? target.right - box.width : target.left;
  element.style.left = `${Math.max(x + gap, Math.min(left, x + width - box.width - gap))}px`;
  element.style.top = `${Math.max(y + gap, Math.min(upwards ? target.top - box.height - gap : target.bottom + gap, y + height - box.height - gap))}px`;
}

// Safari can pan as well as shrink the visible area when the keyboard opens.
// Shells and top-layer surfaces consume the same event-driven geometry.
export function observeViewport(update: () => void) {
  let frame = 0;
  const changed = () => {
    cancelAnimationFrame(frame);
    frame = requestAnimationFrame(update);
  };
  const viewport = globalThis.visualViewport;
  addEventListener("resize", changed);
  addEventListener("scroll", changed);
  addEventListener("pageshow", changed);
  addEventListener("orientationchange", changed);
  screen.orientation?.addEventListener("change", changed);
  document.addEventListener("visibilitychange", changed);
  document.addEventListener("focusin", changed);
  document.addEventListener("focusout", changed);
  viewport?.addEventListener("resize", changed);
  viewport?.addEventListener("scroll", changed);
  update();
  return () => {
    cancelAnimationFrame(frame);
    removeEventListener("resize", changed);
    removeEventListener("scroll", changed);
    removeEventListener("pageshow", changed);
    removeEventListener("orientationchange", changed);
    screen.orientation?.removeEventListener("change", changed);
    document.removeEventListener("visibilitychange", changed);
    document.removeEventListener("focusin", changed);
    document.removeEventListener("focusout", changed);
    viewport?.removeEventListener("resize", changed);
    viewport?.removeEventListener("scroll", changed);
  };
}

// Mobile keyboards shrink the visual viewport without moving layout: a
// focused field can end up under the keyboard even though its surface
// fits the visible window. Reveal it inside its own scroll container
// (dialog body, management editor) — never the transcript (the scroll
// stick owns it) and never the document (which must stay at scroll 0).
// Runs on focus and on every viewport change: real keyboards arrive
// after focus, and mocked viewports (tests) after the fill.
function revealFocusedField() {
  const active = document.activeElement;
  if (!(active instanceof HTMLElement) || active === document.body) return;
  if (active.closest(".transcript, .composer")) return;
  const surface = active.closest("dialog, .management");
  if (!(surface instanceof HTMLElement)) return;
  let scroller: HTMLElement | null = active.parentElement;
  while (scroller && scroller !== surface) {
    if (scroller.scrollHeight > scroller.clientHeight + 1) break;
    scroller = scroller.parentElement;
  }
  if (scroller === surface) {
    scroller = surface.scrollHeight > surface.clientHeight + 1 ? surface : null;
  }
  if (!scroller) return;
  const field = active.getBoundingClientRect();
  const view = scroller.getBoundingClientRect();
  const pad = parseFloat(getComputedStyle(scroller).scrollPaddingTop) || 0;
  if (field.bottom > view.bottom)
    scroller.scrollTop += field.bottom - view.bottom + pad;
  else if (field.top < view.top)
    scroller.scrollTop -= view.top - field.top + pad;
}

export function trackViewport() {
  const standalone = matchMedia("(display-mode: standalone)");
  const stop = observeViewport(() => {
    const root = document.documentElement;
    const viewport = globalThis.visualViewport;
    const bounds = readViewportBounds();
    const editing = document.activeElement?.matches(
      'textarea, input:not([type="range"]):not([type="checkbox"]):not([type="radio"]):not([type="button"]):not([type="submit"]), [contenteditable="true"]',
    );
    // Only a field being typed into holds the keyboard up. iOS can keep
    // reporting the smaller visual viewport after the keyboard closes, until
    // the page scrolls, so a smaller viewport alone is not a keyboard.
    const keyboard =
      !!editing &&
      (viewport?.scale || 1) === 1 &&
      Math.abs((viewport?.width || innerWidth) - root.clientWidth) <=
        VIEWPORT_ROUNDING_PX &&
      bounds.height < root.clientHeight - VIEWPORT_ROUNDING_PX;
    // WebKit may retain landscape visual-viewport dimensions/offsets in a
    // standalone app. At rest the CSS layout viewport is authoritative; keep
    // visual geometry for keyboards and actual browser pinch zoom.
    if (standalone.matches && !keyboard && (viewport?.scale || 1) === 1) {
      Object.assign(bounds, {
        left: 0,
        top: 0,
        width: root.clientWidth,
        height: root.clientHeight,
      });
      if (scrollX || scrollY) scrollTo(0, 0);
    }
    currentViewport = bounds;
    root.toggleAttribute("data-keyboard", keyboard);
    for (const [key, value] of Object.entries(bounds))
      document.documentElement.style.setProperty(
        `--viewport-${key}`,
        `${value}px`,
      );
    // No transcript scrollIntoView here: the transcript hook owns its
    // scroll; stealing it on focus caused jumps. Dialog and management surfaces reveal their focused
    // field inside their own body instead (see revealFocusedField).
    revealFocusedField();
  });
  return () => {
    stop();
    currentViewport = undefined;
  };
}

// Whether a media query matches, kept current.
export function useMedia(query: string) {
  const [matches, setMatches] = useState(() => matchMedia(query).matches);
  useEffect(() => {
    const media = matchMedia(query);
    const update = () => setMatches(media.matches);
    update();
    media.addEventListener("change", update);
    return () => media.removeEventListener("change", update);
  }, [query]);
  return matches;
}

// The width under which the phone layout applies, as the stylesheets have it.
export const usePhone = () => useMedia("(max-width: 600px)");
// The width under which the conversations list moves into a drawer, as the
// stylesheets have it.
export const useCompact = () => useMedia("(max-width: 900px)");

// Animations play unless this device asks for less motion or Settings
// turns them off.
export function applyMotion(motion: string) {
  const media = matchMedia("(prefers-reduced-motion: reduce)");
  const apply = () => {
    document.documentElement.dataset.motion =
      motion === "off" || media.matches ? "off" : "on";
  };
  apply();
  media.addEventListener("change", apply);
  return () => media.removeEventListener("change", apply);
}

export function applyTheme(theme: string) {
  const media = matchMedia("(prefers-color-scheme: dark)");
  const apply = () => {
    const resolved =
      theme === "system" ? (media.matches ? "dark" : "light") : theme;
    document.documentElement.dataset.theme = resolved;
    document.querySelector<HTMLMetaElement>(
      'meta[name="theme-color"]',
    )!.content = resolved === "dark" ? "#000000" : "#ffffff";
  };
  apply();
  media.addEventListener("change", apply);
  return () => media.removeEventListener("change", apply);
}

// Safari still pinches the whole page through its own gesture events, which
// touch-action does not stop; the app shell must never zoom.
export function lockPageZoom() {
  const block = (event: Event) => event.preventDefault();
  for (const kind of ["gesturestart", "gesturechange"])
    document.addEventListener(kind, block, { passive: false });
}
