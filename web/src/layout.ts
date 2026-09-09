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

export function viewportBounds() {
  const viewport = globalThis.visualViewport;
  return {
    left: viewport?.offsetLeft || 0,
    top: viewport?.offsetTop || 0,
    width: viewport?.width || innerWidth,
    height: viewport?.height || innerHeight,
  };
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
    // Pinch zoom should magnify and pan the existing layout, not reflow it.
    if (Math.abs((globalThis.visualViewport?.scale || 1) - 1) > 0.01) return;
    const bounds = viewportBounds();
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
