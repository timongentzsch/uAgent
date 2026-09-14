import type { InstallPrompt, Report } from "./types.ts";

export function watchPwa(
  install: (prompt: InstallPrompt) => void,
  update: (worker: ServiceWorker) => void,
  report: Report,
) {
  let active: ServiceWorker | null = null;
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
        if (registration.waiting) update(registration.waiting);
        registration.addEventListener("updatefound", () => {
          const worker = registration.installing;
          worker?.addEventListener("statechange", () => {
            if (
              worker.state === "installed" &&
              navigator.serviceWorker.controller
            )
              update(worker);
          });
        });
        navigator.serviceWorker.ready.then((ready) => {
          active = ready.active;
          cache(performance.getEntriesByType("resource"));
        });
      })
      .catch(report);
  return () => {
    resources.disconnect();
    removeEventListener("beforeinstallprompt", prompted);
  };
}
