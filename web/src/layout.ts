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
  document.addEventListener("focusin", changed);
  document.addEventListener("focusout", changed);
  viewport?.addEventListener("resize", changed);
  viewport?.addEventListener("scroll", changed);
  update();
  return () => {
    cancelAnimationFrame(frame);
    removeEventListener("resize", changed);
    document.removeEventListener("focusin", changed);
    document.removeEventListener("focusout", changed);
    viewport?.removeEventListener("resize", changed);
    viewport?.removeEventListener("scroll", changed);
  };
}

export function trackViewport() {
  const restingHeights = new Map<number, number>();
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
    const viewportWidth = Math.round(bounds.width);
    restingHeights.set(
      viewportWidth,
      Math.max(
        restingHeights.get(viewportWidth) || 0,
        bounds.height,
        document.documentElement.clientHeight,
      ),
    );
    // Focusout commonly arrives before the keyboard finishes closing. Keep the
    // inset suppressed until the visual viewport itself returns to rest.
    const keyboard =
      !zoomed &&
      bounds.height < (restingHeights.get(viewportWidth) || bounds.height) - 1;
    document.documentElement.toggleAttribute("data-keyboard", keyboard);
    for (const [key, value] of Object.entries(bounds))
      document.documentElement.style.setProperty(
        `--viewport-${key}`,
        `${value}px`,
      );
    // No scrollIntoView here: the transcript owns scroll via sentinel follow
    // and native overflow-anchor. Stealing scroll on focus caused jumps.
  });
}

export function observeCompact(changed: (compact: boolean) => void) {
  const media = matchMedia("(max-width: 900px)");
  const update = () => changed(media.matches);
  media.addEventListener("change", update);
  return () => media.removeEventListener("change", update);
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
  localStorage.setItem("uagent-theme", theme);
  media.addEventListener("change", apply);
  return () => media.removeEventListener("change", apply);
}
