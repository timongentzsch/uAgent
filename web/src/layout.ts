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

export function viewportBounds(safeArea = false) {
  const viewport = globalThis.visualViewport;
  const bounds = {
    left: viewport?.offsetLeft || 0,
    top: viewport?.offsetTop || 0,
    width: viewport?.width || innerWidth,
    height: viewport?.height || innerHeight,
  };
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
  viewport?.addEventListener("resize", changed);
  viewport?.addEventListener("scroll", changed);
  update();
  return () => {
    cancelAnimationFrame(frame);
    removeEventListener("resize", changed);
    viewport?.removeEventListener("resize", changed);
    viewport?.removeEventListener("scroll", changed);
  };
}

export function trackViewport() {
  let width = 0,
    height = 0;
  return observeViewport(() => {
    // During pinch zoom retain layout coordinates. Refresh them from the
    // layout viewport on rotation instead of leaving stale portrait bounds.
    const zoomed = Math.abs((globalThis.visualViewport?.scale || 1) - 1) > 0.01;
    const bounds = zoomed
      ? {
          left: 0,
          top: 0,
          width: document.documentElement.clientWidth,
          height: document.documentElement.clientHeight,
        }
      : viewportBounds();
    const resized = width !== bounds.width || height !== bounds.height;
    ({ width, height } = bounds);
    for (const [key, value] of Object.entries(bounds))
      document.documentElement.style.setProperty(
        `--viewport-${key}`,
        `${value}px`,
      );
    // Focus can precede the keyboard animation. Reveal a lower form field in
    // its scrolling surface after reflow, without stealing focus or selection.
    const focused = document.activeElement;
    if (
      !zoomed &&
      resized &&
      focused instanceof HTMLElement &&
      focused.matches("input, textarea, [contenteditable=true]")
    ) {
      const box = focused.getBoundingClientRect();
      if (box.top < bounds.top || box.bottom > bounds.top + bounds.height)
        focused.scrollIntoView({
          block: "nearest",
          inline: "nearest",
          behavior: "instant",
        });
    }
  });
}
