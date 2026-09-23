import type { InstallPrompt, Report } from "./types.ts";

// Single owner for activating a waiting service worker: wake it, then
// reload exactly once when it takes control. Shared by the update banner
// and Settings so the two can never drift apart.
export function applyUpdate(worker: ServiceWorker) {
  navigator.serviceWorker.addEventListener(
    "controllerchange",
    () => location.reload(),
    { once: true },
  );
  worker.postMessage({ type: "ACTIVATE_UPDATE" });
}

export function watchPwa(
  install: (prompt: InstallPrompt) => void,
  update: (worker: ServiceWorker) => void,
  report: Report,
) {
  let active: ServiceWorker | null = null;
  let disposed = false;
  const cleanup: (() => void)[] = [];
  const cache = (entries: PerformanceEntry[]) => {
    if (!active) return;
    const paths = entries
      .map((entry) => new URL(entry.name).pathname)
      .filter((path) => path.startsWith("/assets/"));
    if (paths.length) active.postMessage({ type: "CACHE_RENDERERS", paths });
  };
  const resources = new PerformanceObserver((list) => cache(list.getEntries()));
  resources.observe({ type: "resource", buffered: true });
  const prompted = (event: Event) => {
    event.preventDefault();
    install(event as InstallPrompt);
  };
  addEventListener("beforeinstallprompt", prompted);
  if ("serviceWorker" in navigator && isSecureContext)
    navigator.serviceWorker
      .register("/sw.js", { updateViaCache: "none" })
      .then((registration) => {
        if (disposed) return;
        if (registration.waiting) update(registration.waiting);
        const found = () => {
          const worker = registration.installing;
          if (!worker) return;
          const changed = () => {
            if (
              worker.state === "installed" &&
              navigator.serviceWorker.controller
            )
              update(worker);
          };
          worker.addEventListener("statechange", changed);
          cleanup.push(() =>
            worker.removeEventListener("statechange", changed),
          );
        };
        registration.addEventListener("updatefound", found);
        cleanup.push(() =>
          registration.removeEventListener("updatefound", found),
        );
        // Standalone PWAs can live for days without the browser's own
        // periodic update check firing; check every time the app becomes
        // visible so a waiting worker is actually discovered.
        const refreshRegistration = () => {
          if (document.visibilityState !== "visible") return;
          Promise.resolve(registration.update()).catch(() => {});
        };
        document.addEventListener("visibilitychange", refreshRegistration);
        cleanup.push(() =>
          document.removeEventListener("visibilitychange", refreshRegistration),
        );
        navigator.serviceWorker.ready.then((ready) => {
          if (disposed) return;
          active = ready.active;
          cache(performance.getEntriesByType("resource"));
        });
      })
      .catch((error) => {
        if (!disposed) report(error);
      });
  return () => {
    disposed = true;
    active = null;
    cleanup.forEach((stop) => stop());
    resources.disconnect();
    removeEventListener("beforeinstallprompt", prompted);
  };
}
