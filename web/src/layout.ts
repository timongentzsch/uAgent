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
