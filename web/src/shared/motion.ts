// Motion in script uses the same tokens as CSS (style.css :root), so the
// one reduced-motion switch there covers animations started here too.
type Motion = "fast" | "base" | "slow";

export function motionMs(name: Motion) {
  const value = getComputedStyle(document.documentElement).getPropertyValue(
    `--motion-${name}`,
  );
  return parseFloat(value) || 0;
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
// resolves once it has played.
export async function animateOut(element: Element | null) {
  // Without motion there is nothing to play: close in the same task.
  if (!element || !motionMs("base")) return;
  element.setAttribute("data-leaving", "");
  // Let the style change start its transitions before collecting them.
  await new Promise((resolve) => requestAnimationFrame(resolve));
  await settled(element);
}
