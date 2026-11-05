import { compactNow } from "./layout.ts";

// Motion in script uses the same tokens as CSS (style.css :root), so the
// one reduced-motion switch there covers animations started here too.
type Motion = "fast" | "base" | "slow";

export function motionMs(name: Motion) {
  const value = getComputedStyle(document.documentElement)
    .getPropertyValue(`--motion-${name}`)
    .trim();
  // The build may write a token in seconds.
  return parseFloat(value) * (value.endsWith("ms") ? 1 : 1000) || 0;
}

export const motionEase = (name: "out" | "in" | "standard") =>
  getComputedStyle(document.documentElement)
    .getPropertyValue(`--ease-${name}`)
    .trim() || "ease";

// Resolves when an element's finite animations and transitions finish (a
// spinner inside never does), or at once when nothing animates.
export async function settled(element: Element | null) {
  if (!element) return;
  await Promise.allSettled(
    element
      .getAnimations({ subtree: true })
      .filter((item) => item.effect?.getComputedTiming().endTime !== Infinity)
      .map((item) => item.finished),
  );
}

// Plays an element's leave state (`data-leaving`, styled in CSS) and
// resolves once it has played; the caller then removes the element. A
// leaving element is inert, so a second tap cannot land on what is going.
export async function animateOut(element: HTMLElement | null) {
  // Without motion there is nothing to play: close in the same task.
  if (!element || !motionMs("base")) return;
  element.inert = true;
  element.setAttribute("data-leaving", "");
  // Let the style change start its transitions before collecting them.
  await new Promise((resolve) => requestAnimationFrame(resolve));
  await settled(element);
}

let sliding: { run: ViewTransition; to: string } | undefined;

// Runs the change to conversation `to` as a slide of the transcript in from
// the side it came from (style.css names it for the run), where the list is
// a drawer and the browser can; elsewhere the change happens at once. A
// different change that arrives while one slides ends that one.
export function slide(back: boolean, to: string, change: () => void) {
  if (sliding?.to === to) return;
  sliding?.run.skipTransition();
  if (!document.startViewTransition || !motionMs("base") || !compactNow())
    return change();
  const root = document.documentElement;
  root.dataset.slide = back ? "back" : "forward";
  const run = document.startViewTransition(async () => {
    change();
    // The slide carries the messages if they load in a moment; the page keeps
    // the old frame until then. Past that it carries their placeholder.
    const latest = performance.now() + motionMs("slow");
    do await new Promise((resolve) => setTimeout(resolve));
    while (
      to &&
      !document.querySelector(
        `.transcript[data-session="${CSS.escape(to)}"]:not([aria-busy])`,
      ) &&
      performance.now() < latest
    );
  });
  const mine = { run, to };
  sliding = mine;
  // A skipped transition rejects `ready`.
  run.ready.catch(() => {});
  void run.finished.finally(() => {
    if (sliding !== mine) return;
    sliding = undefined;
    delete root.dataset.slide;
  });
}
